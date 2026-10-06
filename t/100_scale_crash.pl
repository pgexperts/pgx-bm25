use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# CI-ONLY: the PostgreSQL::Test perl modules are not available in local builds.
# Run this test only via CI (e.g. pg_regress --tap-tests or prove under PGDG).
#
# Exercises §5 two-phase commit atomicity at scale, via crash RECOVERY of the
# build seal. (A literal crash *during* a bm25_seal C call is not deterministically
# reproducible from TAP without fault injection; this instead crashes immediately
# after the build's seal and asserts recovery is all-or-nothing.)
#   - Build a 2500-doc corpus (term 'common' in all docs -> multi-page postings;
#     unique term per doc -> multi-page dict); CREATE INDEX seals one segment.
#   - Crash with pg_ctl -m immediate stop (bypasses checkpoint/fsync -> WAL replay).
#   - Restart and assert crash-atomicity:
#       (a) the index is queryable (no corruption),
#       (b) either 0 or 1 segment is published (never a partial segment — the
#           single publish record either fully replays or not at all),
#       (c) a subsequent bm25_seal + query returns the full 2500-doc corpus.
#
# Corpus design (M2a multi-page limits):
#   2500 rows; 'common' and 'term' appear in all 2500 docs -> multiple posting pages;
#   each doc has unique token 'uN' -> 2500+ distinct dict entries -> multiple dict pages.
#   This is exactly the corpus that would have ERRORED in M0/M1 (single-page limits).

my $node = PostgreSQL::Test::Cluster->new('scale_crash');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', 'CREATE TABLE big (id int primary key, body text)');
$node->safe_psql('postgres',
    qq{INSERT INTO big SELECT g, 'common term' || ' u' || g FROM generate_series(1, 2500) g});

# Build the index (publishes one sealed segment on CREATE INDEX).
$node->safe_psql('postgres', 'CREATE INDEX big_bm25 ON big USING bm25_native (body)');

# Crash immediately after building the index (pg_ctl -m immediate bypasses
# checkpoint and fsync, forcing WAL replay on restart).
$node->stop('immediate');
$node->start;

# (a) Index must be queryable after crash recovery — safe_psql dies on any error,
# so reaching the assertion proves no corruption. The count is all-or-nothing: the
# build's single publish record either replayed (segment present -> 2500) or not
# (no segment, no pending -> 0); a value in between would signal a torn publish.
my $after_common_count = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM big WHERE body \@\@\@ 'common'});

ok($after_common_count == 0 || $after_common_count == 2500,
    "after crash: 'common' corpus is all-or-nothing — got $after_common_count");

# (b) Segment count after recovery: 0 or 1 (two-phase commit atomicity — no partial segments).
my $after_nsegs = $node->safe_psql('postgres',
    "SELECT nsegs FROM bm25_stats('big_bm25')");

ok($after_nsegs == 0 || $after_nsegs == 1,
    "after crash: 0 or 1 published segments (never partial) — got nsegs=$after_nsegs");

# (c) After re-seal, the full 2500-doc corpus is visible and queryable.
$node->safe_psql('postgres', "SELECT bm25_seal('big_bm25')");

my $final_common_count = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM big WHERE body \@\@\@ 'common'});
my $final_nsegs = $node->safe_psql('postgres',
    "SELECT nsegs FROM bm25_stats('big_bm25')");
my $final_ndocs = $node->safe_psql('postgres',
    "SELECT ndocs FROM bm25_stats('big_bm25')");

is($final_common_count, 2500,
    "after re-seal: all 2500 docs match 'common'");
is($final_ndocs, 2500,
    "after re-seal: ndocs=2500 in index stats");
ok($final_nsegs >= 1,
    "after re-seal: at least 1 segment published");

# Unique term u1777 must return exactly 1 hit.
my $u_hits = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM big WHERE body \@\@\@ 'u1777'});
is($u_hits, 1,
    "unique term u1777 returns exactly 1 hit after re-seal");

# Top-3 ranked results for common term must return 3 rows.
my $top3 = $node->safe_psql('postgres',
    qq{SELECT count(*) FROM (SELECT id FROM big ORDER BY body &\@\@ 'common' LIMIT 3) s});
is($top3, 3,
    "top-3 ranked query for 'common' returns 3 rows after re-seal");

$node->stop;
bm25_check_logs($node);
done_testing();
