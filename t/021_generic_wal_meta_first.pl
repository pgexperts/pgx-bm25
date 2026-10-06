use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Every multi-buffer Generic WAL record that carries the metapage registers it
# FIRST (issue #240, ADR 0018).
#
# On a hot standby generic_redo locks each registered block EXCLUSIVE in
# block_id (= registration) order and holds all of them until the record is
# applied. A standby scan holds the metapage SHARE while it walks the segment
# catalog SHARE (ADR 0018's metapage-before-catalog order), so a record that
# registers a reachable catalog page before the metapage wedges the startup
# process and the query on two LWLocks -- uninterruptible, until SIGKILL. On the
# primary registration takes no locks at all, which is why only the WAL shows it.
#
# The deadlock itself needs a timed standby race; this suite asserts the
# mechanism instead, deterministically: pg_waldump over a workload that emits
# each of the three multi-buffer records carrying the metapage, and every such
# record must have block 0 at blkref #0. Each record shape is also required to
# have been OBSERVED, so a workload drift that stops emitting one of them fails
# here instead of passing vacuously:
#   (a) bm25_pending_append_multi: pending tail page + metapage (every INSERT),
#       and, when the part needs a fresh page, metapage + old tail + new page in
#       ONE record (issue #300) -- no Generic record of an INSERT may touch a
#       pending page without the metapage, which is what the old three-record
#       shape (init the new page; link the old tail; then append) emitted;
#   (b) bm25_segcat_publish_append, IN-PLACE branch: a second bm25_seal appends
#       to the live segcat_root the first seal created -- the reachable-page
#       case; the fresh-page branch writes an unreachable orphan;
#   (c) bm25_livedocs_clear: LIVE page + catalog page + metapage (VACUUM of a
#       DELETEd sealed document).
#
# pg_waldump, not pg_walinspect: the source-built cassert/UBSan/ASan CI legs
# install no contrib, while pg_waldump is a core src/bin program present in
# every leg's bindir (which PGXS's prove_installcheck puts first on PATH).

use constant {
    PAGE_PENDING => 1 << 1,
    PAGE_SEGCAT  => 1 << 2,
    PAGE_LIVE    => 1 << 6,
};

my $node = PostgreSQL::Test::Cluster->new('generic_wal_meta_first');
# wal_level = replica: the WAL a standby would replay. Nothing here needs the
# standby itself.
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 10GB
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION bm25_native;
CREATE TABLE d(id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
-- Built EMPTY, so segcat_root is Invalid until the first seal: that seal takes
-- the fresh-page branch and the second one provably takes the in-place branch.
CREATE INDEX d_bm25 ON d USING bm25_native (body);
});

my $rel = $node->safe_psql('postgres', q{
SELECT CASE WHEN c.reltablespace = 0 THEN db.dattablespace ELSE c.reltablespace END
       || '/' || db.oid || '/' || pg_relation_filenode(c.oid)
  FROM pg_class c, pg_database db
 WHERE c.oid = 'd_bm25'::regclass AND db.datname = current_database()});

sub lsn_int
{
    my ($hi, $lo) = split m{/}, shift;
    return hex($hi) * 2**32 + hex($lo);
}

sub insert_lsn
{
    return $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
}

# Page kinds as of the end of a phase, so later phases (seal draining the
# pending chain, a VACUUM retiring pages) cannot reclassify a block after the
# fact.
sub page_flags
{
    my %flags = map { split /:/ } split /,/, $node->safe_psql('postgres', q{
SELECT string_agg(b || ':' || bm25_debug_page_flags('d_bm25', b), ',')
  FROM generate_series(0, bm25_debug_npages('d_bm25')::int - 1) b});
    return \%flags;
}

my @phases;    # [name, start lsn, end lsn, flags]
sub phase
{
    my ($name, $sql) = @_;
    my $start = insert_lsn();
    $node->safe_psql('postgres', $sql);
    push @phases, [ $name, lsn_int($start), lsn_int(insert_lsn()), page_flags() ];
    return;
}

my $wal_start = insert_lsn();
phase('insert1', q{INSERT INTO d SELECT g, 'alpha beta gamma ' || g FROM generate_series(1, 40) g});
# Several pending pages' worth: every row adds ~20 distinct terms, so the chain
# grows by fresh pages linked from the old tail.
phase('insert_pages', q{INSERT INTO d SELECT g, (SELECT string_agg('t' || g || 'w' || k, ' ')
  FROM generate_series(1, 20) k) FROM generate_series(1001, 1200) g});
phase('seal1',   q{SELECT bm25_seal('d_bm25')});
phase('insert2', q{INSERT INTO d SELECT g, 'delta epsilon ' || g FROM generate_series(41, 80) g});
phase('seal2',   q{SELECT bm25_seal('d_bm25')});
phase('vacuum',  q{DELETE FROM d WHERE id = 7; VACUUM d});
my $wal_end = insert_lsn();
# Flush everything up to $wal_end: pg_waldump reads the files, and the seal
# records carry no xid, so no commit flushed them.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

my ($stdout, $stderr);
my $ok = IPC::Run::run(
    [ 'pg_waldump', '--path' => $node->data_dir . '/pg_wal',
      '--rmgr' => 'Generic', '--relation' => $rel,
      '--start' => $wal_start, '--end' => $wal_end ],
    '>', \$stdout, '2>', \$stderr);
ok($ok, 'pg_waldump read the workload WAL') or diag($stderr);

