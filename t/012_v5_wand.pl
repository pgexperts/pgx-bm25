use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# M2b v5 WAND crash + replica: v5's only new on-disk surface over v4 is the
# per-block, per-field impact table (BM25BlockImpact: max_tf/min_doclen) that the
# block-max WAND driver prunes against. It is written by the SAME segment writer
# ambuild/bm25_seal/bm25_merge already share (bm25_seg_build.c:encode_block), so it
# is plain Generic-WAL page bytes -- no custom rmgr, no shared_preload -- exactly
# like the v4 postings this suite's siblings (t/006, t/007) already proved durable.
# What is genuinely new here, and what 43_wand_parity.sql/44_wand_skip.sql cannot
# prove because they only run against a single freshly-sealed process, is that the
# WAND-pruned ranked path stays bit-identical to the exhaustive path (a) across a
# crash mid-seal/mid-merge and (b) on a streaming replica -- i.e. that recovery and
# WAL replay reproduce the impact bytes precisely enough that pruning decisions
# taken from them still land on the exact top-k the exhaustive scorer computes.
#
# Parity alone is not enough, though: a WAND driver that silently fell back to
# decoding every block (never actually pruning) would still pass every parity
# check above, because a full decode is also correct -- just slow. It would
# only be "wrong" in that the headline block-max pruning feature had been
# disabled, and parity cannot see that. So each scenario also asserts
# bm25_wand_stats(...).blocks_skipped > 0 -- the same anti-neuter witness
# sql/44_wand_skip.sql's Gate 1 established -- immediately after each parity
# check, proving pruning genuinely fires (not just "produces the right
# answer") at that point in the crash/replica timeline.
#
# Two independent scenarios, each mirroring the M4 harness shape exactly
# (t/006_v4_crash.pl for crash, t/007_v4_replica.pl for replica), plus a WAND-vs-
# exhaustive parity check (the array_agg-over-ordered-subquery idiom from
# sql/44_wand_skip.sql -- both paths drain the same score-desc/tid-asc comparator,
# so no id tie-break is needed there) folded into each:
#
#   1. Crash node:  build a multi-segment v5 index over a corpus sized so WAND
#      actually prunes, leave it holding both crash outcomes -- a merge that
#      really merged and published, and a seal that built its segment and never
#      published it -- snapshot ranked + WAND==exhaustive + blocks_skipped>0,
#      crash (immediate stop), restart, and assert all three are unchanged -- the
#      post-recovery blocks_skipped>0 check is the load-bearing one, since it is
#      the only assertion that would fail if recovery replayed impact bytes
#      correct enough for the right answer but somehow left the scan defaulting
#      to a non-pruning path.
#
#      Both outcomes are produced deterministically (issue #226). This scenario
#      used to fire its last merge asynchronously and crash without waiting; but
#      bm25_seal/bm25_merge assign no xid, so their commits flush no WAL, and
#      recovery replayed neither that merge (it had nothing to do anyway) nor the
#      publish record of the synchronous one before it, which had only sealed:
#      recovery came back with that batch still in the pending list, and no merged
#      impact table ever existed.
#      Now the merge runs to completion, bm25_debug_seal_unpublished leaves the
#      orphans a crash between a seal's build and its publish record leaves, and
#      pg_switch_wal() -- which XLogFlush()es its XLOG_SWITCH record and so
#      everything before it, unlike a CHECKPOINT without writing the pages out --
#      makes recovery replay both. (A bare txid_current() is NOT a flush: a commit
#      flushes only when its own transaction wrote WAL, since RecordTransactionCommit
#      samples wrote_xlog = (XactLastRecEnd != 0) before the commit record and
#      XactLastRecEnd is reset at every transaction end.)
#   2. Primary/standby: build the same shape of index, assert the ranked list,
#      WAND==exhaustive parity, and blocks_skipped>0 are identical/hold on both
#      nodes -- the standby-side blocks_skipped>0 check is load-bearing for the
#      same reason: it is the only assertion a replicated-but-not-pruning scan
#      would fail.

