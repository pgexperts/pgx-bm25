use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# M5 crash recovery: a crash mid-seal / mid-merge over a MULTI-FIELD index WITH
# key_field must preserve (a) each field's postings, (b) the per-field-boosted BM25F
# ranking, and (c) the docid->key (KEYMAP) mapping so results still return the user id.
# The metapage flip is atomic (two-phase commit) and the field-config + per-segment
# keymap pages are written by the orphan builder before the committing flip, so recovery
# from an immediate stop leaves a consistent index whether or not the last seal/merge
# published. We then VACUUM and confirm bm25_reclaim_orphans reclaims the crash orphans
# (keymap pages included) WITHOUT freeing live keymap/field pages (results + ranking
# unchanged, relation bounded). Mirrors t/006_v4_crash.pl for the multi-field /
# key_field surface.
#
# Both crash outcomes are produced deterministically before a single crash (issue
# #226): a merge that really merges and publishes (four segments in one size layer,
# retired in its swap record), and a seal that built its segment -- keymap chain and
# all -- and never published it (bm25_debug_seal_unpublished: the orphans a crash
# between build and publish leaves). Then pg_switch_wal(), and only then
# stop('immediate'). bm25_seal, bm25_merge and the debug build assign no xid, so their
# commits flush no WAL; this suite used to fire its last merge asynchronously and
# crash, and recovery replayed neither that merge (it had nothing to do anyway) nor
# the publish record of the synchronous one before it. pg_switch_wal() XLogFlush()es
# its XLOG_SWITCH record and so everything before it, and unlike a CHECKPOINT it
# leaves recovery the records to replay. A bare txid_current() is NOT a flush: a
# commit flushes only when its own transaction wrote WAL (RecordTransactionCommit
# samples wrote_xlog = (XactLastRecEnd != 0) before the commit record, and
# XactLastRecEnd is reset at every transaction end), so an xid-only transaction
# commits asynchronously.

my $node = PostgreSQL::Test::Cluster->new('m5_crash');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres',
	'CREATE TABLE docs(id int primary key, title text, body text)');
# TITLE carries a per-batch high-idf term (boosted 5:1); BODY carries a shared term in
# every row for a stable ranking + a per-row discriminator. key_field='id' is carried as
# an INCLUDE column (amcaninclude), so its value reaches the build callback without a
# bm25_native opclass; the tokenized fields are (title, body).
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'alpha', 'storage defendant term' || (g % 9) FROM generate_series(1, 3000) g});
$node->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id) WITH (key_field='id', boost_title='5.0', boost_body='1.0')});

# field_count reflects the two indexed content fields (title, body); key_field is the id
# column, not a scored field.
my $fc_before = $node->safe_psql('postgres',
	qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_before, '2', "field_count is 2 (title, body) before crash");

# A bare (BM25F) 'alpha' query returns the user id (key_field), not ctid. The ranked
# &@@ form forces the bm25_native index scan (only it can compute the &@@ order).
my $alpha_before = $node->safe_psql('postgres',
	qq{SELECT array_agg(id) FROM (SELECT id FROM docs WHERE title \@\@\@ 'alpha' ORDER BY title &\@\@ 'alpha', id LIMIT 5) s});

# Four 250-document segments share one size layer (bm25_merge_layer_of: floor(log4
# ndocs) = 3), which is BM25_MERGE_LAYER_FANOUT, so the bm25_merge below really merges
# them: one output segment, four retired ranges. Two 500-document seals never reached
# the fanout, and bm25_merge only sealed.
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'beta', 'database negligence' FROM generate_series(3001, 3250) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'beta', 'database negligence' FROM generate_series(3251, 3500) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'gamma', 'database defendant' FROM generate_series(3501, 3750) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'gamma', 'database defendant' FROM generate_series(3751, 4000) g});

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

# 1. field_count and the multi-field structure survived the crash.
my $fc_after = $node->safe_psql('postgres',
	qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_after, '2', "field_count unchanged across crash recovery");

# 2. The BM25F ranked id list (returned by key_field id) is unchanged after recovery:
#    postings + per-field boosts + keymap all replayed consistently.
my $alpha_after = $node->safe_psql('postgres',
	qq{SELECT array_agg(id) FROM (SELECT id FROM docs WHERE title \@\@\@ 'alpha' ORDER BY title &\@\@ 'alpha', id LIMIT 5) s});
is($alpha_after, $alpha_before, "BM25F alpha ranking (returned by id key) unchanged across crash");

# 3. Every 'database' body doc (the 1000 rows in 3001-4000) still matches -- no loss/dup.
my $db_match = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'database'});
is($db_match, '1000', "all 1000 'database' body docs match after recovery (no loss/dup)");

# 4. Field scoping survives: a 'title:alpha' scoped query (ranked &@@ form forces the
#    bm25_native index, which parses field:term) hits ONLY the 3000 title-alpha docs and none
#    of the beta/gamma/database body rows -- per-block field-RLE replayed correctly.
my $scoped = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ 'title:alpha' ORDER BY title &\@\@ 'title:alpha') s});
is($scoped, '3000', "field-scoped 'title:alpha' matches exactly the 3000 title-alpha docs after recovery");

# 5. VACUUM reclaims crash orphans WITHOUT freeing live keymap/field pages: the id-keyed
#    ranking is still returned correctly and the relation is bounded across a 2nd VACUUM
#    (keymap pages are not leaked).
#
#    Same zero-slack hazard as t/006_v4_crash.pl step 4, and the same fix: freed
#    pending pages now carry a real retire_xid (issue #135) whose stamp is a
#    not-yet-assigned xid, and neither VACUUM nor a read-only SELECT assigns one.
#    Burn two xids so the horizon can actually clear before VACUUM #2 allocates.
$node->safe_psql('postgres', 'VACUUM docs');
my $pages1 = $node->safe_psql('postgres', qq{SELECT bm25_debug_npages('docs_bm25')});
is($node->safe_psql('postgres',
		qq{SELECT (bm25_debug_page_flags('docs_bm25', $orphan) & $PAGE_DELETED) <> 0}),
	't', "VACUUM reclaimed the crash orphan: block $orphan is stamped DELETED");
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'SELECT txid_current()');
$node->safe_psql('postgres', 'VACUUM docs');
my $pages2 = $node->safe_psql('postgres', qq{SELECT bm25_debug_npages('docs_bm25')});
ok($pages2 <= $pages1, "relation does not grow across repeated VACUUM (bounded; keymap pages not leaked)");

my $alpha_vac = $node->safe_psql('postgres',
	qq{SELECT array_agg(id) FROM (SELECT id FROM docs WHERE title \@\@\@ 'alpha' ORDER BY title &\@\@ 'alpha', id LIMIT 5) s});
is($alpha_vac, $alpha_before, "id-keyed BM25F ranking stable after VACUUM (keymap page not reclaimed)");

$node->stop;
bm25_check_logs($node);
done_testing();