# One line per record. Both the compact (", blkref #N: rel S/D/R blk B") and
# the --bkp-details ("fork main") spellings are accepted; PG17-19 print the
# compact one for the main fork. A trailing " FPW" etc. is ignored.
my @records;
for my $line (split /\n/, $stdout)
{
    next unless $line =~ m{lsn: ([0-9A-Fa-f]+/[0-9A-Fa-f]+)};
    my $lsn = lsn_int($1);
    my @blk;
    while ($line =~ m{blkref #(\d+): rel (\d+/\d+/\d+)(?: fork \S+)? blk (\d+)}g)
    {
        push @blk, { id => $1, rel => $2, blk => $3 };
    }
    my ($ph) = grep { $lsn >= $_->[1] && $lsn < $_->[2] } @phases;
    push @records, { lsn => $lsn, blk => \@blk, phase => $ph };
}
cmp_ok(scalar @records, '>', 0, 'pg_waldump returned Generic records for the index');

# The invariant under test.
my @with_meta = grep {
    my $r = $_;
    grep { $_->{rel} eq $rel && $_->{blk} == 0 } @{ $r->{blk} }
} @records;
my @bad = grep {
    my $b0 = (grep { $_->{id} == 0 } @{ $_->{blk} })[0];
    !($b0 && $b0->{rel} eq $rel && $b0->{blk} == 0)
} @with_meta;
is(scalar @bad, 0, 'every Generic record touching the metapage registers it as blkref #0')
    or diag(join "\n", map {
        sprintf('lsn %X/%X: %s', int($_->{lsn} / 2**32), $_->{lsn} % 2**32,
            join ', ', map { "#$_->{id} blk $_->{blk}" } @{ $_->{blk} })
    } @bad[0 .. ($#bad < 4 ? $#bad : 4)]);

# Shape coverage: the other (non-meta) blocks of each multi-buffer meta record,
# classified by page kind at the end of the phase that wrote it.
sub others_of_kind
{
    my ($r, $kind) = @_;
    my $flags = $r->{phase} ? $r->{phase}[3] : {};
    return grep { $_->{blk} != 0 && (($flags->{ $_->{blk} } // 0) & $kind) } @{ $r->{blk} };
}
my @multi = grep { @{ $_->{blk} } >= 2 } @with_meta;
sub in_phase { my ($r, $name) = @_; return $r->{phase} && $r->{phase}[0] eq $name }

my @pending = grep { in_phase($_, 'insert1') && others_of_kind($_, PAGE_PENDING) } @multi;
cmp_ok(scalar @pending, '>', 0, '(a) pending append emitted a tail+metapage record');

# The new-page append: metapage, old tail, new page, in that order (the old tail is
# the predecessor the chain walker meets first). Pre-#300 this was three records.
my @linked = grep {
    my $r = $_;
    my %by_id = map { ($_->{id} => $_) } @{ $r->{blk} };
    my $flags = $r->{phase}[3];
    @{ $r->{blk} } == 3
      && $by_id{1} && $by_id{2} && $by_id{1}{blk} != $by_id{2}{blk}
      && (($flags->{ $by_id{1}{blk} } // 0) & PAGE_PENDING)
      && (($flags->{ $by_id{2}{blk} } // 0) & PAGE_PENDING)
} grep { in_phase($_, 'insert_pages') } @with_meta;
cmp_ok(scalar @linked, '>', 0,
    '(a) a new pending page is initialised, linked and published in one metapage+old tail+new page record');
my @split = grep {
    my $r = $_;
    (in_phase($r, 'insert1') || in_phase($r, 'insert_pages') || in_phase($r, 'insert2'))
      && !grep { $_->{rel} eq $rel && $_->{blk} == 0 } @{ $r->{blk} }
} @records;
is(scalar @split, 0, '(a) no INSERT emits a Generic record without the metapage (the old split append)')
    or diag(join "\n", map {
        sprintf('lsn %X/%X: %s', int($_->{lsn} / 2**32), $_->{lsn} % 2**32,
            join ', ', map { "#$_->{id} blk $_->{blk}" } @{ $_->{blk} })
    } @split[0 .. ($#split < 4 ? $#split : 4)]);

my @pub1 = grep { in_phase($_, 'seal1') && others_of_kind($_, PAGE_SEGCAT) } @multi;
my @pub2 = grep { in_phase($_, 'seal2') && others_of_kind($_, PAGE_SEGCAT) } @multi;
is(scalar @pub1, 1, 'first seal emitted one catalog+metapage publish record');
is(scalar @pub2, 1, 'second seal emitted one catalog+metapage publish record');
# In place: the second publish rewrote the SAME catalog page the first one
# created, i.e. the reachable segcat_root, not a freshly prepended orphan.
my ($cat1) = map { $_->{blk} } map { others_of_kind($_, PAGE_SEGCAT) } @pub1;
my ($cat2) = map { $_->{blk} } map { others_of_kind($_, PAGE_SEGCAT) } @pub2;
ok(defined $cat1 && defined $cat2 && $cat1 == $cat2,
    '(b) second seal appended to the live catalog root in place')
    or diag('seal1 catalog blk ' . ($cat1 // 'none') . ', seal2 catalog blk ' . ($cat2 // 'none'));

my @clear = grep {
    in_phase($_, 'vacuum') && others_of_kind($_, PAGE_LIVE) && others_of_kind($_, PAGE_SEGCAT)
} @multi;
cmp_ok(scalar @clear, '>', 0, '(c) VACUUM tombstone emitted a LIVE+catalog+metapage record');

$node->stop;
bm25_check_logs($node);
done_testing();
