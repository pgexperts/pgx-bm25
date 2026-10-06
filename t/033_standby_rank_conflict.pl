use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# A recovery conflict during a ranked scan on a hot standby must cancel the
# statement, not terminate the session (#307 SCAN-02; the standby-conflict test of
# #309 CI-12).
#
# Core resolves a recovery conflict with an ERROR (SQLSTATE 40001, "canceling
# statement due to conflict with recovery") only when the backend is not inside a
# subtransaction; inside one it raises FATAL, since a subtransaction could catch the
# ERROR. bm25_scan_build_ranking used to run every ranking build in an internal
# subtransaction, to retry its own reuse-safety 40001, so a conflict delivered at any
# of the build's CHECK_FOR_INTERRUPTS killed the client's connection. In recovery the
# build now runs once with no subtransaction; the primary keeps the retry.
#
# Every part parks a ranking build at the pause point rank_build_attempt (one hit per
# attempt, inside the wrapper's subtransaction when there is one):
#   PART ONE   standby: the primary takes ACCESS EXCLUSIVE on the table while a
#              standby ranked scan is parked, with max_standby_streaming_delay = 0.
#              The session must get the 40001 ERROR and stay usable.
#   PART TWO   primary: with the pending tail stamped so every build fails with the
#              reuse 40001, a ranked scan must make a second attempt (the retry the
#              bypass must not remove off-standby).
#   PART THREE standby: the same stamped index fails after ONE attempt (no retry
#              in recovery, so the reuse 40001 reaches the client).
# PARTS TWO and THREE count attempts with two lock holders: A holds the pause lock,
# the scan queues behind A, then B queues behind the scan. When A lets go, the scan's
# ShareLock is granted ahead of B, the scan releases it at once, and B takes the lock;
# a second attempt then parks behind B, and a scan with no retry ends instead.
#
# Two more parts need no pause point. They cover every caller of the wrapper on the
# standby's no-subtransaction branch, on their own tables (cdocs, nopos), which the
# PART TWO stamp on docs_bm25 cannot reach:
#   PART FOUR  each caller returns on the standby what it returns on the primary:
#              the over-pull tail rebuild (wand_top_k = 3 under LIMIT 200, so the
#              forced exhaustive rebuild runs), the delegated build of a bitmap
#              phrase @@@ (bm25_load_if_needed), the filtered branch of a scan with
#              two @@@ keys (nwhere > 0), and the bm25_debug_rank SRF.
#   PART FIVE  an ordinary ERROR raised inside the build on the standby (D7: a phrase
#              over a position-less index) leaves the session usable: the same
#              connection then runs ranked and delegated queries, with no FATAL and
#              no leak warning. The standby log is checked for traps, panics,
#              crashed backends and leak reports at the end.

my $pause_key = 1651323445;     # BM25_DEBUG_PAUSE_LOCKKEY
my $rank_build_attempt = 15;    # position in bm25_handler.c's pause-point table

my $primary = PostgreSQL::Test::Cluster->new('rank_conflict_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "autovacuum = off\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# A sealed segment from the build, then a pending chain (PART TWO stamps its tail).
$primary->safe_psql('postgres', qq{
	CREATE TABLE docs (id int PRIMARY KEY, body text);
	INSERT INTO docs SELECT g, 'alpha w' || g FROM generate_series(1, 2000) g;
	CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
	SET bm25_native.seal_threshold = 4000000;
	INSERT INTO docs SELECT g, 'alpha p' || g FROM generate_series(2001, 2200) g;
});
# PARTS FOUR and FIVE: a sealed segment plus pending rows, and a position-less index.
$primary->safe_psql('postgres', qq{
	CREATE TABLE cdocs (id int PRIMARY KEY, body text);
	INSERT INTO cdocs SELECT g, 'alpha w' || (g % 50) || ' beta gamma'
	  FROM generate_series(1, 3000) g;
	CREATE INDEX cdocs_bm25 ON cdocs USING bm25_native (body);
	SET bm25_native.seal_threshold = 4000000;
	INSERT INTO cdocs SELECT g, 'alpha beta p' || g FROM generate_series(3001, 3100) g;
	CREATE TABLE nopos (id int PRIMARY KEY, body text);
	INSERT INTO nopos SELECT g, 'alpha beta' FROM generate_series(1, 100) g;
	CREATE INDEX nopos_bm25 ON nopos USING bm25_native (body)
	  WITH (store_positions = false);
});
isnt($primary->safe_psql('postgres', "SELECT bm25_debug_pending_nth_page('docs_bm25', 0)"),
	'', 'the index has a pending chain');

$primary->backup('rank_conflict_backup');
my $standby = PostgreSQL::Test::Cluster->new('rank_conflict_standby');
$standby->init_from_backup($primary, 'rank_conflict_backup', has_streaming => 1);
# Cancel conflicting standby queries at once rather than after the 30 s default.
$standby->append_conf('postgresql.conf', "max_standby_streaming_delay = 0\n");
$standby->start;

sub catchup
{
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
}
catchup();

my $ranked = "SELECT id FROM docs WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 5";

# Where is the session named $app? 'waiting' = parked at rank_build_attempt,
# 'idle' = its ranked statement ended (the text test keeps the idle moment between
# the session's SETs and that statement from counting), 'gone' = its backend
# exited, else 'running'.
sub state_sql
{
	my ($app) = @_;
	return qq{SELECT CASE
		WHEN NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE application_name = '$app')
			THEN 'gone'
		WHEN EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		              WHERE a.application_name = '$app' AND l.locktype = 'advisory'
		                AND l.objid = $rank_build_attempt AND l.objsubid = 2
		                AND NOT l.granted)
			THEN 'waiting'
		WHEN EXISTS (SELECT 1 FROM pg_stat_activity
		              WHERE application_name = '$app' AND state = 'idle'
		                AND query LIKE '%ORDER BY body &@@%')
			THEN 'idle'
		ELSE 'running' END};
}

