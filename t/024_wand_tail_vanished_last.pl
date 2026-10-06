use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# The WAND over-pull tail when the last entry the scan emitted has vanished (#268).
#
# A capped scan (bm25_native.wand_top_k = k) that the executor pulls past k rows
# rebuilds the full ranking and resumes after the last entry it emitted. That entry
# need not be a row the executor returned: the capped ranking can hold a dead entry
# (a row deleted before the cursor's snapshot, not yet vacuumed), which the AM emits
# and the executor discards within the same pull before pulling again into the tail.
# Nothing holds VACUUM off such a row, so another session can remove it -- and
# tombstone its index entry -- between the cursor's FETCHes, and the rebuild then no
# longer ranks it.
#
# The tail used to look for that entry's TID in the rebuilt ranking and, not finding
# it, fall back to the ordinal, which is one slot off once the entry is gone: the row
# ranked just after it was skipped. It now resumes at the first entry strictly after
# the last emitted (score, TID), which needs no such lookup.
#
# Twelve rows ranked by length, k = 4. Row 4 is deleted and committed before the
# cursor opens; the cursor fetches 3 rows; a second session vacuums; the cursor
# fetches the rest. The AM emits row 4's dead entry first (the executor drops it),
# then rebuilds. Before the fix row 5 was missing.

my $node = PostgreSQL::Test::Cluster->new('wand_tail_vanished_last');
$node->init;
# No autovacuum: the test decides when row 4 is vacuumed, and it must not be before
# the capped ranking is built.
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

# PLAIN storage with ~2.6 kB of padding puts three rows on each heap page, so row 4
# starts page 1. That layout is load-bearing: after FETCH 3 the cursor still has
# page 0 pinned (row 3 is there), and VACUUM needs a cleanup lock on row 4's page to
# remove it. If row 4 shared page 0, VACUUM could not remove it and the 12 -> 11
# witness below would fail loudly rather than the test passing vacuously.
$node->safe_psql('postgres', q{
	CREATE EXTENSION bm25_native;
	CREATE TABLE xd (id int, body text, pad text) WITH (autovacuum_enabled = off);
	ALTER TABLE xd ALTER pad SET STORAGE PLAIN;
	INSERT INTO xd SELECT n, 'alpha ' || repeat('zzq ', n), repeat('p', 2600)
	FROM generate_series(1, 12) n;
	CREATE INDEX xd_i ON xd USING bm25_native (body);
	DELETE FROM xd WHERE id = 4;
});

my $query = q{SELECT id, body &@@ 'alpha' AS d FROM xd WHERE body @@@ 'alpha'
	ORDER BY body &@@ 'alpha'};
my $gucs = q{SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;};

# The reference: the exhaustive scorer (no cap, so no tail) before anything changes.
# Row 4 is already invisible, so it is 1-3 and 5-12.
my $ref = $node->safe_psql('postgres',
	"$gucs SET bm25_native.wand_top_k = 0; $query");
my @ref = split /\n/, $ref;
is(join(',', map { (split /\|/)[0] } @ref), '1,2,3,5,6,7,8,9,10,11,12',
	'reference ranks the eleven visible rows by length');

# The ranking before the VACUUM still holds row 4's entry: bm25_debug_rank reads the
# index without a heap check.
is($node->safe_psql('postgres',
		"SELECT count(*) FROM bm25_debug_rank('xd_i', 'alpha')"),
	'12', 'before VACUUM the index still ranks the deleted row');

my $cur = $node->background_psql('postgres');
# Per-query timeout, so a slow cassert leg does not exhaust one budget for the file.
$cur->set_query_timer_restart();
$cur->query_safe("$gucs SET bm25_native.wand_top_k = 4");
$cur->query_safe('BEGIN');
$cur->query_safe("DECLARE c CURSOR FOR $query");
my @head = split /\n/, $cur->query_safe('FETCH 3 FROM c');
is(join(',', map { (split /\|/)[0] } @head), '1,2,3', 'first three rows from the capped ranking');

# Remove row 4 while the cursor is open. Its deleter committed before the cursor's
# snapshot, so the cursor does not hold it back.
$node->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON) xd');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM bm25_debug_rank('xd_i', 'alpha')"),
	'11', 'VACUUM tombstoned the deleted row: the rebuilt ranking will not hold it');

# The AM emits row 4's dead entry (the 4th and last capped entry), the executor drops
# it and pulls again, and the tail rebuild runs without that entry.
my @tail = split /\n/, $cur->query_safe('FETCH ALL FROM c');
$cur->query_safe('COMMIT');
$cur->quit;

my @got = (@head, @tail);
my @ids = map { (split /\|/)[0] } @got;
my @dist = map { (split /\|/)[1] } @got;

is(join(',', map { (split /\|/)[0] } @tail), '5,6,7,8,9,10,11,12',
	'the tail resumes at row 5, the row ranked just after the vanished entry');
my %seen;
is(scalar(grep { !$seen{$_}++ } @ids), scalar(@ids), 'no row emitted twice');
my $ordered = 1;
for my $i (1 .. $#dist)
{
	$ordered = 0 if $dist[$i] < $dist[ $i - 1 ];
}
ok($ordered, 'distances are non-decreasing (scores non-increasing)');
is(join("\n", @got), join("\n", @ref),
	'ids and distances equal the uncapped reference exactly');

$node->stop;
bm25_check_logs($node);
done_testing();
