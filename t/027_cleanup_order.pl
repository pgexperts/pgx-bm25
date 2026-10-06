use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# VACUUM cleanup order and the merge's lock order (issue #300, part A).
#
# bm25_vacuumcleanup runs four passes, each under the seal/merge singleton. The
# orphan sweep is the only one whose cost scales with the whole index on every
# VACUUM, so it is the hold most likely to be cancelled -- by the user, or, for an
# autovacuum, by an insert's deadlock check after deadlock_timeout. It used to run
# SECOND, so a cancelled sweep also skipped the opportunistic merge (the only
# automatic merge) and the retired reclaim (the only automatic one): an insert-busy
# table whose autovacuum kept being cancelled in the sweep grew segments and
# retired-but-unfreed ranges without bound. Cleanup now runs seal ->
# reclaim_retired -> merge -> sweep.
#
# Scenarios (A) and (B) park a VACUUM at the start of its orphan sweep (pause point
# orphan_sweep_start: singleton held, nothing marked; reached only when the sweep's
# gate lets it run, so each scenario makes sure it will), cancel it there, and check
# that the merge (A) and the retired reclaim (B) had already happened. Pre-fix both
# fail: the sweep ran before either. Scenario (C) parks the cleanup's merge
# (merge_preswap) and cancels it, and checks the retired reclaim had still run --
# which pins the reclaim-before-merge half of the order.
#
# Scenario (D) is the lock-order check (XCUT-11). bm25_merge() seals first, and the
# seal drops its heap lock on the way out, so the merge proper starts holding no heap
# lock. It used to take the singleton and THEN open the heap, so a DDL lock that
# queued on the heap in that gap left the merge waiting for the heap while holding
# the singleton every document-adding insert needs. The merge is parked between the
# two (merge_start), an ACCESS EXCLUSIVE heap lock is taken, the merge is released,
# and the test asserts that while it waits for the heap it holds no page lock on the
# index. Pre-fix it holds the singleton (a granted 'page' lock) while it waits.

my $node = PostgreSQL::Test::Cluster->new('cleanup_order');
$node->init;
# No autovacuum: a worker running cleanup on these tables would merge or reclaim
# behind the test's back, and one vacuuming a table first would make the manual
# VACUUM queue on the relation lock and never reach its pause point.
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N) parks pause point N; N is the
# point's position in bm25_handler.c's pause-point table.
my $pause_key = 1651323445;
my %point = (
	merge_preswap      => 4,
	orphan_sweep_start => 8,
	merge_start        => 9);

my $holder = $node->background_psql('postgres');
$holder->set_query_timer_restart();

sub hold
{
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{$_[0]})");
}

sub release
{
	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{$_[0]})");
}

# A session that survives an ERROR (on_error_stop => 0): the cancelled VACUUM is
# expected to fail, and its error is checked rather than killing the harness.
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

# Wait for the running statement to end; return its stderr ('' on success).
sub finish
{
	my ($s) = @_;
	$s->query('SELECT 1');
	my $err = $s->{stderr};
	$s->{stderr} = '';
	return $err;
}

# Wait until backend $app is blocked on a lock and return the locktype.
sub wait_blocked
{
	my ($app) = @_;
	my $q = qq{SELECT l.locktype FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		 WHERE a.application_name = '$app' AND NOT l.granted LIMIT 1};
	$node->poll_query_until('postgres', "SELECT ($q) IS NOT NULL")
	  or die "timed out waiting for $app to block";
	return $node->safe_psql('postgres', $q);
}

sub cancel
{
	my ($app) = @_;
	$node->safe_psql('postgres',
		"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = '$app'");
}

sub nsegs
{
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM bm25_debug_segcat('$_[0]_idx')");
}

sub nretired
{
	return $node->safe_psql('postgres',
		"SELECT bm25_debug_retired_count('$_[0]_idx')");
}

# Drive the cluster horizon past every retire_xid stamped so far. retire_xid is a
# ReadNextFullTransactionId() -- a FUTURE xid -- so xid-assigning transactions must
# commit after it before GlobalVisCheckRemovableFullXid clears it (20_merge_reclaim).
# The idle background sessions hold no snapshot.
sub burn
{
	$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;
}

# 20_merge_reclaim's fixture: three segments (CREATE INDEX + two seals), then half
# the rows deleted. Every segment is ~50% tombstoned after the next VACUUM's
# bulkdelete, so that VACUUM's opportunistic merge fires on the tombstone trigger
# and collapses them into one, retiring the three inputs.
sub setup
{
	my ($t) = @_;
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text);
		INSERT INTO $t SELECT g, 'common database storage term' || (g % 4)
		  FROM generate_series(1, 400) g;
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
		INSERT INTO $t SELECT g, 'common database storage' FROM generate_series(401, 600) g;
		SELECT bm25_seal('${t}_idx');
		INSERT INTO $t SELECT g, 'common database storage' FROM generate_series(601, 800) g;
		SELECT bm25_seal('${t}_idx');
		DELETE FROM $t WHERE id % 2 = 0;
	});
}

my $vacuum = 'VACUUM (INDEX_CLEANUP ON, PARALLEL 0)';
my $cancelled = qr/canceling statement due to user request/;

# Index still answers correctly after a cancelled cleanup.
sub check_rows
{
	my ($t, $label) = @_;
	my $n = $node->safe_psql('postgres', qq{
		SET enable_seqscan = off;
		SELECT count(*) FROM $t WHERE body @@@ 'common'});
	my $heap = $node->safe_psql('postgres', "SELECT count(*) FROM $t");
	is($n, $heap, "$label: every live row is still found through the index");
}

