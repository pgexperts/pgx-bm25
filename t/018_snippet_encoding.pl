use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# 018_snippet_encoding.pl -- bm25_snippet's truncation marker must be representable
# in the SERVER ENCODING.
#
# bm25_snippet marks a truncated excerpt with U+2026 HORIZONTAL ELLIPSIS. That used to
# be emitted as three hardcoded UTF-8 bytes, unconditionally, which is right in exactly
# one server encoding. Everywhere else it injected bytes that do not mean what they say:
#
#   LATIN1   the value silently carries 0xE2 0x80 0xA6, which IS legal LATIN1 (every
#            byte is), so nothing errors -- it just renders as three mojibake characters
#            forever. This is the case below.
#   EUC_JP   0xE2 is a valid lead byte but 0x80 is not a valid trail, so the value is
#            not legal EUC_JP at all and the query dies with "invalid byte sequence for
#            encoding EUC_JP: 0xe2 0x80" the moment it is converted for the client.
#
# The two encodings below are chosen because they drive the two DIFFERENT branches of
# the fix, and fail differently when it regresses:
#
#   LATIN1  has no ellipsis in its repertoire, so it drives the ASCII FALLBACK branch.
#           Its permissiveness is what makes it the right regression target: the pre-fix
#           output raised no error, so only an assertion about the actual BYTES can
#           distinguish fixed from broken. `rc == 0` would pass either way.
#   EUC_JP  DOES have an ellipsis (JIS X 0208, bytes 0xA1 0xC4), so it drives the
#           CONVERSION branch -- the fix must emit the real character there, not "...".
#           And because 0xE2 0x80 is not a legal EUC_JP sequence at all, a regression
#           here is a hard ERROR rather than silent mojibake.
#
# WHY THE EUC_JP NODE ARRIVED LATE. It could not be written when this suite was: until
# the SQL script was made 7-bit clean, bm25_native could not be installed in an EUC_JP
# database AT ALL. bm25_native--1.0.sql carried 11 comment lines with em dashes and a
# Sigma, and CREATE EXTENSION validates the WHOLE script against the server encoding, so
# it died on `invalid byte sequence for encoding "EUC_JP": 0xe2 0x80` before any of this
# code was reachable. (The review finding about non-ASCII sources, #66, is scoped to the
# C files -- where it genuinely is cosmetic -- and never looked at the SQL script, where
# it is load-bearing.) That is now fixed and gated by test/check_source_ascii.py; this
# node is the behavioural half of that gate, and it fails loudly if the script ever
# picks up a non-ASCII byte again.

# One corpus, one query shape, two encodings. The 12-character budget over a
# 63-character field puts the hit in the middle, so the excerpt truncates on BOTH sides
# and both ellipsis sites fire. Reading the value back as hex keeps the assertion at the
# byte level, where the defect lives -- comparing rendered text would go through a
# client-encoding conversion that hides it.
sub snippet_hex_in_encoding
{
    my ($name, $encoding) = @_;

    my $node = PostgreSQL::Test::Cluster->new($name);
    $node->init(extra => [ "--encoding=$encoding", '--locale=C' ]);
    $node->start;

    # Not safe_psql: this is the assertion the EUC_JP node exists to make, so a failure
    # here must be reported as a failed test rather than as a died-in-setup. Returning
    # early on failure matters for the DIAGNOSTIC, not the verdict -- every safe_psql
    # below would die without the extension, aborting the file with "exited just after
    # N" and burying the one line that says why.
    my ($crc, undef, $cstderr) = $node->psql('postgres', 'CREATE EXTENSION bm25_native');
    if (!is($crc, 0, "CREATE EXTENSION bm25_native succeeds in a $encoding database"))
    {
        diag("CREATE EXTENSION failed: $cstderr");
        $node->stop;
        return '';
    }

    $node->safe_psql('postgres',
        'CREATE TABLE e (id int primary key, body text) WITH (autovacuum_enabled=off)');
    $node->safe_psql('postgres',
        q{INSERT INTO e VALUES (1, 'alpha bravo charlie delta echo foxtrot golf hotel india juliett')});
    $node->safe_psql('postgres',
        q{CREATE INDEX e_bm25 ON e USING bm25_native (body) WITH (language = 'english')});

    my ($rc, $stdout, $stderr) = $node->psql('postgres',
        q{SET enable_seqscan=off; }
      . qq{SELECT encode(convert_to(bm25_snippet(body, '<m>', '</m>', 12), '$encoding'), 'hex') }
      . q{FROM e WHERE body @@@ 'echo' ORDER BY body &@@ 'echo'});

    is($rc, 0, "truncating snippet succeeds in a $encoding database")
        or diag("psql failed: $stderr");

    # The hit itself must still be wrapped -- guards against the marker assertions
    # passing because the snippet came back empty or NULL.
    like($stdout, qr/3c6d3e6563686f3c2f6d3e/,
        "$encoding: the query term is still tag-wrapped (\"<m>echo</m>\")");

    # No raw UTF-8 ellipsis in either encoding: that is the shared defect.
    unlike($stdout, qr/e280a6/,
        "$encoding: no raw UTF-8 ellipsis bytes leak into the value");

    $node->stop;
    return $stdout;
}

