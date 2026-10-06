use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# M4 crash recovery: a crash mid-seal / mid-merge over a MULTI-FIELD, key_field,
# POSITION-BEARING index must preserve, after recovery: (a) each field's postings +
# the POS chain, so phrase / proximity queries return the same docs; (b) the scored
# scan slot, so bm25_snippet still renders a highlighted excerpt; (c) the docid->key
# (KEYMAP) mapping, so results return the user id; and it must still merge cleanly.
# The POS chain is a plain Generic-WAL page chain (like the POST chain) built by the
# orphan builder before the committing metapage flip, so recovery from an immediate
# stop leaves a consistent tree whether or not the last seal/merge published.
# Mirrors t/008_m5_crash.pl, extended for the M4 positions / phrase / snippet surface.
#
# Both crash outcomes are produced deterministically before a single crash (issue
# #226): a merge that really merges and publishes (four segments in one size layer,
# retired in its swap record), and a seal that built a position-bearing segment from
# the unsealed rows and never published it (bm25_debug_seal_unpublished: the orphans
# a crash between build and publish leaves). Then pg_switch_wal(), and only then
# stop('immediate'). bm25_merge and the debug build assign no xid, so their commits
# flush no WAL; this suite used to fire its last merge asynchronously and crash, and
# recovery never replayed any of it. pg_switch_wal() XLogFlush()es its XLOG_SWITCH
# record and so everything before it, and unlike a CHECKPOINT it leaves recovery the
# records to replay. A bare txid_current() is NOT a flush: a commit flushes only when
# its own transaction wrote WAL (RecordTransactionCommit samples
# wrote_xlog = (XactLastRecEnd != 0) before the commit record, and XactLastRecEnd is
# reset at every transaction end), so an xid-only transaction commits asynchronously.

my $node = PostgreSQL::Test::Cluster->new('m4_crash');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres',
	'CREATE TABLE docs(id int primary key, title text, body text)');
# BODY carries the phrase 'product liability' as an ADJACENT bigram in every base row,
# padded by 'signal' so a phrase recheck must FILTER (positions decide the match, not
# co-occurrence). TITLE carries a boosted term. key_field='id' is an INCLUDE column
# (amcaninclude), so its value reaches the build callback; the tokenized fields are
# (title, body). Positions are ON by default (no store_positions=false).
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'alpha', 'signal product liability signal' FROM generate_series(1, 3000) g});
$node->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id) WITH (key_field='id', boost_title='5.0', boost_body='1.0')});

# field_count reflects the two indexed content fields (title, body).
my $fc_before = $node->safe_psql('postgres',
	qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_before, '2', "field_count is 2 (title, body) before crash");

# EXACT phrase "product liability" over the SEALED base segment: the adjacent bigram is
# present in all 3000 base rows, so it matches exactly 3000. The ranked &@@ form forces
# the bm25_native index scan (only it parses the phrase + runs the positional recheck); operators
# anchor on the LEADING indexed column (title), the phrase is a bare RHS literal (OR across
# fields, D6) so it lands in body. A COUNT over the inner forced scan is ORDER-FREE: the
# 3000 base docs share identical content -> tied scores, so an ordered id list would be
# nondeterministic (and a `, id` tiebreak after &@@ risks a PG16 bitmap scan that skips the
# recheck). Count is the portable, recheck-exercising invariant.
my $phrase_before = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ '"product liability"' ORDER BY title &\@\@ '"product liability"' LIMIT 100000) s});
is($phrase_before, '3000', "exact phrase 'product liability' matches all 3000 base docs before crash");

# A body hit renders a highlighted snippet under the active scored scan. The snippet reads
# the ACTIVE scored-scan slot, so the query runs inside the bm25_native ordered index scan: @@@
# key + &@@ order-by on the SAME literal, anchored on the leading column, with NO secondary
# sort key and NO id predicate (a `, id` tiebreak risks a PG16 bitmap scan, and an `id=n`
# predicate would steal the plan to docs_pkey -> either yields a NULL snippet). Every base
# body is identical, so LIMIT 1 returns a stable snippet regardless of which tied row wins.
my $snip_before = $node->safe_psql('postgres',
	qq{SELECT bm25_snippet(body, '<mark>', '</mark>', 300) FROM docs WHERE title \@\@\@ 'product' ORDER BY title &\@\@ 'product' LIMIT 1});
