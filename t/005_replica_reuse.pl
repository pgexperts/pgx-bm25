use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Physical-replica ranking equality under MERGE-DRIVEN PAGE REUSE — the headline
# open risk of the Generic-WAL design (spec §15). A primary workload that deletes,
# merges (retires pages), and VACUUMs twice (reclaims into the FSM, then reuses on the
# next alloc) exercises the exact page-recycling path that option (d) — per-page
# seg_gen stamping + reader validation — protects in place of a custom reuse-conflict
# WAL record (which is unbuildable under Generic-WAL-only). Mirrors t/003_replica.pl,
# extended with the delete+merge+double-VACUUM reuse cycle.
#
# With hot_standby_feedback = on the standby's xmin holds the primary's horizon back,
# so the horizon-gated allocator (Task 26) cannot recycle a page the standby is still
# scanning; the standby's ranking must therefore EQUAL the primary's after every cycle
# (never even reaching the gen-validation abort). With feedback OFF an aggressive
# reclaim may recycle a page under a standby read: the guarantee weakens to "identical
# ranking OR a clean seg_gen-validation abort — never a wrong/corrupt result".

my $primary = PostgreSQL::Test::Cluster->new('primary_reuse');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database storage engine term' || (g % 6) FROM generate_series(1, 2000) g});
$primary->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

my $backup_name = 'reuse_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby_reuse');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
# hot_standby_feedback keeps the primary horizon behind the standby's reads so
# horizon-gated reuse never recycles a page the standby is scanning; option (d)'s
# seg_gen validation is the backstop if a recycle ever sneaks past the horizon.
$standby->append_conf('postgresql.conf', "hot_standby_feedback = on\n");
$standby->start;

# Force page reuse on the primary: build a segment, delete a third, merge (retires the
# dropped segments' pages), VACUUM twice (cross the XID horizon -> reclaim into the
# FSM), then insert+seal (the gated allocator reuses the reclaimed pages).
for my $cycle (1 .. 3)
{
	my $base = 2000 + $cycle * 1000;
	$primary->safe_psql('postgres',
		qq{INSERT INTO docs SELECT g, 'database reuse cycle storage' FROM generate_series(@{[$base + 1]}, @{[$base + 500]}) g});
	$primary->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
	$primary->safe_psql('postgres', qq{DELETE FROM docs WHERE id % 3 = 0 AND id < $base});
	$primary->safe_psql('postgres', 'VACUUM docs');
	$primary->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});
	$primary->safe_psql('postgres', 'VACUUM docs');
	# The DELETE above is what crosses the horizon, not these VACUUMs: VACUUM (like
	# bm25_seal and any read-only SELECT) assigns no xid, so on its own it cannot
	# advance nextXid past a retire_xid stamp. The VACUUMs run the reclaim passes
	# that then hand the cleared pages to the FSM. This test asserts standby/primary
	# ranking equality, not a page count, so a missed reuse would not fail it.
	$primary->safe_psql('postgres', 'VACUUM docs');
	$primary->safe_psql('postgres',
		qq{INSERT INTO docs SELECT g, 'database reuse again storage' FROM generate_series(@{[$base + 501]}, @{[$base + 800]}) g});
	$primary->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});

	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('flush'));

	my $p = $primary->safe_psql('postgres',
		qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database', id LIMIT 10});
	my $s = $standby->safe_psql('postgres',
		qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database', id LIMIT 10});
	is($s, $p, "standby ranking identical to primary after reuse cycle $cycle");
}

# Option (d) backstop: with hot_standby_feedback OFF the standby no longer holds the
# primary horizon back, so an aggressive reclaim may recycle a page under a standby
# read. The guarantee is "correct OR clean abort, never wrong results": each standby
# query must return the identical ranking OR fail with the seg_gen-validation error
# ("bm25: segment reclaimed concurrently; retry") — never a different (corrupt) ranking.
$standby->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off');
$standby->safe_psql('postgres', 'SELECT pg_reload_conf()');
for my $cycle (1 .. 2)
{
	$primary->safe_psql('postgres', 'DELETE FROM docs WHERE id % 3 = 1 AND id < 5000');
	$primary->safe_psql('postgres', 'VACUUM docs');
	$primary->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});
	$primary->safe_psql('postgres', 'VACUUM docs');
	$primary->safe_psql('postgres', 'VACUUM docs');
	my $base = 10000 + $cycle * 1000;
	$primary->safe_psql('postgres',
		qq{INSERT INTO docs SELECT g, 'database nofeedback reuse storage' FROM generate_series($base, @{[$base + 400]}) g});
	$primary->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('flush'));

	my $p = $primary->safe_psql('postgres',
		qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database', id LIMIT 10});
	my ($s, $err);
	my $rc = $standby->psql('postgres',
		qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database', id LIMIT 10},
		stdout => \$s, stderr => \$err);
	ok($rc == 0 ? ($s eq $p) : ($err =~ /reclaimed concurrently/),
		"no-feedback cycle $cycle: standby returns identical ranking OR aborts cleanly (never wrong)");
}

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
