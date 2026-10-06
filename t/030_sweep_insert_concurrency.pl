use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Inserts do not wait for VACUUM's orphan sweep, and so do not get the autovacuum
# that runs it cancelled (issue #300, the issue's success criterion).
#
# The sweep marks every live chain and visits every block of the index, throttled.
# It used to hold the seal/merge singleton in ExclusiveLock throughout, and every
# document-adding INSERT takes that singleton in ShareLock (ADR 0022), so each
# insert waited for the whole sweep. Under autovacuum, the waiting insert's deadlock
# check then cancelled the worker after deadlock_timeout ("canceling autovacuum
# task ... while cleaning up index"), the worker's whole transaction aborted, and on
# an insert-busy table that repeated on every attempt. The sweep now holds the
# singleton in ShareLock, which appenders share.
#
#   (A) A manual VACUUM parked inside its sweep (pause point orphan_sweep_start); an
#       INSERT with a lock_timeout far below anything a sweep could take succeeds.
#   (B) An autovacuum sweeping a 500-page index under a throttle that makes the sweep
#       take many seconds, with deadlock_timeout = 200ms; document INSERTs run while
#       the worker holds the singleton. None may wait as long as deadlock_timeout, the
#       log must have no "canceling autovacuum task", and autovacuum_count advances.
#       Set up as the issue's reproduction: pending list empty, autovacuum triggered by
#       all-NULL rows (they add nothing to the pending list, so the seal and the merge
#       are not the long holders), and the index put in the "sweep needed" state first
#       -- or the gate alone would pass this by skipping the sweep.
#
# Against the ExclusiveLock sweep, (A)'s INSERT times out and (B) reproduces the
# cancellation.

my $node = PostgreSQL::Test::Cluster->new('sweep_insert_concurrency');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = on
autovacuum_naptime = 1s
deadlock_timeout = 200ms
log_autovacuum_min_duration = 0
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

my $pause_key = 1651323445;
my %point = (orphan_sweep_start => 8);

sub big_table
{
	my ($t) = @_;
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		INSERT INTO $t SELECT g, (SELECT string_agg('w' || ((g * k) % 4999), ' ')
		                            FROM generate_series(1, 40) k)
		  FROM generate_series(1, 20000) g;
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
	});
}

# ---- (A) an INSERT during a manual VACUUM's sweep ---------------------------------
{
	big_table('ta');    # never swept: its first VACUUM sweeps
	my $holder = $node->background_psql('postgres');
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{orphan_sweep_start})");
	my $v = $node->background_psql('postgres', on_error_stop => 0);
	$v->query_safe("SET application_name = 'vaca'");
	$v->query_safe("SET bm25_native.debug_pause = 'orphan_sweep_start'");
	$v->query_until(qr/started/, "\\echo started\nVACUUM ta;\n");
	$node->poll_query_until('postgres', q{SELECT EXISTS (SELECT 1 FROM pg_locks l
		JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'vaca'
		AND l.locktype = 'advisory' AND NOT l.granted)})
	  or die '(A) VACUUM never reached its sweep';
	is($node->safe_psql('postgres', q{SELECT l.mode FROM pg_locks l
		JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'vaca'
		AND l.locktype = 'page' AND l.relation = 'ta_idx'::regclass AND l.page = 0
		AND l.granted}), 'ShareLock', '(A) the parked sweep holds the singleton in ShareLock');

	my ($rc, $out, $err) = $node->psql('postgres', q{
		SET lock_timeout = '100ms';
		INSERT INTO ta VALUES (20001, 'inserted during the sweep');});
	is($rc, 0, '(A) a document INSERT does not wait for the sweep') or diag($err);

	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{orphan_sweep_start})");
	$v->query('SELECT 1');
	is($v->{stderr}, '', '(A) the VACUUM finished');
	$v->quit;
	$holder->quit;
	is($node->safe_psql('postgres', q{SET enable_seqscan = off;
		SELECT count(*) FROM ta WHERE body @@@ 'inserted'}), '1',
		'(A) the inserted document is found through the index');
}