like($snip_before, qr/<mark>product<\/mark>/, "snippet renders a highlighted body hit before crash");

# Positions round-trip through seal: bm25_debug_seg_positions dumps (seg, local_docid,
# field_id, pos) for the term. 'product' sits at body position 1 in every base row, so
# the position multiset is non-empty and every 'product' posting reports pos=1.
my $pos_before = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM bm25_debug_seg_positions('docs_bm25', 'product') WHERE pos = 1});
ok($pos_before > 0, "positions present for 'product' (pos=1) before crash");

# Build more segments + a merge that really merges. New batches carry DISTINCT phrase
# bigrams so post-recovery phrase recall is discriminating. Four 250-document segments
# share one size layer (bm25_merge_layer_of: floor(log4 ndocs) = 3), which is
# BM25_MERGE_LAYER_FANOUT, so the bm25_merge merges them -- one output with a merged
# POS chain, four retired ranges. (Two 500-document seals never reached the fanout,
# and bm25_merge only sealed.)
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'beta', 'signal breach contract signal' FROM generate_series(3001, 3250) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'beta', 'signal breach contract signal' FROM generate_series(3251, 3500) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'gamma', 'signal duty care signal' FROM generate_series(3501, 3750) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'gamma', 'signal duty care signal' FROM generate_series(3751, 4000) g});
$node->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});

# Leave some rows UNSEALED in the pending list (INSERTed after the last seal/merge): their
# posblob positions must feed the phrase recheck after recovery (read-your-writes). They
# carry a UNIQUE bigram ('foxtrot golf') absent from the base corpus, so a phrase query for
# it isolates exactly these two rows WITHOUT an id predicate -- an `AND id = n` predicate
# would let the planner use docs_pkey and relegate @@@ to a bm25_match Filter (column-local,
# no positions, no scan slot), skipping the recheck. doc 4001 is ADJACENT, 4002 REVERSED.
$node->safe_psql('postgres',
	qq{INSERT INTO docs VALUES (4001, 'delta', 'signal foxtrot golf signal')});
$node->safe_psql('postgres',
	qq{INSERT INTO docs VALUES (4002, 'delta', 'signal golf foxtrot signal')});

# (The INSERTs above write WAL under an xid, so their commits are synchronous and
# also flushed the merge.) Build
# the unsealed rows into a segment the way a seal would and stop before publishing:
# the crashed seal's orphans, POS chain included, holding a second copy of 4001/4002
# that no query may see. Page kind from bm25_format.h: a segment header is
# BM25_PAGE_SEGCAT.
my $PAGE_SEGCAT = 4;
my $orphan = $node->safe_psql('postgres', qq{SELECT bm25_debug_seal_unpublished('docs_bm25')});
like($orphan, qr/^\d+$/, "seal build without publish left an orphan segment (header block $orphan)");
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$node->start;

# 0. Recovery replayed both outcomes. nsegs and the retired-range count change only
#    in the merge's swap record, and 4001/4002 are still pending because nothing
#    published them; the orphan header's page kind exists only if its page-init
#    record was replayed (a lost build leaves a zero page from relation extension).
is($node->safe_psql('postgres',
		qq{SELECT nsegs || '/' || pending_ndocs || '/' || bm25_debug_retired_count('docs_bm25') FROM bm25_stats('docs_bm25')}),
	'2/2/4', 'recovery replayed the merge (2 segments, 4 retired ranges) and left 4001/4002 pending');
is($node->safe_psql('postgres', qq{SELECT bm25_debug_page_flags('docs_bm25', $orphan)}),
	$PAGE_SEGCAT, "recovery replayed the orphan build: block $orphan is an unpublished segment header");

# 1. field_count and the multi-field structure survived the crash.
my $fc_after = $node->safe_psql('postgres',
	qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_after, '2', "field_count unchanged across crash recovery");

