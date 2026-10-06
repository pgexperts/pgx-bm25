use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# The orphan sweep's mark_chain must refuse a chain link onto a never-initialized
# page (issue #302 F).
#
# mark_chain read the opaque of every page it reached. On an all-zero page the
# opaque is the page header: a cassert build fails PageGetSpecialPointer's assertion
# (the backend aborts and the server restarts), and a production build reads nextblk
# 0 and stops only because block 0 happens to be marked already. No live chain can
# link a zero page, so the sweep now raises ERRCODE_INDEX_CORRUPTED instead.
#
# A zero page cannot be forged with bm25_debug_poke_page (its record stamps an LSN),
# so this suite makes a real one the way a crash does: an INSERT parked right after
# bm25_page_alloc extended the relation for it (pause point pending_append_alloc,
# the t/028 recipe), then an immediate stop. The extension survives recovery as an
# all-zero block. The poke lever then links the last page of a live segment's NORMS
# chain to it -- a chain only the sweep and scoring read, so VACUUM's bulk delete of
# the aborted INSERT's dead tuple does not reach it first -- and VACUUM runs the sweep,
# which the restart's new crash epoch makes due.

use constant {
	PAGE_NORMS => 1 << 5,
};

my $node = PostgreSQL::Test::Cluster->new('mark_chain_uninitialized');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION bm25_native;
CREATE TABLE d(id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX d_bm25 ON d USING bm25_native (body);
INSERT INTO d SELECT g, 'sealed document number ' || g FROM generate_series(1, 50) g;
SELECT bm25_seal('d_bm25');
INSERT INTO d VALUES (100, 'anchor document already on the chain');
CHECKPOINT;
});

my $pause_key = 1651323445;
my $point = 10;    # pending_append_alloc

my $holder = $node->background_psql('postgres');
$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point)");

my $ins = $node->background_psql('postgres', on_error_stop => 0);
$ins->query_safe("SET application_name = 'parked_insert'");
$ins->query_safe("SET bm25_native.debug_pause = 'pending_append_alloc'");
# Far more distinct terms than the tail page has room for: the append needs a fresh
# page, and parks right after bm25_page_alloc extended the relation for it.
$ins->query_until(qr/started/, q{\echo started
INSERT INTO d SELECT 200, string_agg('term' || k, ' ') FROM generate_series(1, 600) k;
});

$node->poll_query_until('postgres', q{
SELECT EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
                WHERE a.application_name = 'parked_insert'
                  AND l.locktype = 'advisory' AND NOT l.granted)})
  or die 'the INSERT never reached pending_append_alloc';

# The parked backend holds content locks, so it cannot be cancelled; only the
# immediate stop ends it.
$node->stop('immediate');
eval { $ins->quit };
eval { $holder->quit };
$node->start;

# The never-initialized page: flags 0 (bm25_debug_page_flags reports a zero page as 0;
# every initialized bm25 page carries a kind bit).
my @zero = grep { length } split /,/, $node->safe_psql('postgres', q{
SELECT string_agg(b::text, ',' ORDER BY b)
  FROM generate_series(1, bm25_debug_npages('d_bm25')::int - 1) b
 WHERE bm25_debug_page_flags('d_bm25', b) = 0});
is(scalar @zero, 1, 'the crash left exactly one never-initialized page')
  or BAIL_OUT("zero pages: @zero");
my $zero = $zero[0];

my $norms = $node->safe_psql('postgres', qq{
SELECT max(b) FROM generate_series(1, bm25_debug_npages('d_bm25')::int - 1) b
 WHERE bm25_debug_page_flags('d_bm25', b) = @{[PAGE_NORMS]}});
ok($norms ne '', 'the sealed segment has a NORMS page');

my $nextblk_off = $node->safe_psql('postgres', q{
SELECT (SELECT off FROM bm25_debug_layout() WHERE struct = 'page' AND field = 'special')
     + (SELECT off FROM bm25_debug_layout() WHERE struct = 'BM25PageOpaque' AND field = 'nextblk')});

# Host byte order, which is what the opaque stores.
my $native = sub { '\x' . unpack('H*', pack('L', shift)) };

my $old = $node->safe_psql('postgres', qq{
SELECT bm25_debug_poke_page('d_bm25', $norms, $nextblk_off, '@{[$native->($zero)]}')});
is($old, '\xffffffff', 'the NORMS page was the end of its chain');

my ($ret, $out, $err) = $node->psql('postgres', "\\set VERBOSITY verbose\nVACUUM d");
isnt($ret, 0, 'VACUUM fails on a chain link to a never-initialized page');
like($err, qr/XX002: bm25: chain from block \d+ links uninitialized block $zero\b/,
	'the orphan sweep reports the uninitialized page as index corruption')
  or diag("stderr: $err");
unlike($err, qr/server closed the connection/,
	'the backend raised an ERROR rather than failing an assertion');

# Repaired, the next VACUUM sweeps, and the index still answers.
$node->safe_psql('postgres', qq{
SELECT bm25_debug_poke_page('d_bm25', $norms, $nextblk_off, '\\xffffffff')});
($ret, $out, $err) = $node->psql('postgres', 'VACUUM d');
is($ret, 0, 'VACUUM succeeds once the link is repaired') or diag("stderr: $err");
is($node->safe_psql('postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM d WHERE body @@@ 'sealed'}), '50',
	'every sealed document is still found through the index');

$node->stop;
bm25_check_logs($node);
done_testing();
