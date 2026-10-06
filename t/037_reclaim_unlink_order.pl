use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# bm25_reclaim_retired drops a retired range from its descriptor BEFORE freeing the
# range, on the path that then unlinks the emptied descriptor page too (issue #50,
# H11; ADR 0031).
#
# Freed pages are reusable at once (their retire_xid has cleared the horizon, and the
# pending-append allocator does not take the singleton), and Generic WAL page writes
# are not undone by abort. So a reclaim that freed a range while its descriptor still
# listed it, then failed before dropping the entry, left a live entry naming pages
# another structure may already own; the next pass would walk that structure's links
# and free it. The fix compacts first on every path: an interruption then leaves an
# empty-but-linked descriptor and some unfreed pages, which the orphan sweep recovers.
#
# The suites that exercise this function -- t/029 (C) above all -- drive the head
# descriptor, which is never unlinked, and the pre-fix code already compacted there:
# reverting #50 leaves t/029, t/030 and t/031 green. This drives the unlink path. Two
# merges each prepend a descriptor page; the second merge's own reclaim (bm25_merge
# runs one after its swap) finds its new head not yet reclaimable -- its entries'
# retire_xid is the next xid as of the swap, which no horizon has passed -- and the
# older page wholly reclaimable, so that page is the one it compacts and then
# unlinks. The reclaim is parked at reclaim_retired_compacted and cancelled there,
# the interruption #50 is about.
#
# Against the reverted order the merge never parks: the unlink path frees and splices
# without compacting. The test reports that directly instead of waiting out a poll.
#
# Not witnessed here (residual, #309): the #36 lock order (metapage, then LIVE, then
# SEGCAT, in bm25_livedocs_clear and the seal publish). Its failure is an LWLock
# deadlock inside buffer-content-lock windows, where no pause point may sit (callers
# of bm25_debug_pause_point hold no buffer lock), so a witness needs a new lever.

my $node = PostgreSQL::Test::Cluster->new('reclaim_unlink_order');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

use constant PAGE_SEGCAT  => 1 << 2;
use constant PAGE_DELETED => 1 << 9;

my $pause_key = 1651323445;
my $point_compacted = 12;    # reclaim_retired_compacted

my $t = 'ru';
$node->safe_psql('postgres', qq{
	CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO $t SELECT g, 'base ' || g FROM generate_series(1, 20) g;
	CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
});

# Live segment header pages: SEGCAT-kind, not DELETED, with a segment generation.
my $hdr_q = qq{SELECT coalesce(string_agg(b::text, ','), '') FROM generate_series(1,
	bm25_debug_npages('${t}_idx')::int - 1) b
	WHERE (bm25_debug_page_flags('${t}_idx', b) & (@{[PAGE_SEGCAT]} | @{[PAGE_DELETED]})) = @{[PAGE_SEGCAT]}
	  AND bm25_debug_page_seg_gen('${t}_idx', b) > 0};
sub headers { return map { $_ => 1 } grep { length } split /,/, $node->safe_psql('postgres', $hdr_q); }
sub deleted
{
	my ($blk) = @_;
	return ($node->safe_psql('postgres', "SELECT bm25_debug_page_flags('${t}_idx', $blk)")
		& PAGE_DELETED) != 0;
}
sub retired
{
	return $node->safe_psql('postgres',
		"SELECT bm25_debug_retired_count('${t}_idx') || '/' || bm25_debug_retired_pages('${t}_idx')");
}

# Four same-layer segments (10 docs each); returns their header blocks.
my $next_id = 21;
sub four_segments
{
	my $label = shift;
	my %before = headers();
	for (1 .. 4)
	{
		my $hi = $next_id + 9;
		$node->safe_psql('postgres', qq{
			INSERT INTO $t SELECT g, '$label ' || g FROM generate_series($next_id, $hi) g;
			SELECT bm25_seal('${t}_idx');});
		$next_id = $hi + 1;
	}
	my %after = headers();
	return grep { !$before{$_} } sort { $a <=> $b } keys %after;
}