# Corpus shape (shared by both scenarios, scaled down from sql/44_wand_skip.sql's
# proven 20000-doc version): 1-in-N docs carry the high-idf 'rareterm', the rest
# carry 'common'; a 0-4 token 'pad' tail varies doc length across docs so blocks
# differ in max_impact and the WAND driver has real pruning to do (not merely
# "correct because every block looks the same").
sub corpus_sql
{
	my ($table, $lo, $hi, $modulus) = @_;
	return qq{INSERT INTO $table SELECT g,
	            concat_ws(' ', CASE WHEN g % $modulus = 0 THEN 'rareterm' ELSE 'common' END,
	                           repeat('pad ', g % 5))
	          FROM generate_series($lo, $hi) g};
}

# Ranked query used for crash-durability / replica-equality comparisons: the WAND
# driver is the DEFAULT path (bm25_native.wand_top_k=100 compiled-in default), so this
# exercises WAND without setting the GUC at all. 'id' is a secondary tie-break
# (t/007/t/010/t/011's style) so the returned LIST is reproducible even where two
# docs land on an identical score; enable_seqscan=off + a leading &@@ forces the
# ordered bm25_native index scan (the only path that can even evaluate &@@).
sub ranked_sql
{
	my ($table, $limit) = @_;
	return qq{SET enable_seqscan=off; SELECT id FROM $table WHERE body \@\@\@ 'common rareterm' ORDER BY body &\@\@ 'common rareterm', id LIMIT $limit};
}

# WAND-vs-exhaustive parity for one GUC setting: an array_agg over an ORDER BY ...
# LIMIT subquery, with NO id tie-break -- both the WAND heap and the exhaustive
# scorer drain the identical (score desc, tid asc) comparator (bm25_scan_rank.c's
# scored_desc / the BM25TopK comparator), so their outputs agree exactly even on
# ties. This is the same idiom sql/44_wand_skip.sql already uses and passes CI
# with (Task 11); reused verbatim rather than reinvented.
sub topk_ids_sql
{
	my ($table, $wand_top_k, $limit) = @_;
	return qq{SET enable_seqscan=off; SET bm25_native.wand_top_k=$wand_top_k;
	          SELECT array_agg(id) FROM (SELECT id FROM $table WHERE body \@\@\@ 'common rareterm'
	            ORDER BY body &\@\@ 'common rareterm' LIMIT $limit) s};
}

# Anti-neuter probe (mirrors sql/44_wand_skip.sql / sql/45_m2b_acceptance.sql): a
# WAND that decodes every block still lands on the correct top-k, so the parity
# checks above cannot tell a pruning driver from a neutered one. This witnesses
# that pruning actually FIRED: (blocks_skipped + deep_check_skips) > 0.
# k=5 -- NOT the query's LIMIT -- is load-bearing: with the mutually-exclusive
# 'common'/'rareterm' terms, k must be BELOW the ~20 seeded 'rareterm' docs so the
# top-k threshold is set by their high-idf scores; only then do the many
# low-scoring 'common' blocks fall below it and get skipped. Measured on this exact
# corpus: k=5 yields blocks_skipped = 11 (4500-doc crash node) / 4 (3000-doc
# replica), whereas k=30 prunes NOTHING (docs_scored == all matches). deep_check_skips
# stays 0 because the two terms never co-occur in one doc, so the sum is really
# blocks_skipped; the sum form just keeps the gate robust if the corpus shape changes.
sub pruning_skips_sql
{
	my ($index) = @_;
	return qq{SELECT blocks_skipped + deep_check_skips FROM bm25_wand_stats('$index', 'common rareterm', 5)};
}

# ---------------------------------------------------------------------------
# Scenario 1: crash recovery over Generic WAL.
# ---------------------------------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('v5_wand_crash');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');

# Base build: 4000 docs via ambuild, which seals exactly one segment through the
# same segment writer bm25_seal/bm25_merge use (bm25_build.c), so the impact table
# is already present without a separate seal call.
$node->safe_psql('postgres', corpus_sql('docs', 1, 4000, 225));
$node->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

my $fc_before = $node->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_before, '1', "field_count is 1 (single default field) before crash");

