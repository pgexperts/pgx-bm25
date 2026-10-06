use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# v4 crash recovery: a crash mid-seal / mid-merge must preserve the per-index
# field-config page (BM25_PAGE_FIELDCFG, rooted at meta->field_config_blkno) and the
# metapage analyzer_fingerprint. The field-config page is written once at CREATE
# INDEX and never rewritten, and the metapage flip is atomic (two-phase commit), so
# recovery from an immediate stop leaves both intact whether or not the last
# seal/merge published. We then VACUUM and confirm bm25_reclaim_orphans reclaims the
# crash orphans WITHOUT freeing the field-config page (fingerprint unchanged,
# relation bounded). Mirrors t/004_crash_seal.pl for the v4 format.
#
# Both crash outcomes are produced deterministically before a single crash (issue
# #226): a merge that really merges and publishes (four segments in one size layer,
# retired in its swap record), and a seal that built its segment and never published
# it (bm25_debug_seal_unpublished: the orphans a crash between build and publish
# leaves). Then pg_switch_wal(), and only then stop('immediate'). bm25_seal, bm25_merge
# and the debug build assign no xid, so their commits flush no WAL; this suite used to
# fire its last merge asynchronously and crash, and recovery replayed neither that
# merge (it had nothing to do anyway) nor the publish record of the synchronous one
# before it. pg_switch_wal() XLogFlush()es its XLOG_SWITCH record and so everything
# before it, and unlike a CHECKPOINT it leaves recovery the records to replay. A bare
# txid_current() is NOT a flush: a commit flushes only when its own transaction wrote
# WAL (RecordTransactionCommit samples wrote_xlog = (XactLastRecEnd != 0) before the
# commit record, and XactLastRecEnd is reset at every transaction end), so an
# xid-only transaction commits asynchronously.

my $node = PostgreSQL::Test::Cluster->new('v4_crash');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'negligent defendant storage term' || (g % 9) FROM generate_series(1, 3000) g});
$node->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (body) WITH (analyzer = 'english', stopwords = 'default')});

# Capture the analyzer fingerprint and field_count written at build time.
my $fp_before = $node->safe_psql('postgres',
	qq{SELECT bm25_debug_analyzer_fingerprint('docs_bm25')});
my $fc_before = $node->safe_psql('postgres',
	qq{SELECT field_count FROM bm25_stats('docs_bm25')});
ok($fp_before ne '0' && $fp_before ne '', "v4 fingerprint is set and non-zero before crash");
is($fc_before, '1', "field_count is 1 (single default field) before crash");

# Four 250-document segments share one size layer (bm25_merge_layer_of: floor(log4
# ndocs) = 3), which is BM25_MERGE_LAYER_FANOUT, so the bm25_merge below really merges
# them: one output segment, four retired ranges. Two 500-document seals never reached
# the fanout, and bm25_merge only sealed.
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database alpha negligent' FROM generate_series(3001, 3250) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database alpha negligent' FROM generate_series(3251, 3500) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database beta defendant' FROM generate_series(3501, 3750) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'database beta defendant' FROM generate_series(3751, 4000) g});

# The crashed seal's orphans, built from the last batch while it is still pending;
# the merge then seals that batch for real and merges the four. Page kinds from
# bm25_format.h: a segment header is BM25_PAGE_SEGCAT, and the orphan sweep ORs
# BM25_PAGE_DELETED into what it frees.
my ($PAGE_SEGCAT, $PAGE_DELETED) = (4, 512);
my $orphan = $node->safe_psql('postgres', qq{SELECT bm25_debug_seal_unpublished('docs_bm25')});
like($orphan, qr/^\d+$/, "seal build without publish left an orphan segment (header block $orphan)");
$node->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;

