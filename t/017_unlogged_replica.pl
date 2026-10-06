use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Promotion of a standby carrying an UNLOGGED bm25_native index (ADR 0042).
#
# The replica half of t/016_unlogged_crash.pl. An unlogged relation's contents
# are never streamed -- that is the point of UNLOGGED -- but its INIT fork is,
# and on promotion the standby runs the same ResetUnloggedRelations path a
# crash restart does, rebuilding the main fork from that init fork.
#
# With the pre-ADR-0042 bug the standby received only core's log_smgrcreate
# (an empty init-fork FILE) and never the page contents, because GenericXLog
# emits no WAL for a non-permanent relation. Promotion then reset the main fork
# from a zero-filled init fork and every query failed bm25_meta_validate's
# magic gate -- surfacing as "could not read block 0" or "bm25: corrupt or
# uninitialized index" until REINDEX.
#
# No CHECKPOINT is issued on the primary before the backup/promote sequence,
# for the same reason t/016 avoids one: a checkpoint makes the init fork
# durable by accident and the suite would then pass against the broken code.
# checkpoint_timeout / max_wal_size are pushed out so nothing fires
# automatically. (backup() does take a checkpoint as part of pg_basebackup --
# so the index is created AFTER the backup, and its init-fork WAL reaches the
# standby by streaming, which is precisely the path under test.)

my $primary = PostgreSQL::Test::Cluster->new('unlogged_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 10GB
});
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

my $backup_name = 'unlogged_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('unlogged_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

# Create the unlogged table and its bm25 index AFTER the standby is streaming,
# so the init-fork WAL records are carried by replication rather than by the
# base backup's own checkpoint.
$primary->safe_psql('postgres',
    'CREATE UNLOGGED TABLE udocs(id int primary key, body text)');
$primary->safe_psql('postgres',
    qq{INSERT INTO udocs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 400) g});
$primary->safe_psql('postgres',
    'CREATE INDEX udocs_bm25 ON udocs USING bm25_native (body)');

my $before = $primary->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});
is($before, 400, 'unlogged bm25 index matches all 400 rows on the primary');

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# An unlogged relation is not readable on a standby at all, so there is nothing
# to assert pre-promotion. Promote, which runs ResetUnloggedRelations.
$standby->promote;
$standby->safe_psql('postgres', 'SELECT 1');   # wait until it accepts writes

# Same contract as the crash case: empty, but queryable and usable.
my ($rc, $stdout, $stderr) = $standby->psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});

is($rc, 0, 'unlogged bm25 index is queryable on the promoted standby (init fork was WAL-logged)')
    or diag("psql failed after promotion: $stderr");
like($stderr, qr/^$/, 'no error from the post-promotion query')
    or diag("unexpected stderr: $stderr");
is($stdout, 0, 'unlogged bm25 index reads as EMPTY on the promoted standby');

my ($src, $sout, $serr) = $standby->psql('postgres',
    q{SELECT ndocs FROM bm25_stats('udocs_bm25')});
is($src, 0, 'bm25_stats reads the promoted standby metapage')
    or diag("bm25_stats failed: $serr");
is($sout, 0, 'promoted standby index reports ndocs = 0');

$standby->safe_psql('postgres',
    qq{INSERT INTO udocs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 50) g});
my $after_insert = $standby->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});
is($after_insert, 50, 'promoted standby index accepts and matches new inserts');

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
