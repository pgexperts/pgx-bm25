use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;
use Time::HiRes qw(time);

# REINDEX INDEX CONCURRENTLY vs. live DML (issue #156, BLD-04).
#
# WHAT THIS ADDS THAT sql/104 DOES NOT. sql/104_build_memory_budget.sql already
# runs REINDEX INDEX CONCURRENTLY under a shrunk bm25_native.debug_budget, and
# that proves the statement is ACCEPTED and answers correctly. What it cannot
# prove is anything about CONCURRENCY: pg_regress is single-connection and
# sequential, so its reindex runs against a table nobody is writing to. This
# suite supplies the missing half -- a second live session driving
# INSERT/UPDATE/DELETE straight through the rebuild.
#
# WHY THAT IS THE INTERESTING CASE FOR THIS AM. t/015_pending_append_race.pl
# documents a real pre-fix crash from ordinary concurrent INSERT versus seal:
# this AM carries its own concurrency machinery around the pending list and the
# seal singleton, which core knows nothing about. REINDEX CONCURRENTLY is the
# same shape -- build while the table keeps changing -- run through core's own
# multi-phase snapshot protocol, and it reaches the AM at two distinct points:
#
#   phase 2  index_concurrently_build() -> ambuild. The new index is indisready
#            = FALSE here (index_concurrently_build sets INDEX_CREATE_SET_READY
#            only AFTER index_build returns), so no other backend runs aminsert
#            into it -- but the HEAP is being rewritten under the build's
#            snapshot the whole time, and with a shrunk budget ambuild is
#            publishing segments as it goes rather than one at the end.
#   phase 3  validate_index(). indisready is now TRUE, so validate_index's own
#            index_insert of every missing row races the aminsert of every
#            concurrent writer -- two backends appending to one pending list,
#            either of which may cross seal_threshold and seal. That is exactly
#            t/015's window, reached through a different door.
#
# seal_threshold is deliberately left at its default here, unlike t/015: an
# opportunistic seal firing inside the race is part of what is being tested, not
# noise to be suppressed. Every assertion below is a heap-versus-index equality
# and holds however the sessions interleave.
#
# NO INJECTION POINTS. There is no wait-point extension in this tree, so the race
# window is made purely by making the rebuild genuinely slow: a 20000-document
# corpus of ~40 tokens each, rebuilt under bm25_native.debug_budget = 64kB. The
# budget is shrunk rather than the corpus grown -- the doctrine sql/104 and
# sql/83 state -- and it buys roughly a second of wall clock and a few thousand
# published segments on a fast machine.
#
# THE RACER SELF-TIMES. The DML is a server-side DO block with COMMIT inside the
# loop rather than a Perl loop, for three reasons, each of which a Perl loop gets
# wrong:
#   - Transaction length. REINDEX CONCURRENTLY calls WaitForLockersMultiple
#     before phase 2 and again before phase 3. ONE long writing transaction
#     (t/015's shape) would therefore be waited out BEFORE the build even starts
#     and the two would never overlap at all. Each loop iteration commits, so
#     every wait is brief.
#   - Cost. BackgroundPsql::query runs Data::Dumper over stdout/stderr on every
#     single query; thousands of round trips would cost more than the rebuild.
#   - Self-termination. The block polls pg_stat_activity, starts only once the
#     REINDEX is observed ACTIVE, and runs until it is observed gone. The window
#     is therefore covered end to end however long the rebuild takes -- on a slow
#     cassert/UBSan runner as much as here -- instead of being guessed at. The
#     block records WHY it stopped, and the suite fails if that was anything but
#     the rebuild finishing, so a run that raced only part of the window reports
#     itself instead of passing quietly.
# The DO block must be sent as its own simple query: several statements in one
# message form an implicit transaction block, in which COMMIT is an error.
#
# THE RACER IS THROTTLED ON PURPOSE. Unthrottled it commits ~27000 transactions
# per second here and exhausts any sane iteration cap inside the first 150 ms of
# a one-second rebuild -- covering the START of the window and nothing else,
# which was the first draft's actual behaviour. A short sleep per batch bounds
# the iteration count by TIME rather than by machine speed, so the same cap holds
# whether the rebuild takes one second or thirty.
#
# EVERY DML STREAM IS SUSTAINABLE. Updates cycle over a fixed id range, and once
# the original-row delete range is exhausted the deletes switch to retiring rows
# the racer itself inserted a thousand iterations earlier. A racer whose work
# runs out mid-window stops being a racer.
#
# WHAT A LOST ROW LOOKS LIKE. Every live row contains the token 'common', so the
# index-side id digest must equal the heap-side id digest exactly. A row the
# rebuild dropped shows up as a count mismatch plus a sum-of-ids difference that
# names it. The comparison is against a SEQSCAN of the heap, never a second
# reading through the same index: an index that lost rows agrees with itself.
#
# AND EVERY INDEX-SIDE PROBE HAS ITS PLAN ASSERTED, via index_probe() below. This
# is not belt-and-braces: `@@@` has a perfectly good HEAP implementation, and
#     SET enable_indexscan = off; SELECT count(*) FROM t WHERE body @@@ 'x';
# plans a Seq Scan with `Filter: (body @@@ 'x')` and returns the RIGHT ANSWER.
# enable_seqscan = off only penalises a seqscan, it does not forbid one. So an
# index-side probe that fell back to a heap scan would be comparing the heap
# against the heap and would pass no matter what the rebuild did -- exactly the
# false-pass shape this repo has been bitten by before.

