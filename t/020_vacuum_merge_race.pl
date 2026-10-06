use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# VACUUM's tombstone pass vs. a concurrent seal or merge (#239, #241).
#
# bm25_bulkdelete snapshots the segment catalog, tombstones each segment's dead
# docs, then sweeps the pending list. A seal or merge that swaps the catalog inside
# that pass loses tombstones:
#
#   (i)   a seal drains dead pending docs into a segment the snapshot never saw,
#         before the pending sweep reaches them;
#   (ii)  a merge that retires segments mid-loop leaves VACUUM tombstoning a
#         retired segment: "segment header N not found in catalog";
#   (iii) a merge that replays its inputs before the loop and swaps after it
#         republishes every doc the loop tombstoned in them -- VACUUM succeeds,
#         frees the heap line pointers, and once they are reused a query returns
#         unrelated rows (xs_recheck = false);
#   (iv)  a swap that copies the catalog before the loop and flips after it
#         overwrites a surviving segment's live_ndocs and the metapage ndocs.
#
# The fix holds the seal/merge singleton in ShareLock mode across the whole pass,
# so the seal or merge must WAIT for VACUUM instead of interleaving with it. Each
# scenario parks one backend at the exact step with bm25_native.debug_pause (a
# pause point that waits while this test holds a matching advisory lock), starts
# the other, asserts it is blocked on the index's page lock, releases, and then
# checks that every count agrees with the heap.
#
# Against the pre-fix locking every scenario fails on its data assertions, not only
# on the lock-wait assertion: the pause points park the SAME steps in both trees, so
# the pre-fix tree runs the losing interleaving to completion.

my $node = PostgreSQL::Test::Cluster->new('vacuum_merge_race');
$node->init;
# No autovacuum: a worker vacuuming the table first would make the manual VACUUM
# queue on the relation lock and never reach its pause point.
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N) parks pause point N; the order is
# the pause-point table in bm25_handler.c.
my $pause_key = 1651323445;
my %point = (
	bulkdelete_start    => 1,
	bulkdelete_segment  => 2,
	bulkdelete_pending  => 3,
	merge_preswap       => 4,
	swap_after_snapshot => 5);

# The holder is an idle session between statements, so its advisory locks stay
# held until it explicitly unlocks them; the parked backends wait on it.
my $holder = $node->background_psql('postgres');
# Per-query timeout, not one budget for the whole file: this session and the ones
# session() makes live across many queries, and the cassert+UBSan leg runs slowly.
$holder->set_query_timer_restart();

sub hold
{
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{$_[0]})");
}

sub release
{
	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{$_[0]})");
}

# A session that survives an ERROR (on_error_stop => 0), so a pre-fix VACUUM that
# fails is reported by the assertion below rather than killing the harness.
sub session
{
	my ($app, $pause) = @_;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->set_query_timer_restart();
	$s->query_safe("SET application_name = '$app'");
	$s->query_safe("SET bm25_native.debug_pause = '$pause'") if defined $pause;
	return $s;
}

sub start_async
{
	my ($s, $sql) = @_;
	$s->query_until(qr/started/, "\\echo started\n$sql;\n");
}

# Wait for the statement to end and return its stderr ('' on success). The probe
# queues behind the running statement in psql's input, so it returns only once
# that statement has finished.
sub finish
{
	my ($s) = @_;
	$s->query('SELECT 1');
	my $err = $s->{stderr};
	$s->{stderr} = '';
	return $err;
}

# Wait until backend $app is blocked on a lock (returns the locktype) or has
# finished the statement starting with $stmt (returns 'done'). Matching the
# statement text keeps the idle state BEFORE the statement arrives from counting.
sub wait_state
{
	my ($app, $stmt) = @_;
	my $q = qq{SELECT coalesce(
		(SELECT l.locktype FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		  WHERE a.application_name = '$app' AND NOT l.granted LIMIT 1),
		(SELECT 'done' FROM pg_stat_activity
		  WHERE application_name = '$app' AND state = 'idle'
		    AND query LIKE '$stmt%'))};
	$node->poll_query_until('postgres', "SELECT ($q) IS NOT NULL")
	  or die "timed out waiting for $app";
	return $node->safe_psql('postgres', $q);
}

