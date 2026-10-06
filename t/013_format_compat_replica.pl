use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Bidirectional-skew replica safety (roadmap #4 Task 7): a physical streaming
# standby running the SAME binary as the primary must react correctly no
# matter which way the primary's on-disk format skews relative to what this
# binary understands. Both skew directions are driven purely by ordinary WAL
# replay (no custom rmgr, no shared_preload -- Generic WAL only, matching the
# existing v4/v5 replica suites t/007 and t/011):
#
#   * BREAKING (min_read_version raised past this build's BM25_FORMAT_VERSION):
#     bm25_debug_stamp_version's poke is itself a WAL-logged metapage write, so
#     it replays onto the standby exactly as it landed on the primary. The
#     standby's bm25_meta_validate (enforced on the query path via
#     bm25_scan_snapshot) must then refuse the raised floor with the SAME
#     clean forward-refuse error the primary would give itself -- never a
#     replay PANIC, and never a silent mis-read of a page shaped differently
#     than this build expects.
#   * ADDITIVE (a synthetic optional region appended past the metapage struct,
#     min_read_version left unmoved): the region's bytes are real, WAL-logged
#     content on the standby's copy of block 0 too, but bm25_meta_read's
#     fixed-size struct copy means the standby reads straight through it, just
#     like the primary does.
#
# Ordering is deliberate and load-bearing: the breaking-floor subtest (2) runs
# BEFORE the additive one (3), and (3)'s very first action is restoring the
# floor. That means the destructive poke never leaves the fixture wedged for a
# later subtest -- there is no separate "restore" step floating in between for
# a future edit to accidentally drop or reorder.

my $primary = PostgreSQL::Test::Cluster->new('fc_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$primary->safe_psql('postgres', 'CREATE TABLE fc(id int primary key, body text)');
$primary->safe_psql('postgres',
	qq{INSERT INTO fc SELECT g, 'tortoise hare ' || (g % 5) FROM generate_series(1, 20) g});
$primary->safe_psql('postgres', 'CREATE INDEX fc_bm25 ON fc USING bm25_native (body)');

my $backup_name = 'fc_replica_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('fc_standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# Single-key ORDER BY (no secondary tiebreak column) is the house discriminating
# style for ranked-sequence assertions -- a tiebreak key can mask a rank
# collapse instead of exposing one. enable_seqscan=off on both nodes forces the
# same (index scan) plan the comparison is meant to exercise.
my $ranked_query =
	qq{SET enable_seqscan=off; SELECT id FROM fc WHERE body \@\@\@ 'tortoise' ORDER BY body &\@\@ 'tortoise' LIMIT 3};

# --- (1) Baseline equality: before any poke, primary and standby agree exactly.
my $baseline_primary = $primary->safe_psql('postgres', $ranked_query);
my $baseline_standby = $standby->safe_psql('postgres', $ranked_query);
ok($baseline_primary ne '', "baseline ranked query returns a non-trivial result");
is($baseline_standby, $baseline_primary,
	"baseline: standby ranked result identical to primary before any format poke");

# --- (2) Breaking floor propagates through WAL replay -> standby refuses cleanly.
# min_read_version=9 is one past this build's BM25_FORMAT_VERSION(8): the
# forward check in bm25_meta_validate refuses. The poke is a WAL-logged
# metapage write, so replaying it onto the standby raises the SAME floor there
# -- proving the floor travels via ordinary WAL replay, with no replica-side
# awareness of the new version required.
# NOTE: these literals are "this build + 1", not fixed numbers -- bump them with
# BM25_FORMAT_VERSION. When the build reached v7 the old literal 7 stopped being in the
# future and this subtest began asserting a refusal that no longer happened.
$primary->safe_psql('postgres', qq{SELECT bm25_debug_stamp_version('fc_bm25', 9, 9, 0)});
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# The primary's own next query against this index would ALSO refuse now (same
# gate, same replicated metapage) -- expected, and deliberately not asserted
# here; this subtest is about the standby's behavior after replaying the poke.
#
# enable_seqscan=off is REQUIRED, not decorative: @@@ has a standalone
# (non-index) evaluator, bm25_match(text,text), and bm25_costestimate's fixed
# floor (10.0 + 0.01*tuples) prices the index path well above a seq scan of a
# 20-row table -- so at default settings the planner picks Seq Scan, the query
# SUCCEEDS without ever reading the metapage, and the gate below is never
# exercised. Each ->psql/->safe_psql is a BRAND-NEW psql process, so the SET
# embedded in $ranked_query does not carry over to this statement; it must be
# set here explicitly.
my ($rc, undef, $err) = $standby->psql('postgres',
	qq{SET enable_seqscan=off; SELECT count(*) FROM fc WHERE body \@\@\@ 'tortoise'});
ok($rc != 0, "standby query fails against a raised floor (does not silently succeed)");
# The version in this pattern is the FORGED floor above, i.e. "this build + 1" -- bump it
# with BM25_FORMAT_VERSION alongside the stamp_version literals.
like($err, qr/requires extension format >= 9/,
	"standby refuses a raised floor cleanly with the forward-refuse message, not a crash");

# --- (3) Additive read-through: restore the floor, then prove a real additive
#     region replays onto the standby and is read straight through, just as it
#     is on the primary. Restoring the floor FIRST (rather than as a trailing
#     cleanup step after subtest 2) is what keeps the destructive poke above
#     from wedging this subtest.
$primary->safe_psql('postgres', qq{SELECT bm25_debug_stamp_version('fc_bm25', 8, 5, 4)});
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

# Ordering here is incidental, not load-bearing: every metapage writer RAISES
# pd_lower (bm25_meta_set_pd_lower) rather than assigning it, so no later write
# can retract a previously-written optional region back into the zeroed page
# hole. That raise is exactly what makes an additive tail survive an older
# binary's writes, and it is pinned by sql/55 cases (C)/(G); this subtest's job
# is only to prove the region REPLICATES and reads through on a standby.
$primary->safe_psql('postgres',
	qq{SELECT bm25_debug_stamp_version('fc_bm25', 9, 5, (1::bigint<<31))});
$primary->safe_psql('postgres', qq{SELECT bm25_debug_write_optional_region('fc_bm25')});
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

my $additive_standby = $standby->safe_psql('postgres', $ranked_query);
is($additive_standby, $baseline_primary,
	"standby reads through a replicated additive region, same rows as baseline");

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
