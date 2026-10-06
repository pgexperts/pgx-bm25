use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# CI-ONLY: the PostgreSQL::Test perl modules are not in the local build's @INC;
# this runs only under CI (pg_regress --tap-tests / prove on PGDG). It cannot be
# executed by the local `make installcheck PROVE_TESTS=` workflow.
#
# Exercises Phase-3 crashed-seal ORPHAN reclamation. A seal builds its segment as
# orphan pages and then publishes it in ONE Generic-WAL record (the two-phase install,
# ADR 0004). A crash between the two leaves pages the seal wrote that no live chain
# references -- orphans -- which a later VACUUM (amvacuumcleanup ->
# bm25_reclaim_orphans) stamps DELETED and records to the FSM, where the
# GetFreeIndexPage-first allocator (bm25_page_alloc) reuses them on the next seal
# instead of always extending the relation.
#
# Both sides are produced deterministically before a single crash:
#   - bm25_debug_seal_unpublished runs a seal's build phase over the pending list and
#     stops before the publish record, leaving exactly the crashed seal's orphans;
#   - bm25_seal then seals the same pending documents for real;
#   - pg_switch_wal() flushes WAL, and only then does stop('immediate') run.
# The flush is what makes recovery see any of it. bm25_seal and the debug build assign
# no xid, so their commits flush no WAL (issue #226). This suite used to stop right
# after the seal: recovery replayed none of it (0 segments, all 2000 docs still
# pending), and its only "orphans" were zero pages the lost seal's relation extension
# had left, never the written-but-unpublished pages described above.
# pg_switch_wal() XLogFlush()es its XLOG_SWITCH record and so everything before it. A
# commit is not a flush: even an xid-bearing one (a bare txid_current(), which this
# suite used once) flushes only when its OWN transaction wrote WAL, because
# RecordTransactionCommit samples wrote_xlog = (XactLastRecEnd != 0) before writing
# the commit record and XactLastRecEnd is reset at every transaction end; otherwise it
# commits asynchronously. A CHECKPOINT would flush too, but it would write the pages
# out and leave recovery nothing to replay.
#
# Asserts what is TRUE in Phase 3 (NOT a strict after<=before page count — PG index
# relations never truncate on VACUUM, so npages is monotonic and a reseal that
# extends would make after>before; we assert correctness + bounded growth instead):
#   (a) recovery replayed the seal (one segment, nothing pending) and the orphan build
#       (the orphan's header block is a segment page, not a zero page), and queries
#       are correct (all docs match, none twice);
#   (b) VACUUM reclaims the orphan -- its header is stamped DELETED -- and the index
#       stays queryable;
#   (c) a second insert+seal+vacuum cycle keeps page growth bounded (after2 < 2.5*after1;
#       2 cycles ~= 2x data, comfortably under — proves freed orphans/pending are reused).

my $node = PostgreSQL::Test::Cluster->new('orphan');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres',
    'CREATE TABLE docs (id int primary key, body text)');
$node->safe_psql('postgres',
    'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');
$node->safe_psql('postgres',
    "INSERT INTO docs SELECT g, 'crash token ' || (g % 50) FROM generate_series(1,2000) g");

# Page kinds from bm25_format.h: a segment header page is BM25_PAGE_SEGCAT, and the
# orphan sweep ORs BM25_PAGE_DELETED into whatever it frees.
my ($PAGE_SEGCAT, $PAGE_DELETED) = (4, 512);

# The crashed seal's orphans, then the real seal of the same documents, then the flush
# (see the header for why each is needed).
my $orphan = $node->safe_psql('postgres', "SELECT bm25_debug_seal_unpublished('docs_bm25')");
like($orphan, qr/^\d+$/, "seal build without publish left an orphan segment (header block $orphan)");
$node->safe_psql('postgres', "SELECT bm25_seal('docs_bm25')");
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;     # crash recovery replays Generic WAL

# (a) Recovery replayed both writes. nsegs and pending_ndocs change only in the seal's
# publish record; the orphan header's page kind exists only if its page-init record
# was replayed (a lost build leaves a zero page, flags 0, from relation extension).
is($node->safe_psql('postgres',
        "SELECT nsegs || '/' || pending_ndocs FROM bm25_stats('docs_bm25')"),
    '1/0', 'recovery replayed the seal: one published segment, nothing left pending');
is($node->safe_psql('postgres', "SELECT bm25_debug_page_flags('docs_bm25', $orphan)"),
    $PAGE_SEGCAT, "recovery replayed the orphan build: block $orphan is an unpublished segment header");

# safe_psql dies on any error, so reaching the assertion already proves no corruption;
# the count proves completeness, and that the orphan's copy of every document is not
# being read alongside the published one.
my $cnt = $node->safe_psql('postgres',
    "SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'token'");
is($cnt, 2000, 'all docs still match after crash-recover');

# (b) VACUUM reclaims the crashed seal's orphans and runs cleanly; index stays queryable.
my $before = $node->safe_psql('postgres',
    "SELECT bm25_debug_npages('docs_bm25')");
$node->safe_psql('postgres', 'VACUUM docs');
my $after = $node->safe_psql('postgres',
    "SELECT bm25_debug_npages('docs_bm25')");
is($node->safe_psql('postgres',
        "SELECT (bm25_debug_page_flags('docs_bm25', $orphan) & $PAGE_DELETED) <> 0"),
    't', "VACUUM reclaimed the orphan: block $orphan is stamped DELETED");
my $cnt_after_vac = $node->safe_psql('postgres',
    "SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'token'");
is($cnt_after_vac, 2000, 'index still queryable after VACUUM (all docs match)');
ok($after >= 0, "VACUUM completed; npages reported ($before -> $after)");

# (c) A second insert+seal+vacuum cycle must reuse freed pages (bounded growth).
#     Burn two xids first. As of issue #135 the drained pending pages this VACUUM
#     freed -- via the seal's bm25_pending_truncate, or via bm25_reclaim_orphans for
#     whatever the crash stranded -- carry a real retire_xid, and that stamp is a
#     ReadNextFullTransactionId(): an xid not yet assigned when it was written.
#     Neither VACUUM nor the read-only counts above assign one, so without this the
#     INSERT below would receive the very xid that was stamped, find every candidate
#     still horizon-blocked, and extend for its whole pending chain -- eating most of
#     the 2.5x margin and quietly voiding what this step claims to prove.
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres',
    "INSERT INTO docs SELECT g, 'more token ' || (g % 50) FROM generate_series(2001,4000) g");
$node->safe_psql('postgres', "SELECT bm25_seal('docs_bm25')");
$node->safe_psql('postgres', 'VACUUM docs');
my $after2 = $node->safe_psql('postgres',
    "SELECT bm25_debug_npages('docs_bm25')");
ok($after2 < 2.5 * $after, "page growth bounded across cycles ($after2 < 2.5*$after)");

# Final correctness: both batches (4000 docs) match after the second cycle.
my $cnt_final = $node->safe_psql('postgres',
    "SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'token'");
is($cnt_final, 4000, 'all 4000 docs match after second seal+vacuum cycle');

$node->stop;
bm25_check_logs($node);
done_testing();