my $ROWS        = 20000;    # base corpus
my $TOKENS      = 40;       # filler tokens per document; total tokens set build cost
my $UPD_RANGE   = 8000;     # updates cycle over ids 1..8000
my $DEL_FLOOR   = 12000;    # original-row deletes walk down from $ROWS to here
my $NEW_ID_BASE = 1000000;  # racer inserts live above every original id
my $ROLL_LAG    = 1000;     # rolling deletes retire an insert this many iterations old
my $MAX_ITERS   = 150000;   # ~2 minutes of throttled racing; a bound, not a target

my $node = PostgreSQL::Test::Cluster->new('reindex_concurrently_race');
$node->init;
$node->start;

# Run one index-side probe, asserting FIRST that it really reads the rebuilt
# index. See the "AND EVERY INDEX-SIDE PROBE" note above for why this cannot be
# skipped: a seqscan fallback answers @@@ correctly off the heap, which would
# turn every heap-versus-index equality below into heap-versus-heap.
sub index_probe
{
	my ($sql, $what) = @_;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $plan = $node->safe_psql('postgres',
		"SET enable_seqscan = off;\nEXPLAIN (COSTS OFF) $sql");
	ok($plan =~ /Index Scan using r_bm/ && $plan !~ /Seq Scan/,
		"probe '$what' reads through the rebuilt index")
	  or diag("plan was:\n$plan");

	return $node->safe_psql('postgres', "SET enable_seqscan = off;\n$sql");
}

# The heap side of every comparison, kept off every index so it is a genuinely
# independent reading of the same rows.
sub heap_probe
{
	my ($sql) = @_;
	return $node->safe_psql('postgres',
		"SET enable_indexscan = off; SET enable_bitmapscan = off;\n"
		  . "SET enable_indexonlyscan = off; SET enable_seqscan = on;\n$sql");
}

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', q{
	CREATE TABLE r (id int PRIMARY KEY, body text);
	CREATE TABLE race_log (
		iters int, ins int, upd int, del_orig int, del_new int,
		min_orig_deleted int,
		rows_at_start bigint, rows_at_end bigint,
		idsum_at_start numeric, idsum_at_end numeric,
		window_ms double precision, stopped text);
});

# Corpus. Every document carries 'common' (so one query must return the whole
# live table) and 'alpha' (which the racer's UPDATE strips, so 'alpha' is the
# probe for update visibility). The filler tokens are shared across documents so
# the accumulator's residency is dominated by postings rather than by distinct
# terms, which is what makes a 64kB budget reachable on a corpus this small.
$node->safe_psql('postgres', qq{
	INSERT INTO r
	SELECT g,
	       'common alpha doc' || g || ' ' ||
	       (SELECT string_agg('w' || ((g * 7 + s) % 5000), ' ')
	          FROM generate_series(1, $TOKENS) s)
	  FROM generate_series(1, $ROWS) g;
});
$node->safe_psql('postgres', 'CREATE INDEX r_bm ON r USING bm25_native (body)');

my $oid_before = $node->safe_psql('postgres', q{SELECT 'r_bm'::regclass::oid});
is($node->safe_psql('postgres', 'SELECT count(*) FROM r'), $ROWS,
	'corpus loaded');

# ------------------------------------------------------------------ the race
my $log_offset = -s $node->logfile;