# $nseg segments of 100 docs (layer 3: four of them are a merge rung), an optional
# 20-doc segment (layer 2, which the merge leaves alone), and $npending unsealed
# docs. Every doc contains 'common'. The ctids of the rows about to be deleted are
# kept so the line-pointer-reuse check can prove reuse actually happened.
sub setup
{
	my ($t, $nseg, $small, $npending, $dead) = @_;
	my $sql = qq{
		SET bm25_native.seal_threshold = 4000000;
		CREATE TABLE $t (id int, body text);
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
	};
	my $next = 1;
	for (1 .. $nseg)
	{
		my $last = $next + 99;
		$sql .= "INSERT INTO $t SELECT g, 'common w' || g FROM generate_series($next, $last) g;"
		  . "SELECT bm25_seal('${t}_idx');";
		$next = $last + 1;
	}
	if ($small)
	{
		my $last = $next + 19;
		$sql .= "INSERT INTO $t SELECT g, 'common w' || g FROM generate_series($next, $last) g;"
		  . "SELECT bm25_seal('${t}_idx');";
		$next = $last + 1;
	}
	if ($npending)
	{
		my $last = $next + $npending - 1;
		$sql .= "INSERT INTO $t SELECT g, 'common w' || g FROM generate_series($next, $last) g;";
	}
	$sql .= qq{
		CREATE TABLE ${t}_dead AS SELECT ctid AS c FROM $t WHERE $dead;
		DELETE FROM $t WHERE $dead;
	};
	$node->safe_psql('postgres', $sql);
}

sub nsegs
{
	my ($t) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM bm25_debug_segcat('${t}_idx')");
}

# Every count the index keeps must agree with the heap, and a query must not
# return a row that does not match it once the vacuumed line pointers are reused.
sub check_consistent
{
	my ($t, $label, $expect) = @_;
	my $heap = $node->safe_psql('postgres', "SELECT count(*) FROM $t");
	my $live = $node->safe_psql('postgres',
		"SELECT sum(live_ndocs) FROM bm25_debug_segcat('${t}_idx')");
	my $meta = $node->safe_psql('postgres',
		"SELECT ndocs FROM bm25_debug_global_stats('${t}_idx')");
	my $rank = $node->safe_psql('postgres',
		"SELECT count(*) FROM bm25_debug_rank('${t}_idx', 'common')");
	my $pending = $node->safe_psql('postgres',
		"SELECT pending_ndocs FROM bm25_stats('${t}_idx')");

	is($heap, $expect, "$label: the DELETE left the expected rows");
	is($pending, 0, "$label: pending list fully sealed");
	is($live, $heap, "$label: sum of catalog live_ndocs equals the heap count");
	is($meta, $heap, "$label: metapage ndocs equals the heap count");
	is($rank, $heap, "$label: live bits (debug_rank) equal the heap count");

	$node->safe_psql('postgres',
		"INSERT INTO $t SELECT g, 'unrelated text' FROM generate_series(100001, 100400) g");
	my $reused = $node->safe_psql('postgres',
		"SELECT count(*) FROM $t x JOIN ${t}_dead d ON x.ctid = d.c");
	cmp_ok($reused, '>', 0, "$label: vacuumed line pointers were reused");
	my $wrong = $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM $t WHERE body @@@ 'common' AND body NOT LIKE 'common%'});
	is($wrong, 0, "$label: no reused line pointer is returned for a term it lacks");
}

my $vacuum = 'VACUUM (INDEX_CLEANUP ON, PARALLEL 0)';

# ---- (i) pending sweep vs. seal -------------------------------------------------
{
	setup('t1', 2, 0, 50, 'id % 10 = 0');
	hold('bulkdelete_pending');
	my $v = session('vac1', 'bulkdelete_pending');
	start_async($v, "$vacuum t1");
	is(wait_state('vac1', 'VACUUM'), 'advisory',
		'(i) VACUUM parked before its pending sweep');

	my $m = session('seal1');
	start_async($m, "SELECT bm25_seal('t1_idx')");
	is(wait_state('seal1', 'SELECT bm25_seal'), 'page',
		'(i) the seal waits on the singleton instead of draining under VACUUM');

	release('bulkdelete_pending');
	is(finish($v), '', '(i) VACUUM succeeded');
	is(finish($m), '', '(i) seal succeeded');
	check_consistent('t1', '(i)', 225);
	$v->quit;
	$m->quit;
}