# ---- (A) the merge runs before a cancelled sweep --------------------------------
{
	setup('ta');
	is(nsegs('ta'), 3, '(A) three segments before VACUUM');
	hold('orphan_sweep_start');
	my $v = session('vaca', 'orphan_sweep_start');
	start_async($v, "$vacuum ta");
	is(wait_blocked('vaca'), 'advisory', '(A) VACUUM parked at the start of its orphan sweep');
	cancel('vaca');
	like(finish($v), $cancelled, '(A) the VACUUM was cancelled in the sweep');
	release('orphan_sweep_start');

	is(nsegs('ta'), 1, '(A) the cleanup merge ran before the cancelled sweep');
	cmp_ok(nretired('ta'), '>', 0, '(A) and retired its three inputs');
	check_rows('ta', '(A)');
	$v->quit;
}

# ---- (B) the retired reclaim runs before a cancelled sweep -----------------------
{
	setup('tb');
	# One full VACUUM: the merge collapses the segments and retires the inputs
	# behind a retire_xid nothing has passed yet, so they stay on the list.
	$node->safe_psql('postgres', "$vacuum tb");
	is(nsegs('tb'), 1, '(B) the first VACUUM merged');
	cmp_ok(nretired('tb'), '>', 0, '(B) and left retired ranges behind the horizon');
	burn();
	# The orphan sweep is gated on evidence that orphans can exist (issue #300), and
	# that first VACUUM's own sweep cleared it, so this VACUUM would skip the sweep and
	# never reach the pause point. A seal that "dies" after its build (orphan pages,
	# bracket left open) puts the evidence back.
	$node->safe_psql('postgres', qq{
		INSERT INTO tb VALUES (1001, 'common orphan build');
		SELECT bm25_debug_seal_unpublished('tb_idx');
	});
	is($node->safe_psql('postgres',
		"SELECT ops_begun <> ops_done FROM bm25_debug_sweep_evidence('tb_idx')"),
		't', '(B) the next VACUUM has an orphan sweep to run');

	hold('orphan_sweep_start');
	my $v = session('vacb', 'orphan_sweep_start');
	start_async($v, "$vacuum tb");
	is(wait_blocked('vacb'), 'advisory', '(B) VACUUM parked at the start of its orphan sweep');
	cancel('vacb');
	like(finish($v), $cancelled, '(B) the VACUUM was cancelled in the sweep');
	release('orphan_sweep_start');

	is(nretired('tb'), 0, '(B) the retired ranges were reclaimed before the cancelled sweep');
	check_rows('tb', '(B)');
	$v->quit;
}

# ---- (C) the retired reclaim runs before a cancelled merge -----------------------
{
	setup('tc');
	$node->safe_psql('postgres', "$vacuum tc");
	is(nsegs('tc'), 1, '(C) the first VACUUM merged');
	cmp_ok(nretired('tc'), '>', 0, '(C) and left retired ranges behind the horizon');
	# Arm a second merge: a second segment, and a quarter of the merged segment's
	# rows deleted (the tombstone trigger is 15%).
	$node->safe_psql('postgres', qq{
		INSERT INTO tc SELECT g, 'common database storage' FROM generate_series(801, 900) g;
		SELECT bm25_seal('tc_idx');
		DELETE FROM tc WHERE id % 4 = 1 AND id <= 800;
	});
	burn();

	hold('merge_preswap');
	my $v = session('vacc', 'merge_preswap');
	start_async($v, "$vacuum tc");
	is(wait_blocked('vacc'), 'advisory', '(C) VACUUM parked inside its cleanup merge');
	cancel('vacc');
	like(finish($v), $cancelled, '(C) the VACUUM was cancelled in the merge');
	release('merge_preswap');

	is(nsegs('tc'), 2, '(C) the cancelled merge swapped nothing');
	is(nretired('tc'), 0, '(C) the retired ranges were reclaimed before the cancelled merge');
	check_rows('tc', '(C)');
	$v->quit;
}

# ---- (D) bm25_merge takes the heap lock before the singleton (XCUT-11) -----------
{
	$node->safe_psql('postgres', qq{
		CREATE TABLE td (id int, body text);
		CREATE INDEX td_idx ON td USING bm25_native (body);
		INSERT INTO td SELECT g, 'common w' || g FROM generate_series(1, 100) g;
	});
	hold('merge_start');
	my $m = session('merged', 'merge_start');
	start_async($m, "SELECT bm25_merge('td_idx')");
	is(wait_blocked('merged'), 'advisory',
		'(D) bm25_merge parked after its seal, before the merge takes any lock');

	# The seal released its heap lock, so this is granted at once.
	my $ddl = $node->background_psql('postgres');
	$ddl->set_query_timer_restart();
	$ddl->query_safe('BEGIN; LOCK TABLE td IN ACCESS EXCLUSIVE MODE');

	release('merge_start');
	is(wait_blocked('merged'), 'relation', '(D) the merge now waits for the heap lock');
	my $held = $node->safe_psql('postgres', qq{
		SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		 WHERE a.application_name = 'merged' AND l.locktype = 'page'
		   AND l.relation = 'td_idx'::regclass AND l.granted});
	is($held, 0, '(D) and holds no page lock (the singleton) on the index while it waits');

	$ddl->query_safe('ROLLBACK');
	is(finish($m), '', '(D) the merge completed once the heap lock was released');
	is($node->safe_psql('postgres', 'SELECT pending_ndocs FROM bm25_stats(\'td_idx\')'),
		0, '(D) and its seal drained the pending list');
	$m->quit;
	$ddl->quit;
}

$holder->quit;
$node->stop;
bm25_check_logs($node);
done_testing();
