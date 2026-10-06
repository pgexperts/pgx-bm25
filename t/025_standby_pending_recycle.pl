use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# A hot-standby scan must not read a recycled pending page as live data (#291).
#
# A scan captures pending_head once (bm25_scan_snapshot) and walks the chain from it
# later, unlocked between pages. On the primary the drained chain's pages are
# reusable only once their retire_xid is past the horizon, which the scan's own xmin
# holds back. A standby query holds nothing back when hot_standby_feedback is off,
# and Generic WAL carries no conflict horizon, so the primary can seal, recycle the
# old chain and re-initialize its pages while a standby scan is between two of them.
# Before the fix the scan then read the reused page as part of its chain: a page
# reused as a NEW pending page sent it down the new chain and the old chain's rows
# went missing with no error; a page reused as a segment page failed a healthy index
# with XX002 "not a pending page".
#
# The fix stamps every pending page with its chain's epoch, drawn from next_gen when
# the chain starts, and the scan rejects a page whose epoch is at or above the
# next_gen it captured, with the retryable 40001. Both rounds below park a standby
# @@@ scan after the first page of its chain (pause point scan_pending_page), make
# the primary reuse the page it reads next, check that the reuse really happened,
# and release the scan, which must fail with 40001 rather than return a short count
# or report corruption.

my $pause_key = 1651323445;     # BM25_DEBUG_PAUSE_LOCKKEY
my $scan_pending_page = 7;      # position in bm25_handler.c's pause-point table

# Flag bits from bm25_format.h.
my $page_pending = 2;
my $page_deleted = 512;

my $primary = PostgreSQL::Test::Cluster->new('pend_recycle_primary');
$primary->init(allows_streaming => 1);
# No autovacuum: its seals and reclaims would make page reuse depend on timing.
$primary->append_conf('postgresql.conf', "autovacuum = off\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# An empty index, so every row goes to the pending list, and four sealed 100-doc
# segments: one merge rung for round A.
my $setup = qq{
	CREATE TABLE docs (id int PRIMARY KEY, body text);
	CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
	SET bm25_native.seal_threshold = 4000000;
};
for my $i (0 .. 3)
{
	my $lo = $i * 100 + 1;
	my $hi = $lo + 99;
	$setup .= "INSERT INTO docs SELECT g, 'seg w' || g FROM generate_series($lo, $hi) g;"
	  . "SELECT bm25_seal('docs_bm25');";
}
$primary->safe_psql('postgres', $setup);

$primary->backup('pend_recycle_backup');
my $standby = PostgreSQL::Test::Cluster->new('pend_recycle_standby');
$standby->init_from_backup($primary, 'pend_recycle_backup', has_streaming => 1);
# The setting this defect needs. It is the default, stated so the test cannot be
# defeated by a changed default.
$standby->append_conf('postgresql.conf', "hot_standby_feedback = off\n");
$standby->start;

sub catchup
{
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
}

sub flags
{
	my ($blk) = @_;
	return $primary->safe_psql('postgres',
		"SELECT bm25_debug_page_flags('docs_bm25', $blk)");
}

# Append $n pending docs carrying $word, no seal; returns the chain's first two blocks.
sub append_chain
{
	my ($word, $lo, $n) = @_;
	my $hi = $lo + $n - 1;
	$primary->safe_psql('postgres', qq{
		SET bm25_native.seal_threshold = 4000000;
		INSERT INTO docs SELECT g, '$word w' || g FROM generate_series($lo, $hi) g});
	my $b0 = $primary->safe_psql('postgres', "SELECT bm25_debug_pending_nth_page('docs_bm25', 0)");
	my $b1 = $primary->safe_psql('postgres', "SELECT bm25_debug_pending_nth_page('docs_bm25', 1)");
	my $b3 = $primary->safe_psql('postgres', "SELECT bm25_debug_pending_nth_page('docs_bm25', 3)");
	isnt($b3, '', "the $word chain spans at least four pages");
	return ($b0, $b1);
}

# Seal the chain and move the primary's horizon past the drained pages' retire_xid
# (bm25_pending_truncate stamps ReadNextFullTransactionId()). Each txid_current()
# runs in its own transaction and consumes an xid.
sub seal_and_pass_horizon
{
	$primary->safe_psql('postgres', "SELECT bm25_seal('docs_bm25')");
	$primary->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 3;
}

my $holder = $standby->background_psql('postgres');
$holder->set_query_timer_restart();

# Park a standby @@@ scan for $word between the first and second pages of its
# chain. Returns the session; the statement is still running.
sub park_scan
{
	my ($word) = @_;
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $scan_pending_page)");

	my $s = $standby->background_psql('postgres', on_error_stop => 0);
	$s->set_query_timer_restart();
	$s->query_safe("SET application_name = 'scan_$word'");
	$s->query_safe('\set VERBOSITY verbose');
	$s->query_safe('SET enable_seqscan = off');
	$s->query_safe("SET bm25_native.debug_pause = 'scan_pending_page'");
	$s->query_until(qr/started/,
		"\\echo started\nSELECT count(*) FROM docs WHERE body @@@ '$word';\n");

	my $q = qq{SELECT coalesce(
		(SELECT l.locktype FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		  WHERE a.application_name = 'scan_$word' AND NOT l.granted LIMIT 1),
		(SELECT 'done' FROM pg_stat_activity
		  WHERE application_name = 'scan_$word' AND state = 'idle'
		    AND query LIKE 'SELECT count%'))};
	$standby->poll_query_until('postgres', "SELECT ($q) IS NOT NULL")
	  or die "timed out waiting for scan_$word";
	is($standby->safe_psql('postgres', $q), 'advisory',
		"the standby $word scan is parked inside its pending walk");
	return $s;
}

