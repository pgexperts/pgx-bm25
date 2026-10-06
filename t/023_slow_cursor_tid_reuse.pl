use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# A slow cursor outlives a tombstone and the reuse of the tombstoned line pointer
# (#269, guarding ADR 0100's safety argument).
#
# ADR 0100 lets a reader sample a segment's LIVEDOCS bitmap once, when it opens the
# segment, and then treat the segment as all-live (bm25_seg_reader_init_checked; the
# WAND driver's up-front check in bm25_wand.c). A document VACUUM tombstones after
# that sample is ranked and handed out as live. The argument that this is sound: the
# document's heap line pointer can be reused only after the tombstone, so after the
# sample and therefore after the scan's MVCC snapshot, and the executor's heap
# visibility check (the AM sets xs_recheck = false, so it is the only filter) drops
# whatever new tuple sits there. Until this test that argument was a comment and an
# ADR paragraph.
#
# The test runs the exact interleaving: five cursors (exhaustive, phrase, boolean,
# unranked filter, and WAND) each fetch one row, so each has sampled liveness while
# the deleted document D is still marked live. VACUUM then tombstones D and frees its
# line pointer, an INSERT takes that line pointer, and the cursors fetch the rest.
# None may return the new row, and each must return exactly the live matches.
#
# The other half, a FRESH scan after the tombstone, where the sampled bitmap is what
# keeps the dead TID out, is covered elsewhere and not repeated here: sql/117 for the
# exhaustive, phrase, boolean and filter paths (with a reused line pointer), and
# sql/111 for a WAND build (the dead TID is not ranked). A mutation
# that makes the up-front check report "all live" without reading the bitmap leaves
# this file green (the cursors sampled before the tombstone, so the bit was set
# anyway) and fails sql/117 -- which is the argument: for these cursors the bit is
# not what protects them.

my $node = PostgreSQL::Test::Cluster->new('slow_cursor_tid_reuse');
$node->init;
# No autovacuum: a worker could vacuum the table before the cursors sample liveness,
# tombstoning D too early to reach the case under test.
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# The padding puts about 14 rows on a heap page, so the 20 rows span two pages.
# D (id 17) holds x3 and the longest x3 body, so it ranks last on every ranked path
# and is the last x3 posting on the unranked one: after FETCH 1 every cursor still
# has D's TID ahead of it. The other x3 rows (ids 3 and 10) are on the first page.
$node->safe_psql('postgres', q{
	CREATE TABLE ru (id int PRIMARY KEY, body text, pad char(500) DEFAULT 'p')
	  WITH (autovacuum_enabled = off);
	CREATE INDEX ru_bm ON ru USING bm25_native (body);
	INSERT INTO ru SELECT g,
	  CASE WHEN g = 17 THEN 'common x3 y2 pad pad pad pad pad pad pad pad'
	       ELSE 'common x' || (g % 7) || ' y' || (g % 3) END
	  FROM generate_series(1, 20) g;
	SELECT bm25_seal('ru_bm');
});

# A cursor parked after FETCH 1 keeps a pin on the heap page of the row it returned.
# VACUUM prunes a heap page only under a cleanup lock, and a non-aggressive pass does
# not wait for one, so if a pin sat on D's page, D would never be removed or
# tombstoned and the test would silently run a different case. Assert the placement
# rather than assume it.
my $dead = $node->safe_psql('postgres', 'SELECT ctid FROM ru WHERE id = 17');
is( $node->safe_psql(
		'postgres', q{
	SELECT bool_or((ctid::text::point)[0] = (SELECT (ctid::text::point)[0] FROM ru WHERE id = 17))
	  FROM ru WHERE id IN (3, 10)}),
	'f',
	'no live x3 row (where the cursors park) shares the dead row\'s heap page');
$node->safe_psql('postgres', 'DELETE FROM ru WHERE id = 17');

# Each path, with the wand_top_k it runs under. With wand_top_k > 0, a ranked,
# non-phrase query that is not a multi-leaf boolean tree takes WAND; the setting is
# read once, when the ranking is built at the first fetch, and with three matches
# under k = 10 the WAND scan never reads past k, so it never falls back to the
# exhaustive rebuild. That the wand cursor runs WAND was confirmed by instrumentation
# when this test was written; it is not asserted, as no per-scan path counter exists.
my $x3_bool =
  q{bm25_boolean(must => ARRAY[bm25_term('body','common'), bm25_term('body','x3')])};
