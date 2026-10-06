use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# M5 replica equality: a physical streaming standby must return an IDENTICAL BM25F
# ranked id list to the primary for a multi-field, key_field index. Two things must
# replicate exactly: (a) per-field postings + per-field boosts (the scored order), and
# (b) the docid->key (KEYMAP) mapping (the returned id values). Both are plain Generic
# WAL pages -- no custom rmgr, no shared_preload -- so the primary's build-time pages and
# the standby's replayed pages are byte-identical. We also prove field scoping and the
# 5:1 title boost replicate: a title hit outranks the same term appearing only in body.

my $primary = PostgreSQL::Test::Cluster->new('m5_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres',
	'CREATE TABLE docs(id int primary key, title text, body text)');
# 'running' appears in the TITLE of docs 1..100 and the BODY of docs 101..200. With
# boost_title=5, the title-hit docs must rank ABOVE the body-hit docs for a bare
# 'running' BM25F query -- a discriminating check the standby must reproduce exactly.
# A bare RHS 'running' on the text @@@ operator scores BM25F across ALL fields (the LHS
# column is the opclass anchor, not a scope), so it matches title-hit and body-hit docs
# alike; boost_title=5 orders the title hits first. key_field='id' is an INCLUDE column.
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'running shoes', 'storage term' || (g % 8) FROM generate_series(1, 100) g});
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'hiking boots', 'running storage term' || (g % 8) FROM generate_series(101, 200) g});
$primary->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id) WITH (key_field='id', boost_title='5.0', boost_body='1.0')});

my $backup_name = 'm5_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('m5_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# 1. field_count identical on both nodes (metapage + field-config page replicated).
my $fc_primary = $primary->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
my $fc_standby = $standby->safe_psql('postgres', qq{SELECT field_count FROM bm25_stats('docs_bm25')});
is($fc_standby, $fc_primary, "field_count identical on primary and standby");
is($fc_primary, '2', "field_count is 2 (title, body)");

# 2. Identical BM25F ranked id list (returned by key_field id) for a bare 'running'
#    query. Because title is boosted 5:1, ids 1..100 (title hits) must lead ids 101..200
#    (body hits); the exact scored order must be byte-identical across nodes. The &@@
#    order-by forces the bm25_native index scan on both nodes; tie-break by id for determinism.
my $order_by = qq{SET enable_seqscan=off; SELECT id FROM docs WHERE title \@\@\@ 'running' ORDER BY title &\@\@ 'running', id LIMIT 25};
my $primary_ids = $primary->safe_psql('postgres', $order_by);
my $standby_ids = $standby->safe_psql('postgres', $order_by);
is($standby_ids, $primary_ids, "BM25F ranked id list identical on primary and standby");

# 3. The 5:1 title boost is live and replicated: the top result is a title-hit id (<=100),
#    not a body-hit id, on BOTH nodes.
my $top_primary = $primary->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT id FROM docs WHERE title \@\@\@ 'running' ORDER BY title &\@\@ 'running', id LIMIT 1});
ok($top_primary <= 100, "top BM25F hit is a title-boosted doc on primary");
my $top_standby = $standby->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT id FROM docs WHERE title \@\@\@ 'running' ORDER BY title &\@\@ 'running', id LIMIT 1});
is($top_standby, $top_primary, "top BM25F hit identical on standby (boost replicated)");

# 4. Field scoping replicates: 'title:running' hits ONLY the 100 title-running docs on
#    both nodes (proves per-block field-RLE replayed correctly). The ranked &@@ form
#    forces the bm25_native index, which parses field:term in bm25_rescan.
my $scoped_primary = $primary->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ 'title:running' ORDER BY title &\@\@ 'title:running') s});
my $scoped_standby = $standby->safe_psql('postgres',
	qq{SET enable_seqscan=off; SELECT count(*) FROM (SELECT id FROM docs WHERE title \@\@\@ 'title:running' ORDER BY title &\@\@ 'title:running') s});
is($scoped_primary, '100', "field-scoped 'title:running' hits 100 title docs on primary");
is($scoped_standby, $scoped_primary, "field-scoped count identical on standby");

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
