use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# INSERT's key-config check vs. a merge, a VACUUM and page reuse (#270).
#
# bm25_insert checks every row's key_field configuration against the index's first
# segment before appending it (ADR 0067), holding no seal/merge singleton -- since
# #292, only for an index with no key-identity stamp (built before the stamp existed),
# which this test emulates with bm25_debug_clear_keystamp. It used to
# get that segment from bm25_segcat_read, which releases the metapage after reading
# segcat_root and only then walks the catalog. A catalog root orphaned by a merge has
# no retire_xid, so the next VACUUM's orphan sweep frees it for immediate reuse. An
# insert that stalled between the root read and the walk while a merge, a VACUUM and
# an allocation all ran then read a page of another kind and failed a healthy INSERT
# with XX002 "segment page is not of the expected kind".
#
# The fix reads catalog entry 0 under the metapage SHARE (bm25_segcat_first_entry),
# so the root cannot be orphaned while it is read. The insert_keycheck pause point
# parks the INSERT just after that read; this test then merges, vacuums, and inserts
# from another session until the old catalog root has been reused as another kind
# of page, and releases the INSERT, which must succeed. The segment header it reads
# next belongs to a segment the merge retired; the parked insert's own snapshot
# holds the retired pages' horizon, so that read is safe after the release.
#
# What this pins, stated exactly. The protected window, root read to entry copy, runs
# under a buffer content lock, where no pause can sit, so on the fixed build this
# test covers the read AFTER the window: the reuse leaves the INSERT unharmed. It does
# not by itself catch a return to the unlocked walk, because a pause placed after that
# walk would also come too late. The A/B for #270 kept this test and moved the pause
# into bm25_segcat_read, between its metapage read and its walk: that build failed
# here with XX002 on the reused root, the fixed build passes.

my $node = PostgreSQL::Test::Cluster->new('insert_keycheck_race');
$node->init;
# No autovacuum: an autovacuum of the table would make the sequence below depend on
# its timing.
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N) parks pause point N; the order is
# the pause-point table in bm25_handler.c.
my $pause_key = 1651323445;
my $insert_keycheck = 6;

# Flag bits from bm25_format.h.
my $page_segcat  = 4;
my $page_deleted = 512;

my $holder = $node->background_psql('postgres');
$holder->set_query_timer_restart();

# Four 100-doc segments: one merge rung, so bm25_merge folds them into one and
# orphans the catalog chain that listed them.
my $setup = qq{
	SET bm25_native.seal_threshold = 4000000;
	CREATE TABLE k (id int, body text);
	CREATE INDEX k_idx ON k USING bm25_native (body);
};
for my $i (0 .. 3)
{
	my $lo = $i * 100 + 1;
	my $hi = $lo + 99;
	$setup .= "INSERT INTO k SELECT g, 'common w' || g FROM generate_series($lo, $hi) g;"
	  . "SELECT bm25_seal('k_idx');";
}
$node->safe_psql('postgres', $setup);
is($node->safe_psql('postgres', "SELECT count(*) FROM bm25_debug_segcat('k_idx')"),
	4, 'four segments before the merge');

# #292: an index this binary builds carries a key-identity stamp, and INSERT checks
# that instead of reading the catalog, so the race below exists only for an index a
# pre-#292 binary built. Clear the stamp to make k_idx one; otherwise the pause
# point is never reached and the parked-INSERT check below fails.
is($node->safe_psql('postgres', "SELECT bm25_debug_clear_keystamp('k_idx')"),
	't', 'key stamp cleared: INSERT takes the segment-0 fallback');

sub flags
{
	my ($blk) = @_;
	return $node->safe_psql('postgres', "SELECT bm25_debug_page_flags('k_idx', $blk)");
}

# Every block carrying the SEGCAT kind bit: the catalog chain plus the four segment
# headers, which are SEGCAT pages too.
my $segcat_before = $node->safe_psql('postgres', qq{
	SELECT string_agg(b::text, ',' ORDER BY b)
	  FROM generate_series(1, (pg_relation_size('k_idx')
	                          / current_setting('block_size')::int)::int - 1) b
	 WHERE bm25_debug_page_flags('k_idx', b) & $page_segcat <> 0});

$holder->query_safe("SELECT pg_advisory_lock($pause_key, $insert_keycheck)");

my $ins = $node->background_psql('postgres', on_error_stop => 0);
$ins->set_query_timer_restart();
$ins->query_safe("SET application_name = 'ins'");
$ins->query_safe("SET bm25_native.debug_pause = 'insert_keycheck'");
$ins->query_until(qr/started/,
	"\\echo started\nINSERT INTO k VALUES (100001, 'common parked');\n");

# Wait until the INSERT is parked on the advisory lock, or has finished (which
# would mean the pause point never fired).
my $state_q = qq{SELECT coalesce(
	(SELECT l.locktype FROM pg_locks l JOIN pg_stat_activity a USING (pid)
	  WHERE a.application_name = 'ins' AND NOT l.granted LIMIT 1),
	(SELECT 'done' FROM pg_stat_activity
	  WHERE application_name = 'ins' AND state = 'idle' AND query LIKE 'INSERT%'))};
$node->poll_query_until('postgres', "SELECT ($state_q) IS NOT NULL")
  or die 'timed out waiting for the INSERT';
is($node->safe_psql('postgres', $state_q), 'advisory',
	'the INSERT is parked inside its key-config check');

# Merge: flips segcat_root to a new chain, orphaning the old one. Then VACUUM: its
# orphan sweep frees the old chain with no horizon. The merged-away segments' own
# pages go to the retired list instead, gated on the parked INSERT's snapshot.
$node->safe_psql('postgres', "SELECT bm25_merge('k_idx')");
is($node->safe_psql('postgres', "SELECT count(*) FROM bm25_debug_segcat('k_idx')"),
	1, 'the merge folded the four segments into one');
$node->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON, PARALLEL 0) k');

# The pages that were SEGCAT before and are now freed are the old catalog chain:
# the old segment headers are retired, not freed, while the INSERT's snapshot lives.
my @freed = grep { (flags($_) & $page_deleted) != 0 } split /,/, $segcat_before;
is(scalar(@freed), 1, 'VACUUM freed exactly one old catalog page (the old root)');
my $old_root = $freed[0];

# Insert from another session until an allocation hands the old root out again.
# Pending-list pages come from the same allocator, and a page freed with no
# retire_xid is reusable at once.
my $reused = 0;
for my $i (1 .. 200)
{
	my $lo = 200000 + $i * 20;
	my $hi = $lo + 19;
	$node->safe_psql('postgres',
		"INSERT INTO k SELECT g, 'filler w' || g FROM generate_series($lo, $hi) g");
	my $f = flags($old_root);
	if (($f & $page_deleted) == 0 && $f != 0)
	{
		$reused = $f;
		last;
	}
}
ok($reused != 0, "the old catalog root (block $old_root) was reused");
is($reused & $page_segcat, 0, "it was reused as a non-catalog page (flags $reused)");

$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $insert_keycheck)");

# The probe queues behind the INSERT, so it returns once the INSERT has ended.
$ins->query('SELECT 1');
my $err = $ins->{stderr};
is($err, '', 'the parked INSERT succeeded');

is( $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM k WHERE body @@@ 'parked'}),
	1, 'the parked row is indexed');
is( $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM k WHERE body @@@ 'common'}),
	401, 'every common row is found');

$ins->quit;
$holder->quit;
$node->stop;
bm25_check_logs($node);
done_testing();