# 2. Phrase recall over the sealed segments is unchanged: the 3000 base adjacent-bigram
#    docs still match (positions + postings replayed consistently). Order-free COUNT over
#    the inner forced scan (same portability reasoning as before the crash).
my $phrase_after = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ '"product liability"' ORDER BY title &\@\@ '"product liability"' LIMIT 100000) s});
is($phrase_after, $phrase_before, "phrase 'product liability' recall unchanged across crash");

# 3. read-your-writes across the crash: the UNSEALED adjacent-bigram doc 4001 phrase-
#    matches the unique "foxtrot golf" phrase, and the reversed doc 4002 does NOT (exact ==
#    ordered PRE/0). Positions from the recovered pending list feed the recheck. The unique
#    bigram isolates the two pending rows via a PURE bm25_native index scan (no id predicate ->
#    no docs_pkey plan steal). array_agg ORDER BY id is over the inner subquery's own id
#    column (in-scope; NOT the 008 out-of-scope trap); the inner &@@ scan has no secondary
#    key (PG16-safe).
my $ryw_exact = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM docs WHERE title \@\@\@ '"foxtrot golf"' ORDER BY title &\@\@ '"foxtrot golf"' LIMIT 100000) s});
is($ryw_exact, '{4001}', "unsealed adjacent bigram (4001) matches, reversed (4002) excluded, after recovery");

# 4. Positions survived seal + the crash: 'product' still reports pos=1 for its postings.
my $pos_after = $node->safe_psql('postgres',
	qq{SELECT count(*) FROM bm25_debug_seg_positions('docs_bm25', 'product') WHERE pos = 1});
ok($pos_after > 0, "positions for 'product' (pos=1) survive seal + crash recovery");

# 5. The scored scan slot recovered: bm25_snippet still renders a highlighted body hit.
#    LIMIT 1, no secondary key, no id predicate (PG16-safe slot query; identical bodies).
my $snip_after = $node->safe_psql('postgres',
	qq{SELECT bm25_snippet(body, '<mark>', '</mark>', 300) FROM docs WHERE title \@\@\@ 'product' ORDER BY title &\@\@ 'product' LIMIT 1});
like($snip_after, qr/<mark>product<\/mark>/, "snippet renders a highlighted body hit after crash");

# 6. Field scoping + key survive: a 'title:alpha' scoped query hits ONLY the 3000
#    title-alpha docs (per-block field-RLE replayed), returned by the id key.
my $scoped = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ 'title:alpha' ORDER BY title &\@\@ 'title:alpha' LIMIT 100000) s});
is($scoped, '3000', "field-scoped 'title:alpha' matches exactly the 3000 title-alpha docs after recovery");

# 7. A merge still works after recovery: seal the pending rows and merge to a clean state;
#    phrase recall over the sealed segments is still correct (no loss/dup post-merge). The
#    base "product liability" count stays 3000 (the pending rows carry the distinct
#    "foxtrot golf" bigram, not this one). Order-free count.
$node->safe_psql('postgres', qq{SELECT bm25_seal('docs_bm25')});
$node->safe_psql('postgres', qq{SELECT bm25_merge('docs_bm25')});
my $phrase_merged = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ '"product liability"' ORDER BY title &\@\@ '"product liability"' LIMIT 100000) s});
is($phrase_merged, '3000', "base phrase recall stable after a post-recovery seal + merge");

# The once-unsealed adjacent doc 4001 is now SEALED and still phrase-matches the unique
# "foxtrot golf" (merged POS chain); the reversed 4002 is still excluded. Pure bm25_native scan.
my $ryw_merged = $node->safe_psql('postgres',
	qq{SET enable_seqscan = off; SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM docs WHERE title \@\@\@ '"foxtrot golf"' ORDER BY title &\@\@ '"foxtrot golf"' LIMIT 100000) s});
is($ryw_merged, '{4001}', "sealed-after-merge adjacent bigram (4001) still matches; reversed (4002) still excluded");

$node->stop;
bm25_check_logs($node);
done_testing();