# Poll until $app is in one of @states, plus any extra SQL condition; return the state.
sub wait_state
{
	my ($node, $app, $extra, @states) = @_;
	my $in = join(',', map { "'$_'" } @states);
	my $cond = "(" . state_sql($app) . ") IN ($in)";
	$cond .= " AND ($extra)" if defined $extra;
	$node->poll_query_until('postgres', "SELECT $cond")
	  or die "timed out waiting for $app to reach one of $in";
	return $node->safe_psql('postgres', state_sql($app));
}

# Start $ranked in a new session named $app on $node, with the pause point armed; the
# caller already holds the pause lock. Returns once the scan is parked.
sub park_scan
{
	my ($node, $app) = @_;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->set_query_timer_restart();
	$s->query_safe("SET application_name = '$app'");
	$s->query_safe('\set VERBOSITY verbose');
	$s->query_safe('SET enable_seqscan = off');
	$s->query_safe("SET bm25_native.debug_pause = 'rank_build_attempt'");
	$s->query_until(qr/started/, "\\echo started\n$ranked;\n");
	is(wait_state($node, $app, undef, 'waiting', 'idle', 'gone'), 'waiting',
		"$app: the ranked scan is parked inside its ranking build");
	return $s;
}

# Holder sessions for the pause lock (key2 = rank_build_attempt).
sub holder
{
	my ($node, $app) = @_;
	my $h = $node->background_psql('postgres');
	$h->set_query_timer_restart();
	$h->query_safe("SET application_name = '$app'");
	return $h;
}
my $lock   = "SELECT pg_advisory_lock($pause_key, $rank_build_attempt)";
my $unlock = "SELECT pg_advisory_unlock($pause_key, $rank_build_attempt)";

# ---- PART ONE: a recovery conflict cancels the statement, not the session ----
{
	my $a = holder($standby, 'hold_a1');
	$a->query_safe($lock);
	my $s = park_scan($standby, 'conflict_scan');

	# The standby replays this ACCESS EXCLUSIVE lock against the parked scan's
	# AccessShareLock on docs, and with a zero delay cancels the scan at once.
	$primary->safe_psql('postgres',
		'BEGIN; LOCK TABLE docs IN ACCESS EXCLUSIVE MODE; COMMIT;');
	catchup();

	my $st = wait_state($standby, 'conflict_scan', undef, 'idle', 'gone');
	is($st, 'idle',
		'the parked standby session survives the recovery conflict (pre-fix: FATAL)');
	$a->query_safe($unlock);
	if ($st eq 'idle')
	{
		my $out = $s->query("SELECT 'alive'");
		is($out, 'alive', 'and the same connection still runs statements');
		like($s->{stderr},
			qr/ERROR:\s+40001: canceling statement due to conflict with recovery/,
			'the conflict surfaced as the ordinary 40001 statement cancel');
		like($s->{stderr}, qr/relation lock/, 'and it was the lock conflict');
		unlike($s->{stderr}, qr/FATAL/, 'with no FATAL');
		$s->{stderr} = '';
		is($s->query("SELECT count(*) FROM ($ranked) r"), '5',
			'a ranked scan on the same connection works afterwards');
		$s->quit;
	}
	else
	{
		eval { $s->quit };
		diag("standby session stderr: $s->{stderr}");
	}
	$a->quit;
}

# ---- Stamp the pending tail so every ranking build raises the reuse 40001 ----
# The epoch rule rejects a pending page whose epoch is at or above the scan's
# captured next_gen; UINT32_MAX is above any next_gen this index will reach.
$primary->safe_psql('postgres',
	"SELECT bm25_debug_set_pending_tail_epoch('docs_bm25', 4294967295)");
catchup();
my ($rc, $o, $e) = $primary->psql('postgres', "SET enable_seqscan = off; $ranked");
like($e, qr/pending list recycled concurrently/,
	'the stamped index fails every ranking build with the reuse error');

