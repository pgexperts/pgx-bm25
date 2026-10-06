use strict;
use warnings;
use Encode qw(encode);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# 026_single_byte_encodings.pl -- in a single-byte server encoding, a high byte that
# the encoding maps to a letter or digit is a WORD byte (#295, analyzer revision 6).
#
# Analyzer revision 3 (ADR 0077) stopped consulting LC_CTYPE for run splitting and, as
# the price, made every byte 0x80-0xFF a separator whenever the database encoding is
# single-byte. For a script that lives entirely in the high half -- Cyrillic in WIN1251
# or KOI8-R -- that is zero recall: CREATE INDEX succeeds and stores nothing. For
# LATIN1 it shreds accented words into fragments that cross-match ('Aerger' and
# 'Buerger' both contain 'rger'). The fix classifies the high half through a fixed,
# generated per-encoding table (src/bm25_sb_wordclass.c), so the answer is still a pure
# function of the database encoding.
#
# Why TAP rather than pg_regress: the regression suite runs in one database whose
# encoding is the cluster's, and these cases need several encodings side by side.
# CREATE DATABASE ... ENCODING ... LC_CTYPE 'C' TEMPLATE template0 is accepted for any
# encoding, so no OS locale is needed and the test runs on every CI host. LC_CTYPE 'C'
# is also the honest setting for the claim: the classification must not need a locale.
#
# Index-path proof: every membership query runs with seqscan and bitmapscan disabled
# and is the index's own scan; a seqscan-evaluated @@@ re-analyzes the heap text with
# the same analyzer, so it would show the same answer -- the point here is what the
# INDEX stored, and pre-fix it stored no Cyrillic terms at all.
#
# All SQL is sent as UTF-8 under client_encoding UTF8 and the server converts it into
# each database's encoding (SQL_ASCII stores the UTF-8 bytes as they are). The file
# itself stays 7-bit: non-ASCII text is written with \x{...} escapes.

my $node = PostgreSQL::Test::Cluster->new('sbenc');
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->start;

sub u8 { return encode('UTF-8', $_[0]); }

# Run SQL as UTF-8 in database $db; returns the UTF-8 output bytes.
sub q8
{
    my ($db, $sql) = @_;
    return $node->safe_psql($db,
        "SET client_encoding = 'UTF8'; SET enable_seqscan = off; "
      . "SET enable_bitmapscan = off; " . u8($sql));
}

sub mkdb
{
    my ($db, $enc) = @_;
    $node->safe_psql('postgres',
        "CREATE DATABASE $db WITH ENCODING '$enc' LC_COLLATE 'C' LC_CTYPE 'C' "
      . "TEMPLATE template0");
    $node->safe_psql($db, 'CREATE EXTENSION bm25_native');
}

# Cyrillic, Russian: "Moscow capital of Russia", "Paris capital of France".
my $moskva  = "\x{41C}\x{43E}\x{441}\x{43A}\x{432}\x{430}";
my $stolica = "\x{441}\x{442}\x{43E}\x{43B}\x{438}\x{446}\x{430}";
my $rossii  = "\x{420}\x{43E}\x{441}\x{441}\x{438}\x{438}";
my $parizh  = "\x{41F}\x{430}\x{440}\x{438}\x{436}";
my $francii = "\x{424}\x{440}\x{430}\x{43D}\x{446}\x{438}\x{438}";
my $doc1 = "$moskva $stolica $rossii";
my $doc2 = "$parizh $stolica $francii";

# ---------------------------------------------------------------- Cyrillic
# WIN1251 and KOI8-R put the same letters at DIFFERENT byte values (KOI8-R's are not
# even in alphabetical order), so passing both shows the classification really is per
# encoding rather than a range test that happens to fit one of them.
foreach my $enc ('WIN1251', 'KOI8R')
{
    my $db = 'db_' . lc($enc);
    mkdb($db, $enc);

    # Pre-fix: {} -- every Cyrillic byte was a separator.
    my $toks = q8($db,
        "SELECT bm25_debug_tokenize('$doc1', 'standard', 'default', 'russian')");
    isnt($toks, '{}', "$enc: Cyrillic text tokenizes to terms");
    # Under LC_CTYPE/collation C the case fold is ASCII-only, as for to_tsvector in the
    # same database (core parity), so the capitalised words keep their capitals.
    is($toks,
        u8("{\x{41C}\x{43E}\x{441}\x{43A}\x{432},\x{441}\x{442}\x{43E}\x{43B}\x{438}\x{446},"
          . "\x{420}\x{43E}\x{441}\x{441}}"),
        "$enc: Cyrillic words are runs, stemmed by the Russian Snowball stemmer");

    q8($db,
        "CREATE TABLE c (id int PRIMARY KEY, body text); "
      . "INSERT INTO c VALUES (1, '$doc1'), (2, '$doc2'), (3, 'plain ascii row'); "
      . "CREATE INDEX c_bm ON c USING bm25_native (body) WITH (language = 'russian')");

    # Pre-fix: 0 rows (the index held no Cyrillic term).
    is(q8($db, "SELECT array_agg(id ORDER BY id) FROM c WHERE body @@@ '$moskva'"),
        '{1}', "$enc: the index finds a row by a Cyrillic word");
    is(q8($db, "SELECT array_agg(id ORDER BY id) FROM c WHERE body @@@ '$stolica'"),
        '{1,2}', "$enc: a shared Cyrillic word matches both rows");
    is(q8($db,
            "SELECT array_agg(id ORDER BY id) FROM "
          . "(SELECT id FROM c WHERE body @@@ '$stolica' "
          . "ORDER BY body &@@ '$stolica' LIMIT 10) s"),
        '{1,2}', "$enc: the ranked path agrees");
}