# Fire the rebuild asynchronously. query_until with an empty pattern returns as
# soon as the statement has been WRITTEN to psql's stdin, not when it completes,
# so the backend may not even have begun -- which is why the racer waits for it
# to appear in pg_stat_activity rather than assuming it is already running.
my $h = $node->background_psql('postgres');
$h->query_safe(q{SET bm25_native.debug_budget = '64kB'});
my $t_fired = time();
$h->query_until(qr//, qq{REINDEX INDEX CONCURRENTLY r_bm;\n});

# The pg_stat_activity probe matches on 'REINDEX%' ANCHORED AT THE START. An
# unanchored match would also match this very DO block, whose source text
# contains the word: pid <> pg_backend_pid() covers that today, but only because
# no third session happens to be quoting it.
$node->safe_psql('postgres', qq{
DO \$\$
DECLARE
  nid   int;
  upid  int;
  delid int := $ROWS + 1;
  mindel int := NULL;
  iters int := 0;
  ins   int := 0;
  upd   int := 0;
  dorig int := 0;
  dnew  int := 0;
  active int;
  t0 timestamptz;
  t1 timestamptz;
  n0 bigint; n1 bigint;
  s0 numeric; s1 numeric;
  stopped text := 'never-seen';
  appear_deadline timestamptz := clock_timestamp() + interval '60 seconds';
  deadline timestamptz;
BEGIN
  -- Wait for the rebuild to actually be running before counting anything, so
  -- the measured window starts at the rebuild and not at psql's write().
  LOOP
    SELECT count(*) INTO active FROM pg_stat_activity
     WHERE pid <> pg_backend_pid()
       AND state = 'active'
       AND query ILIKE 'REINDEX%';
    EXIT WHEN active > 0 OR clock_timestamp() > appear_deadline;
    -- MANDATORY, not a tidy-up. pg_stat_activity is collected ONCE per
    -- transaction and cached for the rest of it (pgstat_read_current_status
    -- returns early while localBackendStatusTable is set; only
    -- pgstat_clear_snapshot resets it). Without this the loop re-reads its
    -- FIRST reading for the full 60s and can never see the rebuild appear --
    -- it passed only because a freshly forked psql usually loses the race to
    -- an already-connected session. The exit poll below does not need this: it
    -- re-enters a fresh transaction after each COMMIT.
    PERFORM pg_stat_clear_snapshot();
    PERFORM pg_sleep(0.001);
  END LOOP;
  IF active = 0 THEN
    INSERT INTO race_log
      VALUES (0, 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, stopped);
    RETURN;
  END IF;

  t0 := clock_timestamp();
  deadline := t0 + interval '300 seconds';
  SELECT count(*), coalesce(sum(id::bigint), 0) INTO n0, s0 FROM r;
  stopped := 'deadline';

  LOOP
    -- A batch of short transactions. One COMMIT per iteration keeps every
    -- WaitForLockers the rebuild performs brief; the sleep after the batch
    -- bounds the iteration count by wall clock rather than by how fast this
    -- machine can commit.
    FOR k IN 1..10 LOOP
      iters := iters + 1;

      nid := $NEW_ID_BASE + iters;
      INSERT INTO r VALUES (nid, 'common racerins doc' || nid);
      ins := ins + 1;

      -- Cycles over a fixed range that the deletes never touch, so the update
      -- stream cannot run out however long the rebuild takes. Rewriting the
      -- indexed column makes every one of these a non-HOT update.
      upid := ((iters - 1) % $UPD_RANGE) + 1;
      UPDATE r SET body = 'common racerupd doc' || upid WHERE id = upid;
      upd := upd + 1;

      IF delid > $DEL_FLOOR THEN
        delid := delid - 1;
        DELETE FROM r WHERE id = delid;
        mindel := delid;
        dorig := dorig + 1;
      ELSIF iters > $ROLL_LAG THEN
        DELETE FROM r WHERE id = $NEW_ID_BASE + iters - $ROLL_LAG;
        dnew := dnew + 1;
      END IF;

      COMMIT;
    END LOOP;

    SELECT count(*) INTO active FROM pg_stat_activity
     WHERE pid <> pg_backend_pid()
       AND state = 'active'
       AND query ILIKE 'REINDEX%';
    IF active = 0 THEN
      stopped := 'rebuild-finished';
      EXIT;
    END IF;
    IF iters >= $MAX_ITERS THEN
      stopped := 'cap';
      EXIT;
    END IF;
    EXIT WHEN clock_timestamp() > deadline;

    PERFORM pg_sleep(0.005);
  END LOOP;

  t1 := clock_timestamp();
  SELECT count(*), coalesce(sum(id::bigint), 0) INTO n1, s1 FROM r;
  INSERT INTO race_log
    VALUES (iters, ins, upd, dorig, dnew, mindel, n0, n1, s0, s1,
            extract(epoch FROM (t1 - t0)) * 1000, stopped);
END \$\$;
});

ok($h->quit, 'the background REINDEX session exited cleanly');
my $elapsed = time() - $t_fired;

# ------------------------------------------------------- did a race happen?
my ($iters, $ins, $upd, $dorig, $dnew, $mindel, $n0, $n1, $s0, $s1,
	$window_ms, $stopped) = split /\|/,
	$node->safe_psql('postgres',
		q{SELECT iters, ins, upd, del_orig, del_new,
		         coalesce(min_orig_deleted::text, ''),
		         rows_at_start, rows_at_end, idsum_at_start, idsum_at_end,
		         round(window_ms::numeric, 1), stopped
		    FROM race_log});

# Only empty if the racer never deleted an original row, which its own assertion
# below reports. Substituting an empty range keeps the two range queries valid
# SQL so that failure is reported once instead of cascading into a syntax error.
$mindel = $ROWS + 1 if !defined $mindel || $mindel eq '';

diag(sprintf(
	"race window: %s ms observed by the racer (%.2f s wall from fire to quit), "
	. "stopped=%s; %d transactions committed inside it "
	. "(%d INSERT, %d UPDATE, %d DELETE of original rows, %d DELETE of racer rows); "
	. "live rows %s -> %s",
	$window_ms, $elapsed, $stopped, $iters, $ins, $upd, $dorig, $dnew, $n0, $n1));

# These are the teeth of the whole suite. If the rebuild had finished before the
# racer got going, or the racer had never committed anything, every equality
# below would hold vacuously and the suite would be green while proving nothing.
is($stopped, 'rebuild-finished',
	'the racer drove DML from the start of the rebuild to the end of it');
# A floor, not just >0. `stopped='rebuild-finished'` proves the racer saw the
# rebuild once and later saw it gone; it does NOT bound how much of the rebuild
# it spanned. A run that first catches the rebuild in its last phase yields a
# handful of iterations, every equality below holds, and the suite is green
# having exercised almost no concurrency -- which gets MORE likely on a fast
# machine. This floor is far under the ~950 observed locally and well over what
# a near-miss produces.
cmp_ok($iters, '>=', 100,
	'the racer committed a substantial number of transactions inside the rebuild');
# count(*) alone is NOT a witness here: inserts and deletes run at one apiece, so
# a balanced window leaves the row COUNT unchanged while the row SET has turned
# over completely. The id sum moves because the racer's ids live above 1000000.
isnt($s0, $s1,
	'the live row set actually changed across the rebuild (not a static table)');

my $nsegs = $node->safe_psql('postgres',
	q{SELECT count(*) FROM bm25_debug_segcat('r_bm')});
diag("rebuilt index holds $nsegs segments");
# NOT a witness for the chunked ambuild path, though it looks like one: this is
# read after the race, and by then phase 3's aminserts and any opportunistic
# seal have published segments of their own, so >1 is satisfiable by a
# single-segment build. sql/104 owns that A/B cleanly with no concurrent
# writer. All this asserts is that the rebuilt index is populated and walkable.
cmp_ok($nsegs, '>', 0, 'the rebuilt index holds segments');

# REINDEX CONCURRENTLY builds a new relation under a new OID and swaps it in. If
# it had silently done nothing, every assertion below would be testing the index
# that existed before the race.
my $oid_after = $node->safe_psql('postgres', q{SELECT 'r_bm'::regclass::oid});
isnt($oid_after, $oid_before, 'the index was genuinely rebuilt under a new OID');

my ($indisvalid, $indisready) = split /\|/, $node->safe_psql('postgres',
	q{SELECT indisvalid, indisready FROM pg_index
	   WHERE indexrelid = 'r_bm'::regclass});
is($indisvalid, 't', 'rebuilt index is indisvalid');
is($indisready, 't', 'rebuilt index is indisready');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		   WHERE i.indrelid = 'r'::regclass AND c.relam =
		         (SELECT oid FROM pg_am WHERE amname = 'bm25_native')}),
	'1', 'exactly one bm25 index survives (the old one was dropped)');