my %q = (
	exhaustive => [ 0, q{SELECT id FROM ru WHERE body @@@ 'x3' ORDER BY body &@@ 'x3'} ],
	phrase => [
		0,
		q{SELECT id FROM ru WHERE body @@@ '"common x3"' ORDER BY body &@@ '"common x3"'}
	],
	boolean => [ 0, "SELECT id FROM ru WHERE body @@@ $x3_bool ORDER BY body &@@ $x3_bool" ],
	filter => [ 0,  q{SELECT id FROM ru WHERE body @@@ 'x3'} ],
	wand   => [ 10, q{SELECT id FROM ru WHERE body @@@ 'x3' ORDER BY body &@@ 'x3'} ],);
my @names = sort keys %q;

# REPEATABLE READ pins one snapshot, taken after the DELETE committed, for every
# cursor: D is dead to it, and the row inserted below is invisible to it.
my $bg = $node->background_psql('postgres');
# Per-query timeout, not one budget for the whole file: this session lives across
# many queries, and the cassert+UBSan leg runs slowly.
$bg->set_query_timer_restart();
$bg->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ});
$bg->query_safe(q{SET enable_seqscan = off; SET enable_bitmapscan = off});
my %first;
for my $n (@names)
{
	$bg->query_safe("SET bm25_native.wand_top_k = $q{$n}[0]");
	$bg->query_safe("DECLARE c_$n NO SCROLL CURSOR FOR $q{$n}[1]");
	$first{$n} = $bg->query_safe("FETCH 1 FROM c_$n");
	like($first{$n}, qr/^(3|10)$/, "$n: first row is a live x3 row");
}

# Every cursor has sampled liveness by now, and D's LIVEDOCS bit is still set: the
# bitmap itself, read without the heap, has no clear bit. (That each cursor then holds
# D's TID is shown by the per-cursor count of TIDs handed out, below.)
my $dead_bits = q{SELECT count(*) FILTER (WHERE NOT live) FROM bm25_debug_tombstone('ru_bm')};
is($node->safe_psql('postgres', $dead_bits), '0', 'D\'s bit is set when the cursors sample');

# Tombstone D and reuse its line pointer while the cursors are mid-flight.
# INDEX_CLEANUP ON: VACUUM's bypass (which skips index vacuuming when few pages hold
# dead items on a larger table) would leave D's line pointer LP_DEAD, where no insert
# can reuse it. It cannot fire on this two-page table, but ON keeps the test
# independent of table size.
$node->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON) ru');
is($node->safe_psql('postgres', $dead_bits), '1', 'VACUUM cleared D\'s bit');
# The new row matches the cursors' queries, so only heap visibility can keep it out
# (were an emit-time match recheck ever added, it could not mask a visibility bug).
$node->safe_psql('postgres',
	q{INSERT INTO ru (id, body) VALUES (1000, 'common x3 y0')});
is($node->safe_psql('postgres', 'SELECT ctid FROM ru WHERE id = 1000'),
	$dead, 'the new row took D\'s line pointer');

for my $n (@names)
{
	my $before = $bg->query_safe(
		q{SELECT pg_stat_get_xact_tuples_returned('ru_bm'::regclass)});
	my $rest = $bg->query_safe("FETCH ALL FROM c_$n");
	my $after = $bg->query_safe(
		q{SELECT pg_stat_get_xact_tuples_returned('ru_bm'::regclass)});
	my @ids = grep { length } ($first{$n}, split /\n/, $rest);
	note("$n: ids=[" . join(',', @ids) . "] tids_after_first=" . ($after - $before));

	ok(!(grep { $_ eq '1000' } @ids), "$n: the row on the reused line pointer is not returned");
	is(join(',', sort { $a <=> $b } @ids), '3,10', "$n: exactly the live x3 rows");

	# REGIME witness, not a correctness property: the index handed out both remaining
	# TIDs, the other live row and D's (now naming the new row), so the case above
	# really ran with the heap check as the only filter. If the AM ever rechecks
	# liveness when it emits a TID, this count drops to 1 legitimately; rewrite the
	# witness then rather than read it as a bug.
	is($after - $before, 2, "$n: the index handed out D's TID after the reuse");
}
$bg->query_safe('COMMIT');
$bg->quit;

$node->stop;
bm25_check_logs($node);
done_testing();