# Grow to four more sealed segments, then merge them, so recovery must reassemble a
# consistent multi-segment impact-bearing tree whose merged segment's impact table
# was re-derived by the merge (mirrors t/006_v4_crash.pl's growth shape). Four
# 125-document segments share one size layer (bm25_merge_layer_of: floor(log4 ndocs)
# = 3), which is BM25_MERGE_LAYER_FANOUT, so the bm25_merge really merges them: one
# output, four retired ranges. (A 300- and a 200-document seal never reached the
# fanout, and bm25_merge only sealed.)
$node->safe_psql('postgres', corpus_sql('docs', 4001, 4125, 225));
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres', corpus_sql('docs', 4126, 4250, 225));
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres', corpus_sql('docs', 4251, 4375, 225));
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres', corpus_sql('docs', 4376, 4500, 225));

# The crashed seal's orphans, built from the last batch while it is still pending --
# impact table and all; the merge then seals that batch for real and merges the four.
# Page kind from bm25_format.h: a segment header is BM25_PAGE_SEGCAT.
my $PAGE_SEGCAT = 4;
my $orphan = $node->safe_psql('postgres', qq{SELECT bm25_debug_seal_unpublished('docs_bm25')});
like($orphan, qr/^\d+$/, "seal build without publish left an orphan segment (header block $orphan)");
$node->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});

# Snapshot state before the crash: match counts (correctness), the WAND-driven
# ranked id list (crash-durability target), and WAND==exhaustive parity.
my $rare_before = $node->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'rareterm'});
is($rare_before, '20', "'rareterm' matches exactly the 20 seeded docs before crash");

my $all_before = $node->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'common rareterm'});
is($all_before, '4500', "'common rareterm' matches all 4500 docs before crash");

my $ranked_before = $node->safe_psql('postgres', ranked_sql('docs', 30));

my $wand_before = $node->safe_psql('postgres', topk_ids_sql('docs', 100, 30));
my $exh_before  = $node->safe_psql('postgres', topk_ids_sql('docs', 0, 30));
is($wand_before, $exh_before, "WAND (wand_top_k=100) matches exhaustive (wand_top_k=0) before crash");

# Anti-neuter: pruning must genuinely fire before the crash too, or the
# post-crash check below would just be re-confirming a scan that never
# pruned in the first place.
my $skipped_before = $node->safe_psql('postgres', pruning_skips_sql('docs_bm25'));
cmp_ok($skipped_before, '>', 0, "WAND pruning fires before crash (pruning skips > 0)");

# Flush, then crash (see the scenario description for why the flush is needed).
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;

# 0. Recovery replayed both outcomes. nsegs, pending_ndocs and the retired-range
#    count change only in the merge's publish and swap records; the orphan header's
#    page kind exists only if its page-init record was replayed (a lost build leaves
#    a zero page from relation extension, flags 0).
is($node->safe_psql('postgres',
		qq{SELECT nsegs || '/' || pending_ndocs || '/' || bm25_debug_retired_count('docs_bm25') FROM bm25_stats('docs_bm25')}),
	'2/0/4', 'recovery replayed the merge: 2 segments, nothing pending, 4 retired ranges');
is($node->safe_psql('postgres', qq{SELECT bm25_debug_page_flags('docs_bm25', $orphan)}),
	$PAGE_SEGCAT, "recovery replayed the orphan build: block $orphan is an unpublished segment header");

# 1. field_count and match correctness survived the crash unchanged.
my $fc_after = $node->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_after, $fc_before, "field_count unchanged across crash recovery");

my $rare_after = $node->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'rareterm'});
is($rare_after, '20', "'rareterm' match count stable after crash recovery");

my $all_after = $node->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'common rareterm'});
is($all_after, '4500', "'common rareterm' match count stable after crash recovery");

# 2. Crash durability: the WAND-driven (default wand_top_k=100) ranked id list is
#    byte-identical before and after recovery -- the impact table that pruning
#    decisions rest on replayed consistently, not merely "recovery didn't crash".
my $ranked_after = $node->safe_psql('postgres', ranked_sql('docs', 30));
is($ranked_after, $ranked_before, "WAND-driven ranked id list unchanged across crash recovery");

# 3. WAND == exhaustive still holds post-recovery: pruning correctness survives
#    recovery, not just a fresh build (what 44_wand_skip.sql already covers).
my $wand_after = $node->safe_psql('postgres', topk_ids_sql('docs', 100, 30));
my $exh_after  = $node->safe_psql('postgres', topk_ids_sql('docs', 0, 30));
is($wand_after, $exh_after, "WAND matches exhaustive after crash recovery");
is($wand_after, $wand_before, "WAND-driven top-k array unchanged across crash recovery");