# LATIN1 -- the fallback branch. Pre-fix stdout was
#   e280a6203c6d3e6563686f3c2f6d3e20e280a6
# post-fix it is
#   2e2e2e203c6d3e6563686f3c2f6d3e202e2e2e
my $latin1 = snippet_hex_in_encoding('snippet_latin1', 'LATIN1');
like($latin1, qr/^2e2e2e/,
    'LATIN1: excerpt truncated on the left carries the ASCII fallback marker');
like($latin1, qr/2e2e2e$/,
    'LATIN1: excerpt truncated on the right carries the ASCII fallback marker');

# EUC_JP -- the conversion branch. Post-fix stdout is
#   a1c4203c6d3e6563686f3c2f6d3e20a1c4
# The a1c4 assertions are what make this node more than a duplicate of the LATIN1 one:
# an implementation that always emitted the ASCII fallback would pass every LATIN1
# assertion above and fail both of these.
my $eucjp = snippet_hex_in_encoding('snippet_eucjp', 'EUC_JP');
like($eucjp, qr/^a1c4/,
    'EUC_JP: excerpt truncated on the left carries the real JIS X 0208 ellipsis');
like($eucjp, qr/a1c4$/,
    'EUC_JP: excerpt truncated on the right carries the real JIS X 0208 ellipsis');

# SQL_ASCII holding UTF-8 text (#308 TEXT-07) -- a PIN, not a fix. The excerpt edge used to
# land inside a UTF-8 character: in SQL_ASCII every high byte was a separator to the word
# test the edge snapper uses, and the snapper's continuation-byte nudge is UTF-8-only, so
# an edge stopped wherever the budget put it. The value then carried a split character
# and a client with client_encoding=UTF8 failed the whole statement ("invalid byte
# sequence for encoding UTF8: 0x9e"). PR #327 (#295, ADR 0114) made every high byte a
# word byte in SQL_ASCII, so the snapper now walks a partial run out to an ASCII
# separator; this node holds that in place.
#
# The row is the issue's: '<ja> judge <ja> <ja> <ja>' with Japanese runs written as
# escaped UTF-8 bytes to keep this file ASCII. In SQL_ASCII a byte is a character, so the
# budget of 9 leaves 2 bytes either side of 'judge': both land inside a three-byte run.
{
    my $node = PostgreSQL::Test::Cluster->new('snippet_sql_ascii');
    $node->init(extra => [ '--encoding=SQL_ASCII', '--locale=C' ]);
    $node->start;
    $node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
    $node->safe_psql('postgres',
        'CREATE TABLE e (id int primary key, body text) WITH (autovacuum_enabled=off)');
    my $nihongo  = '\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e';             # three characters
    my $tekisuto = '\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88'; # four characters
    $node->safe_psql('postgres',
        "INSERT INTO e VALUES (1, E'$nihongo judge $tekisuto $nihongo $tekisuto')");
    $node->safe_psql('postgres',
        q{CREATE INDEX e_bm25 ON e USING bm25_native (body) WITH (language = 'english')});

    my $snippet = q{bm25_snippet(body, '<m>', '</m>', 9)};
    my $from    = q{FROM e WHERE body @@@ 'judge' ORDER BY body &@@ 'judge'};

    # The client-visible failure: the server validates every value it sends a UTF8 client.
    my ($rc, $stdout, $stderr) = $node->psql('postgres',
        "SET client_encoding = UTF8; SET enable_seqscan = off; SELECT $snippet $from");
    is($rc, 0, 'SQL_ASCII: a truncating snippet over UTF-8 text reaches a UTF8 client')
        or diag("psql failed: $stderr");

    # The bytes themselves, read back without any conversion: they must be valid UTF-8
    # and still carry the wrapped hit.
    my $hex = $node->safe_psql('postgres',
        "SET enable_seqscan = off; SELECT encode(convert_to($snippet, 'SQL_ASCII'), 'hex') $from");
    like($hex, qr/3c6d3e6a756467653c2f6d3e/,
        'SQL_ASCII: the query term is still tag-wrapped ("<m>judge</m>")');
    my $bytes = pack('H*', $hex);
    ok(utf8::decode($bytes), 'SQL_ASCII: every excerpt edge is a UTF-8 character boundary')
        or diag("excerpt bytes: $hex");

    $node->stop;
}

done_testing();