# ------------------------------------------------------------- the assertions

# Complete: every live heap row is findable through the rebuilt index. Compared
# against a SEQSCAN of the heap. count catches a loss, sum(id) names it when a
# single row is missing, and the md5 catches a substitution that preserves both.
my $digest = q{SELECT count(*) || '/' || coalesce(sum(id::bigint), 0) || '/' ||
	       coalesce(md5(string_agg(id::text, ',' ORDER BY id)), '') FROM r};
is(index_probe("$digest WHERE body \@\@\@ 'common';", 'complete-id-set'),
	heap_probe("$digest;"),
	'every live heap row is findable through the rebuilt index');

# Update visibility: the racer's UPDATE rewrites the body and drops 'alpha'. A
# rebuild that captured the pre-update version of a row, or lost the post-update
# version, diverges from the heap here.
my $heap_alpha = heap_probe(q{SELECT count(*) FROM r WHERE body LIKE '%alpha%';});
is(index_probe(q{SELECT count(*) FROM r WHERE body @@@ 'alpha';}, 'alpha'),
	$heap_alpha,
	'rows updated during the rebuild are indexed at their new value');
cmp_ok($heap_alpha, '<', $ROWS,
	'the UPDATE really removed a token from some rows (the probe is not vacuous)');

my $heap_upd = heap_probe(q{SELECT count(*) FROM r WHERE body LIKE '%racerupd%';});
is(index_probe(q{SELECT count(*) FROM r WHERE body @@@ 'racerupd';}, 'racerupd'),
	$heap_upd, 'the new value of every updated row is findable');
