use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# §5.6 conformance: a physical streaming standby of the exact "§1a" index
# shape (multi-field title/summary/body_plain + per-field boosts + key_field + INCLUDE +
# default positions) must return BYTE-IDENTICAL results to the primary for a
# representative ranked multi-field fan-out query, a phrase query, and a snippet -- the
# Generic-WAL + deterministic-analyzer property extended to the full multi-field shape.

my $primary = PostgreSQL::Test::Cluster->new('bb_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres',
    'CREATE TABLE conf_brief (id int primary key, title text, summary text, body_plain text)');
# Deterministic multi-field corpus. Every row shares stemmable legal terms with a
# per-row discriminator so the scored order is stable and non-trivial. Row (g%5) rotates
# which field carries 'mandamus' so the fan-out ranking exercises the per-field boosts.
$primary->safe_psql('postgres', <<'SQL');
INSERT INTO conf_brief
SELECT g,
       'brief ' || CASE WHEN g % 5 = 0 THEN 'mandamus petition' ELSE 'petition ' || (g % 7) END,
       'summary of proceedings ' || (g % 3),
       'the court reviewed the product liability negligence record '
         || CASE WHEN g % 5 = 1 THEN 'and granted mandamus relief ' ELSE '' END
         || 'in matter number ' || g
FROM generate_series(1, 400) g;
SQL
$primary->safe_psql('postgres', <<'SQL');
CREATE INDEX conf_bm25 ON conf_brief USING bm25_native (title, summary, body_plain)
  INCLUDE (id)
  WITH (key_field = 'id', language = 'english',
        boost_title = 5, boost_summary = 3, boost_body_plain = 1);
SQL

my $backup_name = 'bb_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('bb_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# Helper: run identical SQL on both nodes and assert byte-identical output.
sub both_equal {
    my ($sql, $label) = @_;
    my $p = $primary->safe_psql('postgres', $sql);
    my $s = $standby->safe_psql('postgres', $sql);
    is($s, $p, $label);
    return $p;
}

# 1. Ranked multi-field fan-out: 'mandamus' fans out across all three fields; the
#    per-field boosts weight it. The ranked id list (single-key ORDER BY) must be
#    byte-identical on both nodes.
my $fanout = q{SET enable_seqscan=off;}
  . q{ SELECT id FROM conf_brief}
  . q{ WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')])}
  . q{ ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')]) LIMIT 25};
my $ids = both_equal($fanout, 'ranked multi-field fan-out identical on primary and standby');
ok(length($ids) > 0, 'fan-out returned a non-empty ranked list');

# 2. Phrase query: 'product liability' is adjacent in every body_plain, so the phrase
#    matches the whole corpus; the SINGLE-KEY ranked id list must match byte-for-byte.
#    SINGLE-KEY (no secondary tiebreak) is deliberate: a `, id` secondary key would
#    trigger the ORDER-BY-secondary-key rank collapse and degrade the result to plain
#    id-order, testing nothing about BM25-rank replication. Single-key is genuinely
#    score-driven AND deterministic across nodes.
my $phrase = q{SET enable_seqscan=off;}
  . q{ SELECT id FROM conf_brief}
  . q{ WHERE title @@@ bm25_phrase('body_plain','product liability')}
  . q{ ORDER BY title &@@ bm25_phrase('body_plain','product liability') LIMIT 25};
my $ph = both_equal($phrase, 'ranked phrase query identical on primary and standby');
ok(length($ph) > 0, 'phrase query returned a non-empty ranked list');

# 3. Snippet: the highlighted excerpt of a body hit must be byte-identical (deterministic
#    re-analysis of the stored field). Aggregate to one deterministic row.
my $snippet = q{SET enable_seqscan=off;}
  . q{ SELECT string_agg(snip, '|' ORDER BY id) FROM (}
  . q{   SELECT id, bm25_snippet(body_plain, '<mark>', '</mark>', 60) AS snip FROM conf_brief}
  . q{   WHERE title @@@ bm25_term('body_plain','negligence')}
  . q{   ORDER BY title &@@ bm25_term('body_plain','negligence') LIMIT 5) t};
my $snip = both_equal($snippet, 'snippet highlight identical on primary and standby');
like($snip, qr/<mark>/, 'snippet contains a highlight marker');

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
