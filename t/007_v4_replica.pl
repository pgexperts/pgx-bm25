use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# v4 replica equality: a physical streaming standby must return IDENTICAL bm25_native
# ranking AND an identical analyzer fingerprint to the primary. The M3 analyzer is
# core ts_lexize over Snowball dictionaries — a pure function of (stable dict OID,
# token) — so index-time tokenization on the primary and replay-time pages on the
# standby are byte-identical, with no custom rmgr and no shared_preload (the headline
# Generic-WAL property, extended to the analyzer). We also prove the stemmer is live:
# a query 'negligent' matches a doc indexed 'negligence' on BOTH nodes.

my $primary = PostgreSQL::Test::Cluster->new('v4_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
# Mix of stemmable legal terms + a per-row discriminator. 'negligence' is indexed so a
# stemmed 'negligent' query must match it; 'storage' appears in every row for a stable
# scored ranking the two nodes must agree on.
$primary->safe_psql('postgres',
	qq{INSERT INTO docs SELECT g, 'storage negligence defendant term' || (g % 8) FROM generate_series(1, 400) g});
$primary->safe_psql('postgres',
	qq{CREATE INDEX docs_bm25 ON docs USING bm25_native (body) WITH (analyzer = 'english', stopwords = 'default')});

my $backup_name = 'v4_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('v4_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# 1. Identical analyzer fingerprint on both nodes (the metapage replicated verbatim).
my $fp_primary = $primary->safe_psql('postgres',
	qq{SELECT bm25_debug_analyzer_fingerprint('docs_bm25')});
my $fp_standby = $standby->safe_psql('postgres',
	qq{SELECT bm25_debug_analyzer_fingerprint('docs_bm25')});
is($fp_standby, $fp_primary, "analyzer fingerprint identical on primary and standby");
ok($fp_primary ne '0' && $fp_primary ne '', "fingerprint is non-zero (analyzer active)");

# 2. Identical ranked id list for a stemmed query under the SAME ORDER BY ... LIMIT.
#    'defendant' is a stemmable content term present in every row; the scored order
#    must be byte-identical because tokenization + postings replicate exactly. &@@ is
#    the order-by (KNN distance = -score) operator; tie-break by id for determinism.
my $order_by = qq{SET enable_seqscan=off; SELECT id FROM docs WHERE body \@\@\@ 'defendant' ORDER BY body &\@\@ 'defendant', id LIMIT 25};
my $primary_ids = $primary->safe_psql('postgres', $order_by);
my $standby_ids = $standby->safe_psql('postgres', $order_by);
is($standby_ids, $primary_ids, "ranked id list identical on primary and standby (deterministic tokenization)");

# 3. The stemmer is live on BOTH nodes: a 'negligent' query matches the indexed
#    'negligence' tokens. Equal, non-zero match counts prove replay-time tokens ==
#    index-time tokens.
my $neg_primary = $primary->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'negligent'});
my $neg_standby = $standby->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'negligent'});
ok($neg_primary > 0, "stemmed 'negligent' matches 'negligence' docs on primary");
is($neg_standby, $neg_primary, "stemmed match count identical on standby");

# 4. The scan-time fingerprint gate does not error on the standby (index fp == query
#    fp, both resolved from the same replicated reloptions) — a plain count succeeds.
my $cnt_standby = $standby->safe_psql('postgres',
	qq{SELECT count(*) FROM docs WHERE body \@\@\@ 'storage'});
is($cnt_standby, '400', "all 400 'storage' docs match on standby (gate passes, no mismatch)");

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
