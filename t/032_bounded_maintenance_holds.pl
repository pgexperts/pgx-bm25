use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Maintenance releases the seal/merge singleton between bounded units of work, so a
# document INSERT waits for one unit, not the whole operation (issue #300, part B).
#
#   (A) bm25_reclaim_retired frees one retired-list descriptor page per hold (one
#       merge's inputs), then releases and restarts from retired_head. A VACUUM is
#       parked between two such chunks (pause point reclaim_retired_between_chunks);
#       it must hold no lock on the index there, and an INSERT with a short
#       lock_timeout must succeed. It used to hold the singleton across the whole
#       horizon-cleared list.
#   (B) bm25_merge() releases the singleton between forced ladder passes. It is
#       parked between two passes (merge_between_passes) and an INSERT must succeed.
#       It used to hold the singleton across every pass.
#
# Against the unchunked reclaim and the unreleased merge -- with the same pause
# points left where the old code would be at that moment, still holding the lock --
# both INSERTs time out. The opportunistic single merge pass VACUUM runs is not
# bounded this way (a pass is the unit that cannot be split); that residual is
# documented, not tested here.

my $node = PostgreSQL::Test::Cluster->new('bounded_maintenance_holds');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

my $pause_key = 1651323445;
my %point = (reclaim_retired_between_chunks => 13, merge_between_passes => 14);

# $n documents seal as one segment of $n.
sub segment
{
	my ($t, $from, $n, $word) = @_;
	$node->safe_psql('postgres', qq{
		INSERT INTO $t SELECT g, '$word common w' || g
		  FROM generate_series($from, @{[ $from + $n - 1 ]}) g;
		SELECT bm25_seal('${t}_idx');});
}

sub parked
{
	my ($app, $n) = @_;
	return $node->poll_query_until('postgres', qq{SELECT EXISTS (SELECT 1 FROM pg_locks l
		JOIN pg_stat_activity a USING (pid) WHERE a.application_name = '$app'
		AND l.locktype = 'advisory' AND NOT l.granted AND l.objid = $n)});
}

sub index_locks_held
{
	my ($app, $t) = @_;
	return $node->safe_psql('postgres', qq{SELECT count(*) FROM pg_locks l
		JOIN pg_stat_activity a USING (pid) WHERE a.application_name = '$app'
		AND l.locktype = 'page' AND l.relation = '${t}_idx'::regclass AND l.granted});
}

sub insert_ok
{
	my ($t, $id, $label) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', qq{
		SET lock_timeout = '100ms';
		INSERT INTO $t VALUES ($id, 'inserted between units');});
	is($rc, 0, $label) or diag($err);
}

# ---- (A) reclaim_retired, one descriptor page per hold ----------------------------
{
	my $t = 'ra';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);});
	# Two merges, two descriptor pages, both still behind the horizon until the
	# snapshot below goes away (or the second merge's own reclaim would free the
	# first merge's ranges).
	my $snap = $node->background_psql('postgres');
	$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$snap->query_safe('SELECT 1');
	segment($t, 1 + 50 * $_, 50, 'alpha') for 0 .. 3;
	$node->safe_psql('postgres', "SELECT bm25_merge('${t}_idx')");
	segment($t, 1001 + 50 * $_, 50, 'beta') for 0 .. 3;
	$node->safe_psql('postgres', "SELECT bm25_merge('${t}_idx')");
	is($node->safe_psql('postgres', "SELECT bm25_debug_retired_pages('${t}_idx')"), '2',
		'(A) two retired-list descriptor pages');
	is($node->safe_psql('postgres', "SELECT bm25_debug_retired_count('${t}_idx')"), '8',
		'(A) holding eight merged-away segments');
	$snap->query_safe('COMMIT');
	$snap->quit;
	$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;

	my $holder = $node->background_psql('postgres');
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{reclaim_retired_between_chunks})");
	my $v = $node->background_psql('postgres', on_error_stop => 0);
	$v->query_safe("SET application_name = 'vac_ra'");
	$v->query_safe("SET bm25_native.debug_pause = 'reclaim_retired_between_chunks'");
	$v->query_until(qr/started/, "\\echo started\nVACUUM $t;\n");
	parked('vac_ra', $point{reclaim_retired_between_chunks})
	  or die '(A) VACUUM never reached reclaim_retired_between_chunks';

	cmp_ok($node->safe_psql('postgres', "SELECT bm25_debug_retired_count('${t}_idx')"),
		'<', 8, '(A) one chunk is done');
	cmp_ok($node->safe_psql('postgres', "SELECT bm25_debug_retired_count('${t}_idx')"),
		'>', 0, '(A) and another is still to do');
	is(index_locks_held('vac_ra', $t), '0', '(A) between chunks VACUUM holds no lock on the index');
	insert_ok($t, 5001, '(A) an INSERT does not wait for the rest of the reclaim');

	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{reclaim_retired_between_chunks})");
	$v->query('SELECT 1');
	is($v->{stderr}, '', '(A) the VACUUM finished');
	$v->quit;
	$holder->quit;
	is($node->safe_psql('postgres', "SELECT bm25_debug_retired_count('${t}_idx')"), '0',
		'(A) and reclaimed every retired range');
	is($node->safe_psql('postgres', qq{SET enable_seqscan = off;
		SELECT count(*) FROM $t WHERE body @@@ 'common'}), '400',
		'(A) every document is found');
}

# ---- (B) bm25_merge releases between forced passes ---------------------------------
{
	my $t = 'rb';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);});
	# Two size layers of four segments each (50 and 300 documents): two ladder passes.
	segment($t, 1 + 50 * $_, 50, 'small') for 0 .. 3;
	segment($t, 1001 + 300 * $_, 300, 'large') for 0 .. 3;
	is($node->safe_psql('postgres', "SELECT nsegs FROM bm25_stats('${t}_idx')"), '8',
		'(B) eight segments in two layers');

	my $holder = $node->background_psql('postgres');
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{merge_between_passes})");
	my $m = $node->background_psql('postgres', on_error_stop => 0);
	$m->query_safe("SET application_name = 'merge_rb'");
	$m->query_safe("SET bm25_native.debug_pause = 'merge_between_passes'");
	$m->query_until(qr/started/, "\\echo started\nSELECT bm25_merge('${t}_idx');\n");
	parked('merge_rb', $point{merge_between_passes})
	  or die '(B) bm25_merge never reached merge_between_passes';

	is($node->safe_psql('postgres', "SELECT nsegs FROM bm25_stats('${t}_idx')"), '5',
		'(B) one pass has swapped');
	is(index_locks_held('merge_rb', $t), '0', '(B) between passes the merge holds no lock on the index');
	insert_ok($t, 9001, '(B) an INSERT does not wait for the next pass');

	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{merge_between_passes})");
	$m->query('SELECT 1');
	is($m->{stderr}, '', '(B) the merge finished');
	$m->quit;
	$holder->quit;
	is($node->safe_psql('postgres', "SELECT nsegs FROM bm25_stats('${t}_idx')"), '2',
		'(B) both layers were merged');
	is($node->safe_psql('postgres', qq{SET enable_seqscan = off;
		SELECT count(*) FROM $t WHERE body @@@ 'common'}), '1400',
		'(B) every document is found');
}

$node->stop;
bm25_check_logs($node);
done_testing();