# Park $app on $node behind holder A, queue holder B behind it, let A go, and return
# the scan's state once B holds the lock: 'waiting' = it came back for a second
# attempt, 'idle' = it ended after one.
sub count_attempts
{
	my ($node, $tag) = @_;
	my $app = "scan_$tag";
	my $a = holder($node, "hold_a_$tag");
	my $b = holder($node, "hold_b_$tag");
	$a->query_safe($lock);
	my $s = park_scan($node, $app);

	$b->query_until(qr/queued/, "\\echo queued\n$lock;\n");
	$node->poll_query_until('postgres', qq{
		SELECT count(*) = 2 FROM pg_locks
		 WHERE locktype = 'advisory' AND objid = $rank_build_attempt
		   AND objsubid = 2 AND NOT granted})
	  or die "timed out waiting for holder B to queue behind $app";
	$a->query_safe($unlock);

	my $b_holds = qq{EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE a.application_name = 'hold_b_$tag' AND l.locktype = 'advisory'
		  AND l.objid = $rank_build_attempt AND l.granted)};
	my $st = wait_state($node, $app, $b_holds, 'waiting', 'idle', 'gone');

	$b->query_safe($unlock);
	wait_state($node, $app, undef, 'idle', 'gone');
	my $out = $s->query("SELECT 'end'");
	my $err = $s->{stderr};
	$s->quit;
	$a->quit;
	$b->quit;
	return ($st, $err);
}

# ---- PART TWO: the primary still retries ----
my ($st, $err) = count_attempts($primary, 'primary');
is($st, 'waiting', 'primary: the failed build is retried (a second attempt parks)');
like($err, qr/ERROR:\s+40001: bm25: pending list recycled concurrently/,
	'primary: exhausting the retries surfaces the serialization failure');

# ---- PART THREE: the standby does not ----
($st, $err) = count_attempts($standby, 'standby');
is($st, 'idle', 'standby: the failed build is not retried (pre-fix: a second attempt)');
like($err, qr/ERROR:\s+40001: bm25: pending list recycled concurrently/,
	'standby: the reuse 40001 reaches the client');

# ---- PART FOUR: every caller of the wrapper, standby against primary ----
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 't',
	'the standby is in recovery');
my %callers = (
	# WAND caps at 3, so LIMIT 200 forces the over-pull tail rebuild.
	tail => q{SET enable_seqscan = off; SET bm25_native.wand_top_k = 3;
		SELECT count(*), sum(id) FROM (SELECT id FROM cdocs WHERE body @@@ 'alpha'
		ORDER BY body &@@ 'alpha' LIMIT 200) s},
	# A bitmap phrase @@@: bm25_load_if_needed's delegated build.
	delegated => q{SET enable_seqscan = off; SET enable_indexscan = off;
		SELECT count(*), sum(id) FROM cdocs WHERE body @@@ '"alpha beta"'},
	# Two @@@ keys on one scan: the filtered branch (nwhere > 0).
	multiwhere => q{SET enable_seqscan = off; SET enable_indexscan = off;
		SELECT count(*), sum(id) FROM cdocs WHERE body @@@ 'alpha' AND body @@@ 'w7'},
	debug_rank => q{SELECT count(*), round(sum(score)::numeric, 6)
		FROM bm25_debug_rank('cdocs_bm25', 'alpha')},
);
for my $k (sort keys %callers)
{
	my $p = $primary->safe_psql('postgres', $callers{$k});
	my $s = $standby->safe_psql('postgres', $callers{$k});
	isnt($p, '0|', "$k: the primary returns rows ($p)");
	is($s, $p, "$k: the standby matches the primary");
}
like($standby->safe_psql('postgres', $callers{tail}), qr/^200\|/,
	'the standby tail rebuild returned 200 rows past wand_top_k = 3');

# ---- PART FIVE: an ERROR inside the build leaves the standby session usable ----
my ($rc5, $out5, $err5) = $standby->psql('postgres', q{
	SET enable_seqscan = off;
	SELECT id FROM nopos WHERE body @@@ '"alpha beta"' ORDER BY body &@@ '"alpha beta"' LIMIT 1;
	SELECT count(*) FROM (SELECT id FROM cdocs WHERE body @@@ 'alpha'
	                      ORDER BY body &@@ 'alpha' LIMIT 7) s;
	SELECT count(*) FROM cdocs WHERE body @@@ '"alpha beta"';
	SELECT count(*) FROM (SELECT id FROM cdocs WHERE body @@@ 'alpha'
	                      ORDER BY body &@@ 'alpha' LIMIT 7) s;
}, on_error_stop => 0);
like($err5, qr/ERROR/, "the in-build ERROR is raised on the standby: $err5");
unlike($err5, qr/(?:FATAL|WARNING):|leak/, 'with no FATAL and no leak warning');
my $phrase_n = $standby->safe_psql('postgres',
	"SELECT count(*) FROM cdocs WHERE body @@@ '\"alpha beta\"'");
is($out5, "7\n$phrase_n\n7",
	'the same session then runs ranked and delegated queries');

$standby->stop;
$primary->stop;
unlike(slurp_file($standby->logfile), qr/TRAP:|PANIC:|terminated by signal|leak/,
	'the standby log shows no trap, panic, crashed backend or leak');
bm25_check_logs($primary, $standby);
done_testing();
