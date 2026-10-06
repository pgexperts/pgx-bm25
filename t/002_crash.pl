use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Verify that a bm25_native index is fully recovered after an immediate-mode crash.
# Because all writes go through Generic WAL, standard crash recovery replays
# the index pages with no custom redo logic required.
#
# Corpus design (M0/M1 single-page limits):
#   400 rows; 'database' and 'storage' each appear in all 400 docs (≤500 limit);
#   'termN' (N=0..7) each appear in 50 docs; ~10 distinct terms total — well
#   within the single-page dictionary and single-page posting limits.

my $node = PostgreSQL::Test::Cluster->new('crash');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$node->safe_psql('postgres',
    qq{INSERT INTO docs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 400) g});
$node->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

# Record match count and top-3 ranked IDs before crash.
my $before_count = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
my $before_top3 = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database' LIMIT 3});

# Simulate crash: immediate stop loses shared buffers, forcing WAL replay on restart.
$node->stop('immediate');
$node->start;

my $after_count = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
my $after_top3 = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database' LIMIT 3});

is($after_count, $before_count, 'bm25_native match count unchanged after crash recovery');
is($after_top3,  $before_top3,  'bm25_native top-3 ranked IDs unchanged after crash recovery');

$node->stop;
bm25_check_logs($node);
done_testing();