cmp_ok($heap_upd, '>', 0, 'some rows really were updated during the rebuild');

my $heap_ins = heap_probe(q{SELECT count(*) FROM r WHERE body LIKE '%racerins%';});
is(index_probe(q{SELECT count(*) FROM r WHERE body @@@ 'racerins';}, 'racerins'),
	$heap_ins, 'every row inserted during the rebuild is findable');
cmp_ok($heap_ins, '>', 0, 'some rows really were inserted during the rebuild');

# Nothing deleted during the rebuild is still returned. The original-row deletes
# walked DOWN from $ROWS, so [min_orig_deleted .. $ROWS] must be empty on both
# sides. Asserted through the index AND against the heap: the index side alone
# would be satisfied by an entry whose heap tuple is merely dead.
cmp_ok($dorig, '>', 0, 'some original rows really were deleted during the rebuild');
# OFFSET 0 is an optimization fence, and it is load-bearing. Written flat, the
# planner reads `id >= .. AND id <= ..` as by far the more selective qual and
# answers the whole thing from the PRIMARY KEY with `body @@@ 'common'` demoted
# to a filter -- a Bitmap Heap Scan on r_pkey that says nothing whatever about
# the rebuilt bm25 index. (Not hypothetical: that is what this probe did until
# index_probe's plan assertion caught it.) The fence makes the bm25 scan produce
# the id set and applies the range to its OUTPUT.
is(index_probe(
		qq{SELECT count(*) FROM
		     (SELECT id FROM r WHERE body \@\@\@ 'common' OFFSET 0) s
		    WHERE s.id >= $mindel AND s.id <= $ROWS;},
		'deleted-range'),
	'0', 'no row deleted during the rebuild is still returned');
is(heap_probe(qq{SELECT count(*) FROM r WHERE id >= $mindel AND id <= $ROWS;}),
	'0', 'and the heap agrees those rows are gone');

# Read-your-writes on the rebuilt index: a post-rebuild INSERT lands in the new
# index's pending list and is visible immediately. 'zebracrossing' is not a
# stopword and does not stem into anything else in the corpus.
$node->safe_psql('postgres',
	q{INSERT INTO r VALUES (999999, 'common zebracrossing settled row')});
is(index_probe(
		q{SELECT array_agg(id ORDER BY id) FROM r WHERE body @@@ 'zebracrossing';},
		'zebracrossing'),
	'{999999}', 'the rebuilt index takes writes and reads them back');

# A seal after the race must neither lose nor duplicate: BM25 scores are
# additive, so a document drained twice would be double-counted as well as
# double-returned.
$node->safe_psql('postgres', q{SELECT bm25_seal('r_bm')});
is(index_probe(q{SELECT count(*) FROM r WHERE body @@@ 'common';}, 'post-seal'),
	heap_probe(q{SELECT count(*) FROM r;}),
	'a seal after the race neither loses nor duplicates documents');

# No backend died at any point. Any of the safe_psql calls above would already
# have failed on a crash, but a backend that died and was restarted BETWEEN them
# would otherwise leave no assertion behind.
ok(!$node->log_contains(
		qr/(was terminated by signal|terminating connection because of crash|PANIC:|TRAP:)/,
		$log_offset),
	'no backend crashed during the concurrent rebuild');

$node->stop;
bm25_check_logs($node);
done_testing();