# ---- (ii) merge swap between two segments' tombstones (#239 form a) --------------
{
	setup('t2', 4, 0, 0, 'id % 10 = 0');
	hold('bulkdelete_segment');
	my $v = session('vac2', 'bulkdelete_segment');
	start_async($v, "$vacuum t2");
	is(wait_state('vac2', 'VACUUM'), 'advisory',
		'(ii) VACUUM parked after its first segment');

	my $m = session('merge2');
	start_async($m, "SELECT bm25_merge('t2_idx')");
	is(wait_state('merge2', 'SELECT bm25_merge'), 'page',
		'(ii) the merge waits on the singleton instead of retiring segments mid-loop');

	release('bulkdelete_segment');
	is(finish($v), '', '(ii) VACUUM succeeded');
	is(finish($m), '', '(ii) merge succeeded');
	is(nsegs('t2'), 1, '(ii) the four segments were merged');
	check_consistent('t2', '(ii)', 360);
	$v->quit;
	$m->quit;
}

# ---- (iii) merge replays before the loop, swaps after it (#239 form b) -----------
{
	setup('t3', 4, 0, 0, 'id % 10 = 0');
	hold('bulkdelete_start');
	hold('merge_preswap');
	my $v = session('vac3', 'bulkdelete_start');
	start_async($v, "$vacuum t3");
	is(wait_state('vac3', 'VACUUM'), 'advisory',
		'(iii) VACUUM parked after its catalog snapshot');

	my $m = session('merge3', 'merge_preswap');
	start_async($m, "SELECT bm25_merge('t3_idx')");
	is(wait_state('merge3', 'SELECT bm25_merge'), 'page',
		'(iii) the merge waits on the singleton instead of replaying its inputs');

	# Let VACUUM run until it finishes or blocks, THEN let the merge swap. Pre-fix,
	# that is exactly the losing order: every tombstone lands on an input the merge
	# already replayed. (Pre-fix VACUUM then blocks in its pending sweep behind the
	# parked merge; post-fix it finishes, or blocks in its own cleanup seal.)
	release('bulkdelete_start');
	my $vs = wait_state('vac3', 'VACUUM');
	ok($vs eq 'done' || $vs eq 'page', "(iii) VACUUM ran its tombstone loop ($vs)");
	release('merge_preswap');
	is(finish($v), '', '(iii) VACUUM succeeded');
	is(finish($m), '', '(iii) merge succeeded');
	is(nsegs('t3'), 1, '(iii) the four segments were merged');
	check_consistent('t3', '(iii)', 360);
	$v->quit;
	$m->quit;
}

# ---- (iv) swap copies the catalog before the loop, flips after it (#241) --------
# Deletes only in the 20-doc segment the merge leaves alone, so the only thing that
# can go wrong is its catalog counters: its live bits are never touched by the swap.
{
	setup('t4', 4, 1, 0, 'id IN (410, 420)');
	hold('bulkdelete_start');
	hold('swap_after_snapshot');
	my $v = session('vac4', 'bulkdelete_start');
	start_async($v, "$vacuum t4");
	is(wait_state('vac4', 'VACUUM'), 'advisory',
		'(iv) VACUUM parked after its catalog snapshot');

	my $m = session('merge4', 'swap_after_snapshot');
	start_async($m, "SELECT bm25_merge('t4_idx')");
	is(wait_state('merge4', 'SELECT bm25_merge'), 'page',
		'(iv) the merge waits on the singleton instead of copying the catalog');

	release('bulkdelete_start');
	my $vs = wait_state('vac4', 'VACUUM');
	ok($vs eq 'done' || $vs eq 'page', "(iv) VACUUM ran its tombstone loop ($vs)");
	release('swap_after_snapshot');
	is(finish($v), '', '(iv) VACUUM succeeded');
	is(finish($m), '', '(iv) merge succeeded');
	is(nsegs('t4'), 2, '(iv) the four large segments were merged, the small one kept');
	check_consistent('t4', '(iv)', 418);
	$v->quit;
	$m->quit;
}

$holder->quit;
$node->stop;
bm25_check_logs($node);
done_testing();