# The snippet's edge walkers use the same predicate as the tokenizer (bm25_snippet.c).
# Pre-fix there is no hit to snip at all.
{
    my $long = "$parizh $francii $moskva $stolica $rossii $parizh";
    q8('db_win1251',
        "CREATE TABLE sn (id int PRIMARY KEY, body text); "
      . "INSERT INTO sn VALUES (1, '$long'); "
      . "CREATE INDEX sn_bm ON sn USING bm25_native (body) WITH (language = 'russian')");
    # Budget 20 leaves 7 characters a side around the 6-letter hit, which puts both
    # raw edges INSIDE the neighbouring words; the walkers must drop those partial
    # words rather than show a fragment. A predicate that called Cyrillic bytes
    # separators would stop at once and leave the fragments in. Budget 24 lands both
    # edges on separators, so the neighbours appear whole.
    my $snip = sub {
        return q8('db_win1251',
            "SELECT bm25_snippet(body, '[', ']', $_[0]) FROM sn "
          . "WHERE body @@@ '$moskva' ORDER BY body &@@ '$moskva'");
    };
    is($snip->(20), u8("\x{2026} [$moskva] \x{2026}"),
        'WIN1251: bm25_snippet drops partial Cyrillic words at both edges');
    is($snip->(24), u8("\x{2026} $francii [$moskva] $stolica \x{2026}"),
        'WIN1251: bm25_snippet keeps whole Cyrillic neighbours and highlights the whole hit');
}

# A byte the encoding leaves UNMAPPED (WIN1251 0x98) is a separator, through a
# no-error path: the table, not a conversion, decides.
is( $node->safe_psql('db_win1251',
        q{SELECT bm25_debug_tokenize(convert_from('\x78797a98717273'::bytea, 'WIN1251'), }
      . q{'standard', 'none', 'english')}),
    '{xyz,qrs}', 'WIN1251: unmapped byte 0x98 separates without an error');

# Non-letter high bytes stay separators: the no-break space (0xA0) and the numero sign
# (0xB9, U+2116, a symbol) in WIN1251.
is( $node->safe_psql('db_win1251',
        q{SELECT bm25_debug_tokenize(convert_from('\x78797aa0717273b9747576'::bytea, 'WIN1251'), }
      . q{'standard', 'none', 'english')}),
    '{xyz,qrs,tuv}', 'WIN1251: no-break space and numero sign are separators');

# ---------------------------------------------------------------- LATIN1
mkdb('db_latin1', 'LATIN1');
my $aerger  = "\x{C4}rger";          # A-umlaut rger
my $buerger = "B\x{FC}rger";         # B u-umlaut rger
{
    # Pre-fix: {rger} -- the A-umlaut was a separator.
    is(q8('db_latin1', "SELECT bm25_debug_tokenize('$aerger', 'standard', 'none', 'english')"),
        u8("{$aerger}"), 'LATIN1: an accented word is one term');

    q8('db_latin1',
        "CREATE TABLE l (id int PRIMARY KEY, body text); "
      . "INSERT INTO l VALUES (1, '$aerger im B\x{FC}ro'), (2, 'kein Problem'), "
      . "(3, 'Der $buerger zahlt'); "
      . "CREATE INDEX l_bm ON l USING bm25_native (body) WITH (language = 'german')");

    # Pre-fix: {1,3} -- both reduced to the fragment 'rger'.
    is(q8('db_latin1', "SELECT array_agg(id ORDER BY id) FROM l WHERE body @@@ '$aerger'"),
        '{1}', "LATIN1: 'Aerger' no longer cross-matches 'Buerger'");
    is(q8('db_latin1', "SELECT array_agg(id ORDER BY id) FROM l WHERE body @@@ '$buerger'"),
        '{3}', "LATIN1: 'Buerger' finds only its own row");

    # Letters, not every high byte: multiplication sign and one-half are separators,
    # the feminine ordinal is a letter (Unicode Alphabetic).
    is(q8('db_latin1',
            "SELECT bm25_debug_tokenize('xyz\x{D7}qrs\x{BD}tuv \x{AA}', 'standard', 'none', 'english')"),
        u8("{xyz,qrs,tuv,\x{AA}}"), 'LATIN1: symbols separate, letters join');
}

# ---------------------------------------------------------------- SQL_ASCII
# No declared character set; what sits above 0x7F is in practice unchecked UTF-8, and
# the analyzer joins those bytes into runs as it would in a UTF-8 database.
mkdb('db_sql_ascii', 'SQL_ASCII');
{
    q8('db_sql_ascii',
        "CREATE TABLE s (id int PRIMARY KEY, body text); "
      . "INSERT INTO s VALUES (1, '$doc1'), (2, '$doc2'); "
      . "CREATE INDEX s_bm ON s USING bm25_native (body) WITH (language = 'english')");
    is(q8('db_sql_ascii', "SELECT array_agg(id ORDER BY id) FROM s WHERE body @@@ '$moskva'"),
        '{1}', 'SQL_ASCII: high bytes are word bytes');
}

# ---------------------------------------------------------------- UTF-8 control
# Revision 6 changes nothing for a multibyte database: the same mixed-script sample
# tokenizes exactly as it did under revision 5.
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
is(q8('postgres',
        "SELECT bm25_debug_tokenize('$doc1 $aerger $buerger caf\x{E9} \x{3A3}\x{3BF}\x{3C6}\x{3AF}\x{3B1}', "
      . "'standard', 'none', 'english')"),
    u8("{$moskva,$stolica,$rossii,$aerger,b\x{FC}rger,caf\x{E9},\x{3A3}\x{3BF}\x{3C6}\x{3AF}\x{3B1}}"),
    'UTF8: tokenization of a multilingual sample is unchanged');

$node->stop;
bm25_check_logs($node);
done_testing();
