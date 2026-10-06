use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Verify that a physical streaming replica returns identical bm25_native ranking to
# the primary.  This is the headline advantage of the Generic-WAL design: no
# custom resource manager, no shared-preload hook, no Enterprise tier — stock
# Postgres streaming replication carries the index pages verbatim.
#
# Corpus design (M0/M1 single-page limits):
#   400 rows; 'database' and 'storage' each appear in all 400 docs (≤500 limit);
#   'termN' (N=0..7) each appear in 50 docs; ~10 distinct terms total — well
#   within the single-page dictionary and single-page posting limits.

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$primary->safe_psql('postgres',
    qq{INSERT INTO docs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 400) g});
$primary->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

my $backup_name = 'replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

# Wait until the standby has replayed all WAL written so far.
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# Query both nodes with identical ORDER BY … LIMIT; the ranked id list must match.
# This proves physical replication of a Generic-WAL bm25_native index works without any
# custom rmgr or Enterprise-only feature.
my $primary_ids = $primary->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database' LIMIT 5});
my $standby_ids = $standby->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database' LIMIT 5});

is($standby_ids, $primary_ids,
    'standby returns identical bm25_native ranking (Generic WAL; no custom rmgr, no Enterprise tier)');

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
