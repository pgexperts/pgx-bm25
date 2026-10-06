use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# M4 replica equality: a physical streaming standby must return BYTE-IDENTICAL phrase,
# proximity, snippet, and keyed-score results to the primary for a multi-field,
# key_field, POSITION-BEARING index. The POS chain, per-field postings, and the
# docid->key (KEYMAP) mapping are all plain Generic WAL pages -- no custom rmgr, no
# shared_preload -- so the primary's build-time pages and the standby's replayed pages
# are byte-identical, and the position-dependent recheck / snippet must reproduce
# exactly on the standby. Mirrors t/009_m5_replica.pl for the M4 positions surface.

my $primary = PostgreSQL::Test::Cluster->new('m4_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres',
	'CREATE TABLE docs(id int primary key, title text, body text)');
# BODY carries 'product liability' at KNOWN positions: an ADJACENT bigram in docs 1..100
# (exact-phrase docs) and a REVERSED pair in docs 101..200 (proximity-but-not-ordered
# docs). 'signal' pads so the recheck must FILTER. TITLE carries a boosted term.
# key_field='id' is an INCLUDE column; positions are ON by default.
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'running shoes', 'signal product liability signal' FROM generate_series(1, 100) g});
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'hiking boots', 'signal liability product signal' FROM generate_series(101, 200) g});
$primary->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id) WITH (key_field='id', boost_title='5.0', boost_body='1.0')});

my $backup_name = 'm4_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('m4_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# 1. field_count identical on both nodes (metapage + field-config page replicated).
my $fc_primary = $primary->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
my $fc_standby = $standby->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_standby, $fc_primary, "field_count identical on primary and standby");
is($fc_primary, '2', "field_count is 2 (title, body)");

# 2. EXACT phrase "product liability": the ADJACENT-bigram docs 1..100 match, the
#    REVERSED docs 101..200 do NOT. The ranked &@@ form forces the bm25_native index scan on
#    both nodes (only it parses the phrase + runs the positional recheck). tie-break by
#    id for determinism. The standby's replayed POS chain must reproduce the set exactly.
my $exact = qq{SET enable_seqscan=off; SELECT id FROM docs WHERE title \@\@\@ '"product liability"' ORDER BY title &\@\@ '"product liability"', id LIMIT 250};
my $exact_primary = $primary->safe_psql('postgres', $exact);
my $exact_standby = $standby->safe_psql('postgres', $exact);
is($exact_standby, $exact_primary, "exact-phrase ranked id list identical on primary and standby");

# 3. The exact-phrase set is exactly the 100 adjacent-bigram docs (<=100) on BOTH nodes;
#    no reversed doc (>100) leaks in -- the position-dependent recheck replicated.
my $exact_cnt = qq{SET enable_seqscan=off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ '"product liability"' ORDER BY title &\@\@ '"product liability"' LIMIT 100000) s};
my $exact_cnt_primary = $primary->safe_psql('postgres', $exact_cnt);
my $exact_cnt_standby = $standby->safe_psql('postgres', $exact_cnt);
is($exact_cnt_primary, '100', "exact phrase matches exactly the 100 adjacent-bigram docs on primary");
is($exact_cnt_standby, $exact_cnt_primary, "exact-phrase count identical on standby");

# 4. Proximity: "product liability"~>0 (ordered PRE/0 == EXACT) excludes the reversed
#    docs, "product liability"~1 (unordered W/1) INCLUDES the reversed docs (span 1 <=
#    (2-1)+1). The reversed-docs delta between the two must be identical on both nodes.
my $unord = qq{SET enable_seqscan=off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ '"product liability"~1' ORDER BY title &\@\@ '"product liability"~1' LIMIT 100000) s};
my $unord_primary = $primary->safe_psql('postgres', $unord);
my $unord_standby = $standby->safe_psql('postgres', $unord);
is($unord_primary, '200', "unordered ~1 matches all 200 docs (adjacent + reversed) on primary");
is($unord_standby, $unord_primary, "unordered ~1 count identical on standby");

# 5. bm25_snippet renders a byte-identical highlighted body hit on both nodes. The snippet
#    reads the ACTIVE scored-scan slot, so the query must run inside the bm25_native ordered index
#    scan: @@@ key + &@@ order-by on the SAME literal, anchored on the leading column, with
#    NO secondary sort key and NO id predicate (a `, id` tiebreak risks a PG16 bitmap scan,
#    an `id=n` predicate steals the plan to docs_pkey -> either yields a NULL snippet).
#    LIMIT 1 returns one row; the 'product'-in-body docs share a body, so the snippet string
#    is byte-identical regardless of which tied row the scan surfaces first.
my $snip = qq{SET enable_seqscan=off; SELECT bm25_snippet(body, '<mark>', '</mark>', 300) FROM docs WHERE title \@\@\@ 'product' ORDER BY title &\@\@ 'product' LIMIT 1};
my $snip_primary = $primary->safe_psql('postgres', $snip);
my $snip_standby = $standby->safe_psql('postgres', $snip);
like($snip_primary, qr/<mark>product<\/mark>/, "snippet renders a highlighted body hit on primary");
is($snip_standby, $snip_primary, "snippet byte-identical on standby");

# 6. Keyed score: the keyed BM25F score multiset (returned by key_field id) is byte-
#    identical across nodes -- postings + boosts + KEYMAP all replicated. bm25_score_key(id)
#    reads the same active slot, so the inner scan is the forced &@@ index scan with NO
#    secondary key (PG16-safe); the OUTER query re-orders the collected (id, score) rows by
#    the ROUNDED SCORE then id (ordinary column sorts, not &@@) for a deterministic,
#    tie-tolerant comparison independent of the scan's internal row order.
my $keyed = qq{SET enable_seqscan=off; SELECT id, sc FROM (SELECT id, round(bm25_score_key(id)::numeric, 6) AS sc FROM docs WHERE title \@\@\@ 'product' ORDER BY title &\@\@ 'product' LIMIT 100000) r ORDER BY sc DESC, id};
my $keyed_primary = $primary->safe_psql('postgres', $keyed);
my $keyed_standby = $standby->safe_psql('postgres', $keyed);
is($keyed_standby, $keyed_primary, "keyed BM25F score list (by id key) identical on primary and standby");

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