# Release the parked scan; return (stdout, stderr) of its statement.
sub release_scan
{
	my ($s) = @_;
	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $scan_pending_page)");
	# The probe queues behind the scan, so it returns once the scan has ended.
	my $out = $s->query("SELECT 'end'");
	my $err = $s->{stderr};
	$s->{stderr} = '';
	$s->quit;
	$out =~ s/\n?end$//;
	return ($out, $err);
}

sub standby_count
{
	my ($word) = @_;
	return $standby->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM docs WHERE body @@@ '$word'});
}

# ---- Round A: the next page is reused as a SEGMENT page ----
#
# The seal drains the alpha chain and frees its pages; the merge of the four
# 100-doc segments then allocates its output from the free space map, which now
# holds exactly those pages.
my ($a0, $a1) = append_chain('alpha', 1001, 400);
catchup();
is(standby_count('alpha'), 400, 'round A baseline: the standby sees every alpha row');

my $scan = park_scan('alpha');
seal_and_pass_horizon();
$primary->safe_psql('postgres', "SELECT bm25_merge('docs_bm25')");
my $fa = flags($a1);
ok(($fa & ($page_pending | $page_deleted)) == 0 && $fa != 0,
	"round A: the scan's next block $a1 was reused as a segment page (flags $fa)");
catchup();

my ($out, $err) = release_scan($scan);
like($err, qr/ERROR:\s+40001: bm25: pending list recycled concurrently/,
	'round A: the parked scan fails with the retryable 40001, not XX002');
is($out, '', 'round A: and returns no rows');
is(standby_count('alpha'), 400, 'round A: a fresh standby scan sees every alpha row');

# ---- Round B: the next page is reused as a NEW PENDING page ----
#
# This is the silent variant the issue reproduced: the re-append's new chain
# reuses the drained pages, and a scan that follows the reused page walks the new
# chain instead of the old one.
my ($b0, $b1) = append_chain('delta', 2001, 400);
catchup();
is(standby_count('delta'), 400, 'round B baseline: the standby sees every delta row');

$scan = park_scan('delta');
seal_and_pass_horizon();
$primary->safe_psql('postgres', qq{
	SET bm25_native.seal_threshold = 4000000;
	INSERT INTO docs SELECT g, 'beta w' || g FROM generate_series(3001, 3600) g});
my $fb = flags($b1);
ok(($fb & $page_pending) != 0 && ($fb & $page_deleted) == 0,
	"round B: the scan's next block $b1 was reused as a live pending page (flags $fb)");
catchup();

($out, $err) = release_scan($scan);
like($err, qr/ERROR:\s+40001: bm25: pending list recycled concurrently/,
	'round B: the parked scan fails with the retryable 40001');
is($out, '', 'round B: and returns no short count (the silent pre-fix answer)');
is(standby_count('delta'), 400, 'round B: a fresh standby scan sees every delta row');
is(standby_count('beta'), 600, 'round B: and every beta row of the new chain');

# The primary agrees: nothing was lost on disk.
is( $primary->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM docs WHERE body @@@ 'alpha delta beta seg'}),
	1800, 'the primary sees every row');

$holder->quit;
$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
