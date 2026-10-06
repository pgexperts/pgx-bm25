use strict;
use warnings;
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;
use JSON::PP;

my $node = PostgreSQL::Test::Cluster->new('oracle');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# Five-document corpus: few distinct terms so the M0/M1 single-page dict and
# single-page posting limits are respected.  Inserted in id order so that equal
# BM25 scores sort by doc index on both the oracle and the C sides.
my @docs = (
    'database systems and storage engines',
    'the quick brown fox jumps',
    'database database indexing performance',
    'storage and retrieval of documents',
    'quick database lookups with indexes',
);
my @queries = ('database', 'storage indexes', 'quick fox');

$node->safe_psql('postgres',
    'CREATE TABLE docs (id int primary key, body text) WITH (autovacuum_enabled=off)');
for my $i (0 .. $#docs) {
    my $b = $docs[$i];
    $b =~ s/'/''/g;
    $node->safe_psql('postgres',
        "INSERT INTO docs VALUES (${\($i + 1)}, '$b')");
}
$node->safe_psql('postgres',
    'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');

# v4/M3: the index now tokenizes through the analyzer (lowercase -> stopword ->
# Snowball stemmer), so a raw-text oracle would disagree on which terms exist.
# This test validates the BM25 SCORING math, not tokenization (that is 25_analyzer's
# job), so feed the oracle the SAME tokens the index uses: bm25_debug_tokenize(text)
# applies the index-default analyzer (english/default/standard), identical to what
# CREATE INDEX ... USING bm25_native indexed. The oracle's own whitespace tokenizer then
# passes the already-stemmed, stopword-filtered tokens through unchanged.
my @analyzed_docs    = map { analyze($node, $_) } @docs;
my @analyzed_queries = map { analyze($node, $_) } @queries;

# Run the Python oracle on the analyzer-tokenized corpus + queries.
my $payload = encode_json(
    { docs => \@analyzed_docs, queries => \@analyzed_queries, k1 => 1.2, b => 0.75 });
my $oracle_json = run_python($payload);
my $oracle      = decode_json($oracle_json)->{q};

for my $qi (0 .. $#queries) {
    my $q = $queries[$qi];
    (my $qe = $q) =~ s/'/''/g;

    # Retrieve ids in ranked order from the index.  We force the index path
    # with enable_seqscan=off and use the ORDER BY &@@ operator that drives
    # amcanorderbyop so no Sort node appears (tested separately in Task 11).
    my $got = $node->safe_psql('postgres', qq{
        SET enable_seqscan=off;
        SELECT string_agg(id::text, ',' ORDER BY rn)
        FROM (SELECT id, row_number() OVER () rn
              FROM docs
              WHERE body \@\@\@ '$qe'
              ORDER BY body &\@\@ '$qe') s;
    });

    # Oracle result: zero-based doc indices -> one-based ids.
    my @exp_ids = map { $_->[0] + 1 } @{ $oracle->[$qi] };
    is($got, join(',', @exp_ids), "ranking matches oracle for query: $q");
}

$node->stop;
bm25_check_logs($node);
done_testing();

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# analyze — run text through the index-default analyzer (bm25_debug_tokenize) and
# return the tokens as a single space-joined string for the oracle. This is exactly
# the term stream the index stored/queries, so the oracle scores the same tokens.
sub analyze {
    my ($node, $text) = @_;
    (my $esc = $text) =~ s/'/''/g;
    return $node->safe_psql('postgres',
        "SELECT array_to_string(bm25_debug_tokenize('$esc'), ' ')");
}

# run_python — invoke the BM25 oracle script with $stdin, return stdout.
#
# The script is located relative to this file's real directory (t/) so the
# path survives being called from any working directory under prove or CI.
# Using $FindBin::RealBin rather than $ENV{PWD} is intentional: $ENV{PWD} is
# not updated by prove and may point to the original shell cwd, not t/.
sub run_python {
    my ($stdin) = @_;
    my $script = "$FindBin::RealBin/../test/oracle/bm25_oracle.py";
    my $out     = '';
    run_log([ 'python3', $script ], '<', \$stdin, '>', \$out);
    return $out;
}