# ---- (B) autovacuum's sweep on an insert-busy table --------------------------------
{
	big_table('tb');
	# Make the heap all-visible now, unthrottled, so the worker's heap pass is short;
	# that VACUUM sweeps the new index and clears its evidence, so put it back: a
	# seal that "dies" after its build leaves its orphan bracket open.
	$node->safe_psql('postgres', q{VACUUM tb; SELECT bm25_debug_seal_unpublished('tb_idx')});
	is($node->safe_psql('postgres',
		"SELECT ops_begun > ops_done FROM bm25_debug_sweep_evidence('tb_idx')"),
		't', '(B) the index needs its sweep');
	my $logstart = -s $node->logfile;

	$node->safe_psql('postgres', q{
		ALTER TABLE tb SET (autovacuum_enabled = on,
		                    autovacuum_vacuum_cost_delay = 20,
		                    autovacuum_vacuum_cost_limit = 1,
		                    autovacuum_vacuum_insert_threshold = 1,
		                    autovacuum_vacuum_insert_scale_factor = 0);
		INSERT INTO tb SELECT g, NULL FROM generate_series(30001, 30050) g;});

	# The worker holds the singleton for its sweep, in whatever mode: the seal and
	# the reclaims before it hold it for milliseconds, so a hold seen twice 300 ms
	# apart is the sweep.
	my $hold_q = q{SELECT coalesce((SELECT l.mode FROM pg_locks l
		JOIN pg_stat_activity a USING (pid)
		WHERE a.backend_type = 'autovacuum worker' AND l.locktype = 'page'
		  AND l.relation = 'tb_idx'::regclass AND l.page = 0 AND l.granted LIMIT 1), '')};
	my $sweeping = "SELECT ($hold_q) <> ''";
	my $mode = '';
	for (1 .. 600)
	{
		$mode = $node->safe_psql('postgres', $hold_q);
		if ($mode ne '')
		{
			select(undef, undef, undef, 0.3);
			last if $node->safe_psql('postgres', $hold_q) eq $mode;
		}
		select(undef, undef, undef, 0.3);
		$mode = '';
	}
	isnt($mode, '', '(B) an autovacuum worker is sweeping the index');
	is($mode, 'ShareLock', '(B) and holds the singleton in ShareLock');

	# Twenty document INSERTs, each timed inside the server. A wait for the worker's
	# lock would last deadlock_timeout and end with the worker cancelled.
	my $max_ms = $node->safe_psql('postgres', q{
		SET lock_timeout = '2s';
		CREATE TEMP TABLE lat (ms float8);
		DO $$
		DECLARE t0 timestamptz;
		BEGIN
		  FOR i IN 1 .. 20 LOOP
		    t0 := clock_timestamp();
		    INSERT INTO tb VALUES (40000 + i, 'busy insert ' || i);
		    INSERT INTO lat VALUES (extract(epoch FROM clock_timestamp() - t0) * 1000);
		    PERFORM pg_sleep(0.05);
		  END LOOP;
		END $$;
		SELECT round(max(ms))::int FROM lat;});
	is($node->safe_psql('postgres', $sweeping), 't',
		'(B) the worker was still sweeping after the inserts');
	cmp_ok($max_ms, '<', 150, "(B) no INSERT waited for the sweep (max ${max_ms} ms)");

	$node->poll_query_until('postgres',
		q{SELECT autovacuum_count > 0 FROM pg_stat_user_tables WHERE relname = 'tb'})
	  or die '(B) autovacuum never completed on tb';
	pass('(B) autovacuum_count advanced');
	is($node->safe_psql('postgres',
		"SELECT ops_begun = ops_done FROM bm25_debug_sweep_evidence('tb_idx')"),
		't', '(B) the autovacuum sweep completed and retired the evidence');
	my $log = slurp_file($node->logfile, $logstart);
	unlike($log, qr/canceling autovacuum task/, '(B) the autovacuum was never cancelled');
	is($node->safe_psql('postgres', q{SET enable_seqscan = off;
		SELECT count(*) FROM tb WHERE body @@@ 'busy'}), '20',
		'(B) every busy insert is found through the index');
}

$node->stop;
bm25_check_logs($node);
done_testing();
