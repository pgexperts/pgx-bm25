use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Crash recovery for the M2a segmented path: an immediate stop mid-seal / mid-merge
# must leave NO corruption and NO permanent leak. The two-phase commit invariant is
# what makes this safe — a crash before the final metapage flip leaves only orphan
# pages (the publish/swap record either committed atomically or did not), never a
# torn catalog. After recovery every doc must still match (no loss, no duplicate),
# the ranking must still be valid, and a VACUUM cycle must reclaim the crash orphans
# so the relation does not grow without bound. Mirrors t/002_crash.pl, extended to
# the multi-segment seal + merge paths (M2a lifts the M0/M1 single-page corpus limit).
#
# We cannot stop INSIDE the C seal from Perl, so both outcomes are produced
# deterministically before one crash: a merge that really merged and published (its
# swap record retires the four merged segments), and a seal that built its segment
# and never published it (bm25_debug_seal_unpublished: exactly the orphans a crash
# between build and publish leaves). Then pg_switch_wal(), and only then
# stop('immediate'). This suite used to fire its last merge asynchronously and crash
# without waiting, but bm25_seal/bm25_merge assign no xid, so their commits flush no
# WAL, and the immediate stop discarded that merge whole every time (issue #226):
# recovery never saw an in-flight operation, so no written-but-unpublished page ever
# existed for VACUUM to reclaim (at most a zero page from the lost merge's relation
# extension). pg_switch_wal() XLogFlush()es its XLOG_SWITCH record and so everything
# before it, and unlike a CHECKPOINT it leaves recovery the records to replay. A bare
# txid_current() is NOT a flush: a commit flushes only when its own transaction wrote
# WAL (RecordTransactionCommit samples wrote_xlog = (XactLastRecEnd != 0) before the
# commit record, and XactLastRecEnd is reset at every transaction end), so an
# xid-only transaction commits asynchronously.

my $node = PostgreSQL::Test::Cluster->new('crash_seal');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'term' || (g % 9) || ' database storage engine' FROM generate_series(1, 3000) g});
$node->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

# Build more segments + a merge so the index holds multiple sealed segments and
# retired ranges before the crash. Four 250-document segments share one size layer
# (bm25_merge_layer_of: floor(log4 ndocs) = 3), which is BM25_MERGE_LAYER_FANOUT, so
# the bm25_merge really merges them -- one output, four retired ranges. (Two
# 500-document seals never reached the fanout, and bm25_merge only sealed.)
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database extra alpha' FROM generate_series(3001, 3250) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database extra alpha' FROM generate_series(3251, 3500) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database extra beta' FROM generate_series(3501, 3750) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database extra beta' FROM generate_series(3751, 4000) g});
$node->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});

# Every row contains 'database', so the bm25_native match count must equal the heap row
# count after recovery — the loss/duplicate detector.
my $before = $node->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});

# Commit one more batch (so these docs are durable via the pending-list WAL -- the
# INSERT writes WAL under an xid, so its commit is synchronous and also flushes the
# merge above), then build it
# into a segment the way a seal would and stop before publishing: the crashed seal's
# orphans. Page kinds from bm25_format.h: a segment header is BM25_PAGE_SEGCAT, and
# the orphan sweep ORs BM25_PAGE_DELETED into what it frees.
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database extra gamma' FROM generate_series(4001, 4500) g});
my ($PAGE_SEGCAT, $PAGE_DELETED) = (4, 512);
my $orphan = $node->safe_psql('postgres', qq{SELECT bm25_debug_seal_unpublished('docs_bm25')});
like($orphan, qr/^\d+$/, "seal build without publish left an orphan segment (header block $orphan)");
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;

# Recovery replayed both outcomes. nsegs and the retired-range count change only in
# the merge's swap record, and the gamma batch is still pending because nothing
# published it; the orphan header's page kind exists only if its page-init record was
# replayed (a lost build leaves a zero page from relation extension, flags 0).
is($node->safe_psql('postgres',
		qq{SELECT nsegs || '/' || pending_ndocs || '/' || bm25_debug_retired_count('docs_bm25') FROM bm25_stats('docs_bm25')}),
	'2/500/4', 'recovery replayed the merge (2 segments, 4 retired ranges) and left gamma pending');
is($node->safe_psql('postgres', qq{SELECT bm25_debug_page_flags('docs_bm25', $orphan)}),
	$PAGE_SEGCAT, "recovery replayed the orphan build: block $orphan is an unpublished segment header");

# Recovery correctness: every 'database' doc still matches (the WAL-committed inserts
# survive via the pending list / sealed segments; the orphan's copy of gamma is not
# read alongside the pending one).
my $heap = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body LIKE '%database%'});
my $after = $node->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
is($after, $heap, 'all database docs match after crash recovery (no loss, no duplicate)');
cmp_ok($after, '>=', $before, 'post-crash count includes pre-crash committed docs');

# No-leak: VACUUM reclaims crash orphans, and a follow-up seal reuses what they
# freed, so a steady-state maintenance cycle does not grow the relation.
#
# The txid_current() calls are what actually cross the XID horizon. The comment that
# stood here said "two VACUUMs cross the XID horizon", which is not how it works:
# VACUUM assigns no transaction id, and neither does bm25_seal() or a read-only
# SELECT, so a run of them leaves nextXid exactly where it was and no horizon moves
# at all. Every horizon-gated page in this block -- the merge's retired ranges,
# and (as of issue #135) the drained pending pages -- needs at least one
# committed xid ABOVE its stamp before bm25_page_alloc will take it, because the
# stamp is a ReadNextFullTransactionId(), i.e. an xid not yet assigned when written.
my $size1 = $node->safe_psql('postgres', qq{SELECT pg_relation_size('docs_bm25')});
$node->safe_psql('postgres', 'VACUUM docs');
is($node->safe_psql('postgres',
		qq{SELECT (bm25_debug_page_flags('docs_bm25', $orphan) & $PAGE_DELETED) <> 0}),
	't', "VACUUM reclaimed the crash orphan: block $orphan is stamped DELETED");
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'VACUUM docs');
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'VACUUM docs');
$node->safe_psql('postgres', 'VACUUM docs');
my $size2 = $node->safe_psql('postgres', qq{SELECT pg_relation_size('docs_bm25')});
cmp_ok($size2, '<=', $size1 * 1.5, 'VACUUM reclaims crash orphans (relation size bounded)');

# Ranking still produces a valid top-5 id list after recovery.
my $rank = $node->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'database' ORDER BY body &\@\@ 'database', id LIMIT 5});
like($rank, qr/^\d+(\n\d+){0,4}$/, 'ranked top-5 returns valid ids post-crash');

$node->stop;
bm25_check_logs($node);
done_testing();