# 4. Load-bearing anti-neuter check: parity (2/3 above) cannot tell a real
#    block-max pruning driver apart from a fallback that decodes every block
#    and happens to still compute the right top-k -- both pass is()/parity
#    identically. blocks_skipped > 0 here is the only assertion in this
#    scenario that would catch recovery replaying impact bytes well enough
#    for correctness but leaving the scan silently defaulting to a full,
#    non-pruning decode.
my $skipped_after = $node->safe_psql('postgres', pruning_skips_sql('docs_bm25'));
cmp_ok($skipped_after, '>', 0, "WAND pruning still fires after crash recovery (pruning skips > 0)");

$node->stop;

# ---------------------------------------------------------------------------
# Scenario 2: streaming replica equality.
# ---------------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('v5_wand_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$primary->safe_psql('postgres', corpus_sql('docs', 1, 3000, 150));
$primary->safe_psql('postgres', 'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

# Anti-neuter: confirm pruning fires on the primary BEFORE the backup is taken,
# so the standby-side check below is comparing against a scan that was
# actually pruning, not one that happened to never skip anything to begin with.
my $skipped_primary_prebackup = $primary->safe_psql('postgres', pruning_skips_sql('docs_bm25'));
cmp_ok($skipped_primary_prebackup, '>', 0, "WAND pruning fires on primary before standby backup (pruning skips > 0)");

my $backup_name = 'v5_wand_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('v5_wand_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# 1. field_count identical on both nodes (metapage + impact-bearing segment
#    replicated verbatim).
my $fc_primary = $primary->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
my $fc_standby = $standby->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_standby, $fc_primary, "field_count identical on primary and standby");
is($fc_primary, '1', "field_count is 1 (single default field)");

# 2. Replica match: the WAND-driven (default wand_top_k=100) ranked id list is
#    byte-identical on both nodes -- the impact table WAND prunes against
#    replicated exactly, and the pruning decisions it drives reproduce.
my $ranked_primary = $primary->safe_psql('postgres', ranked_sql('docs', 30));
my $ranked_standby = $standby->safe_psql('postgres', ranked_sql('docs', 30));
is($ranked_standby, $ranked_primary, "WAND-driven ranked id list identical on primary and standby");

my $rare_primary = $primary->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'rareterm'});
my $rare_standby = $standby->safe_psql('postgres', qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'rareterm'});
is($rare_primary, '20', "'rareterm' matches exactly the 20 seeded docs on primary");
is($rare_standby, $rare_primary, "'rareterm' match count identical on standby");

# 3. WAND == exhaustive on BOTH nodes: pruning correctness survives replication,
#    not just a fresh build on one process.
my $wand_primary = $primary->safe_psql('postgres', topk_ids_sql('docs', 100, 30));
my $exh_primary  = $primary->safe_psql('postgres', topk_ids_sql('docs', 0, 30));
is($wand_primary, $exh_primary, "WAND matches exhaustive on primary");

my $wand_standby = $standby->safe_psql('postgres', topk_ids_sql('docs', 100, 30));
my $exh_standby  = $standby->safe_psql('postgres', topk_ids_sql('docs', 0, 30));
is($wand_standby, $exh_standby, "WAND matches exhaustive on standby");
is($wand_standby, $wand_primary, "WAND-driven top-k array identical on primary and standby");

# 4. Load-bearing anti-neuter check: the replica-equality checks above cannot
#    distinguish a real block-max pruning driver from a fallback that decodes
#    every block and still computes the identical top-k -- both pass every
#    is() above unchanged. blocks_skipped > 0 on the STANDBY, after
#    wait_for_catchup, is the only assertion here that would catch replicated
#    impact bytes that are correct enough for the right answer but leave the
#    standby's scan silently defaulting to a full, non-pruning decode.
my $skipped_standby = $standby->safe_psql('postgres', pruning_skips_sql('docs_bm25'));
cmp_ok($skipped_standby, '>', 0, "WAND pruning fires on standby after catchup (pruning skips > 0)");

$standby->stop;
$primary->stop;
bm25_check_logs($node, $primary, $standby);
done_testing();