# 0. Recovery replayed both. nsegs, pending_ndocs and the retired-range count change
#    only in the merge's publish and swap records; the orphan header's page kind
#    exists only if its page-init record was replayed (a lost build leaves a zero
#    page from relation extension, flags 0).
is($node->safe_psql('postgres',
		qq{SELECT nsegs || '/' || pending_ndocs || '/' || bm25_debug_retired_count('docs_bm25') FROM bm25_stats('docs_bm25')}),
	'2/0/4', 'recovery replayed the merge: 2 segments, nothing pending, 4 retired ranges');
is($node->safe_psql('postgres', qq{SELECT bm25_debug_page_flags('docs_bm25', $orphan)}),
	$PAGE_SEGCAT, "recovery replayed the orphan build: block $orphan is an unpublished segment header");

# 1. Fingerprint survived the crash unchanged: the field-config page and metapage
#    fingerprint were not corrupted or reclaimed.
my $fp_after = $node->safe_psql('postgres',
	qq{SELECT bm25_debug_analyzer_fingerprint('docs_bm25')});
is($fp_after, $fp_before, "analyzer fingerprint unchanged across crash recovery");

# 2. Every 'database' doc (the 1000 rows in batches 3001-4000 carry it) still matches
#    after recovery (no loss, no duplicate), and the fingerprint gate does not error
#    (index fp == query fp).
my $db_match = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
is($db_match, '1000', "all 1000 'database' docs match after recovery (no loss/dup)");

# 3. A doc indexed with 'negligent' matches an INFLECTED query 'negligence' that can
#    ONLY match via stemming (both english-stem to 'neglig'; the literal strings
#    differ). This is discriminating: without a surviving analyzer the un-stemmed
#    'negligence' would not equal the indexed 'negligent' token and count would be 0.
my $neg_match = $node->safe_psql('postgres',
	qq{SELECT count(*) > 0 FROM docs WHERE body \@\@\@ 'negligence'});
is($neg_match, 't', "inflected 'negligence' query matches indexed 'negligent' after recovery (stemming survived)");

# 4. VACUUM reclaims crash orphans WITHOUT freeing the field-config page: the
#    fingerprint is still readable + unchanged afterward, and the relation does not
#    grow across a second VACUUM (bounded).
#
#    The txid_current() between the two VACUUMs is load-bearing, and the $pages2 <=
#    $pages1 comparison below has ZERO slack. Freed pending pages -- the merge's
#    drain recycled its chain through bm25_pending_truncate before the crash, and
#    bm25_reclaim_orphans frees any a crash strands -- are stamped with a real
#    retire_xid as of issue #135, and that stamp is a ReadNextFullTransactionId(): an
#    xid not yet assigned when written. Neither VACUUM nor a read-only SELECT assigns
#    an xid, so without a burn nextXid never moves, the horizon never passes the
#    stamp, and any allocation VACUUM #2 makes (its opportunistic merge_maybe, say)
#    has to EXTEND instead of reusing -- failing this assertion on a healthy index.
$node->safe_psql('postgres', 'VACUUM docs');
my $pages1 = $node->safe_psql('postgres', qq{SELECT bm25_debug_npages('docs_bm25')});
is($node->safe_psql('postgres',
		qq{SELECT (bm25_debug_page_flags('docs_bm25', $orphan) & $PAGE_DELETED) <> 0}),
	't', "VACUUM reclaimed the crash orphan: block $orphan is stamped DELETED");
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'VACUUM docs');
my $pages2 = $node->safe_psql('postgres', qq{SELECT bm25_debug_npages('docs_bm25')});
my $fp_vac = $node->safe_psql('postgres',
	qq{SELECT bm25_debug_analyzer_fingerprint('docs_bm25')});
is($fp_vac, $fp_before, "fingerprint unchanged after VACUUM (field-config page not reclaimed)");
ok($pages2 <= $pages1, "relation does not grow across repeated VACUUM (bounded)");

# 5. Match set stable after VACUUM (reused pages carry no stale postings, field-config
#    page intact so the scan gate still passes).
my $db_match2 = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
is($db_match2, '1000', "'database' match count stable after VACUUM");

$node->stop;
bm25_check_logs($node);
done_testing();
