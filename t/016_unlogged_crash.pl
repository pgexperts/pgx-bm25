use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Crash recovery of an UNLOGGED bm25_native index (ADR 0042).
#
# An unlogged relation's MAIN fork is discarded on crash recovery and rebuilt
# from its INIT fork by ResetUnloggedRelations. So the init fork is the only
# thing that survives, and it is the one fork that MUST be WAL-logged -- which
# is exactly what the bug was:
#
#   GenericXLogStart takes isLogged from RelationNeedsWAL, which requires
#   RelationIsPermanent. For an unlogged index that is FALSE, so every
#   INIT_FORKNUM write through GenericXLog emitted NO WAL record at all. The
#   smgrimmedsync believed to cover it fsyncs the FILE at the md layer, while
#   the content sat dirty in shared buffers -- so it flushed the zeros
#   smgrzeroextend wrote at P_NEW time.
#
# Result: crash before the next checkpoint, and ResetUnloggedRelations copied
# two ZERO pages over the main fork. Every subsequent query died on
# bm25_meta_validate's magic gate with "bm25: corrupt or uninitialized index".
# bm25_meta_finish / bm25_fieldcfg_write_init now use core's buildempty
# discipline instead (log_newpage_buffer, as ginbuildempty/brinbuildempty do).
#
# WHY THERE IS NO CHECKPOINT BELOW, AND WHY THE GUCS MATTER: the defect is
# invisible once a checkpoint has flushed the dirty init-fork buffers to disk,
# because then the content is durable by accident rather than by WAL. An
# automatic checkpoint between CREATE INDEX and the crash would make this suite
# pass against the BROKEN code -- the "passes either way" trap this project has
# hit twice (ADR 0041's dead interrupt checks, sql/80_maintenance_interrupts's
# header). checkpoint_timeout and max_wal_size are pushed out of the way so no
# automatic checkpoint can fire, and no explicit CHECKPOINT is ever issued.
#
# The correct post-crash state is an EMPTY, QUERYABLE index: unlogged data is
# gone by design, but the index must read as empty rather than error, and must
# still be usable for new inserts.

my $node = PostgreSQL::Test::Cluster->new('unlogged_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
checkpoint_timeout = 1h
max_wal_size = 10GB
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres',
    'CREATE UNLOGGED TABLE udocs(id int primary key, body text)');
$node->safe_psql('postgres',
    qq{INSERT INTO udocs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 400) g});

# ambuildempty runs here: this is the call that writes the INIT fork.
$node->safe_psql('postgres',
    'CREATE INDEX udocs_bm25 ON udocs USING bm25_native (body)');

# #292: a KEYED unlogged index. ambuildempty must stamp the init fork with the same
# key identity ambuild stamps on the main fork, or the index rebuilt from the init
# fork after a crash would be unstamped and admit a key_field change on its empty
# catalog -- exactly the window #292 closes.
$node->safe_psql('postgres',
    'CREATE UNLOGGED TABLE ukeys(id int, id8 bigint, body text)');
$node->safe_psql('postgres',
    qq{CREATE INDEX ukeys_bm25 ON ukeys USING bm25_native (body) INCLUDE (id, id8) WITH (key_field = 'id')});

# Sanity: the index works before the crash. (It does even with the bug -- the
# main fork is fine; only the init fork's durability is broken.)
my $before = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});
is($before, 400, 'unlogged bm25 index matches all 400 rows before the crash');

# Crash with NO intervening checkpoint. See the header: a checkpoint here would
# make this suite pass against the broken code.
$node->stop('immediate');
$node->start;

# The table is empty by design (unlogged data does not survive a crash). The
# INDEX must survive as a readable, empty index. Pre-fix this query died with
# "bm25: corrupt or uninitialized index" because the main fork had been reset
# from a zero-filled init fork.
my ($rc, $stdout, $stderr) = $node->psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});

is($rc, 0, 'unlogged bm25 index is queryable after crash recovery (init fork was WAL-logged)')
    or diag("psql failed after crash recovery: $stderr");
like($stderr, qr/^$/, 'no error from the post-crash query')
    or diag("unexpected stderr: $stderr");
is($stdout, 0, 'unlogged bm25 index reads as EMPTY after crash recovery');

# bm25_stats must also read the metapage without tripping the version/magic gate.
my ($src, $sout, $serr) = $node->psql('postgres',
    q{SELECT ndocs FROM bm25_stats('udocs_bm25')});
is($src, 0, 'bm25_stats reads the recovered metapage')
    or diag("bm25_stats failed: $serr");
is($sout, 0, 'recovered index reports ndocs = 0');

# And the index must still be USABLE: new rows index and rank normally.
$node->safe_psql('postgres',
    qq{INSERT INTO udocs SELECT g, 'database storage term' || (g % 8) FROM generate_series(1, 50) g});
my $after_insert = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM udocs WHERE body \@\@\@ 'database'});
is($after_insert, 50, 'recovered unlogged index accepts and matches new inserts');

my $ranked = $node->safe_psql('postgres',
    qq{SET enable_seqscan=off; SELECT count(*) FROM (SELECT id FROM udocs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database' LIMIT 5) s});
is($ranked, 5, 'ranked scan works on the recovered unlogged index');

# #292: the reset main fork is a copy of the init fork, so its stamp is the one
# ambuildempty wrote.
is($node->safe_psql('postgres',
    q{SELECT stamped || ',' || key_type || ',' || key_size || ',' || key_column FROM bm25_debug_keystamp('ukeys_bm25')}),
    'true,1,4,id', 'recovered keyed unlogged index carries the init fork key stamp');
$node->safe_psql('postgres', q{ALTER INDEX ukeys_bm25 SET (key_field = 'id8')});
my ($krc, $kout, $kerr) = $node->psql('postgres',
    q{INSERT INTO ukeys VALUES (1, 1, 'alpha one')});
isnt($krc, 0, 'a changed key_field is refused on the recovered, empty index');
like($kerr, qr/key_field does not match the key column this index was built with/,
    'refused by the key-stamp check');
$node->safe_psql('postgres', q{ALTER INDEX ukeys_bm25 SET (key_field = 'id')});
$node->safe_psql('postgres', q{INSERT INTO ukeys VALUES (1, 1, 'alpha one')});
is($node->safe_psql('postgres', q{SELECT bm25_seal('ukeys_bm25') IS NOT NULL}),
    't', 'restoring key_field recovers; the seal succeeds');

$node->stop;
bm25_check_logs($node);
done_testing();