# ---- first merge: one descriptor page, not yet reclaimable ---------------------
my @first = four_segments('first');
is(scalar @first, 4, 'four segments sealed for the first merge');
$node->safe_psql('postgres', "SELECT bm25_merge('${t}_idx')");
is(retired(), '4/1', 'the first merge retired its four inputs on one descriptor page');
# Let the first merge's retire_xid fall behind every snapshot.
$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;

# ---- second merge: parked after compacting the older page, then cancelled --------
my @second = four_segments('second');
is(scalar @second, 4, 'four segments sealed for the second merge');

my $holder = $node->background_psql('postgres');
$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point_compacted)");
my $m = $node->background_psql('postgres', on_error_stop => 0);
$m->query_safe("SET application_name = 'mrg'");
$m->query_safe("SET bm25_native.debug_pause = 'reclaim_retired_compacted'");
$m->query_until(qr/started/, "\\echo started\nSELECT bm25_merge('${t}_idx');\n");

# Parked at the pause point, or finished without reaching it. "Finished" is the
# session idle with the merge as its last statement: idle alone would also match the
# moment before the merge starts.
my $state_q = q{SELECT CASE
	WHEN EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
	             WHERE a.application_name = 'mrg' AND l.locktype = 'advisory' AND NOT l.granted)
	  THEN 'parked'
	WHEN EXISTS (SELECT 1 FROM pg_stat_activity
	             WHERE application_name = 'mrg' AND state LIKE 'idle%'
	               AND query LIKE 'SELECT bm25_merge(%')
	  THEN 'finished'
	END};
$node->poll_query_until('postgres', "SELECT ($state_q) IS NOT NULL")
  or die 'the second merge neither parked nor finished';
my $state = $node->safe_psql('postgres', $state_q);
is($state, 'parked',
	'the second merge compacted the older descriptor on the unlink path before freeing')
  or die 'the unlink path freed its ranges without compacting first (#50 order reverted)';

# The parked state: the older page lists nothing, is still linked, and none of the
# ranges it listed is freed yet.
is(retired(), '4/2', 'parked: the older page is empty but still linked; the new head lists four');
is(scalar(grep { deleted($_) } @first), 0, 'parked: no range the older page listed is freed yet');

$node->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = 'mrg'");
$m->query('SELECT 1');
like($m->{stderr}, qr/canceling statement due to user request/,
	'the merge was cancelled between compacting and freeing');
$m->quit;
$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point_compacted)");
$holder->quit;

# The safe failure mode: no entry names a page that was freed, and the freeing that
# did not happen is a leak, not an alias.
is(retired(), '4/2', 'after the cancel: the empty descriptor stays linked, nothing re-listed');
is(scalar(grep { deleted($_) } @first), 0, 'after the cancel: the dropped ranges leaked, unfreed');
is(scalar(grep { deleted($_) } @second), 0, 'after the cancel: the head page\'s ranges untouched');

# Recovery: VACUUM reclaims the head page's entries (their xid is past the horizon
# now), unlinks the empty page, and the orphan sweep frees the leaked ranges.
$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;
$node->safe_psql('postgres', "VACUUM $t");
is(retired(), '0/1', 'VACUUM drained the head page and unlinked the empty one');
my @still = grep { !deleted($_) } (@first, @second);
is(scalar @still, 0, 'every range of both merges is freed') or diag("unfreed: @still");
is($node->safe_psql('postgres', qq{SET enable_seqscan = off;
	SELECT (SELECT count(*) FROM $t WHERE body @@@ 'base') || ',' ||
	       (SELECT count(*) FROM $t WHERE body @@@ 'first') || ',' ||
	       (SELECT count(*) FROM $t WHERE body @@@ 'second')}), '20,40,40',
	'the index still finds every row');

my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP:|PANIC:|terminated by signal/, 'no assertion failure or crash in the server log');

$node->stop;
bm25_check_logs($node);
done_testing();
