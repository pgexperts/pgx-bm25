use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Upgrade round-trip + crash recovery on both sides of the upgrade's swap (roadmap
# #4 Task 8).
#
# (a) proves bm25_upgrade actually moves a forged legacy-v5 index to this build's
#     BM25_FORMAT_VERSION and that queries are unaffected -- the ordinary,
#     no-crash path.
# (b) proves crash recovery lands on a complete state on each side of the swap that
#     bm25_upgrade's segment-rewrite path rides (bm25_segcat_publish_swap, with the
#     version re-stamp folded into the SAME WAL record as the catalog flip -- see
#     src/bm25_upgrade.c): a crash before the upgrade replays to the full legacy
#     state, and a crash after it replays to the full upgraded one.
#
# Both landings are reached deterministically, one crash each. The earlier shape --
# fire the upgrade asynchronously and stop('immediate') without waiting -- never
# reached either (issue #226). bm25_upgrade and bm25_debug_stamp_version, like every
# bm25 maintenance entry point, write Generic WAL without assigning an xid, and
# RecordTransactionCommit flushes WAL only when
#     (wrote_xlog && markXidCommitted && synchronous_commit > off)
#         || forceSyncCommit || nrels > 0
# so neither call's commit flushes anything. Nor does a later bare txid_current():
# wrote_xlog is sampled as (XactLastRecEnd != 0) BEFORE the commit record is written,
# and XactLastRecEnd is reset at every transaction end, so it is true only if THAT
# transaction wrote WAL of its own; an xid-only transaction commits asynchronously.
# An immediate stop discards everything past the last flush, and recovery replays
# none of it: the forge was lost, the in-flight upgrade was lost, and every run landed
# on the plain as-built index, which also reads format_version 8. So:
#   (b1) forge the legacy metapage, CHECKPOINT it into the data files, then crash;
#   (b2) run the upgrade to completion, flush WAL with pg_switch_wal() -- whose
#        XLOG_SWITCH record is XLogFlush()ed on insert -- NOT a checkpoint, so
#        recovery has to REPLAY the swap record rather than find its pages already
#        written -- then crash, and check from the server log that the redo window
#        covered the whole upgrade.
# A torn mid-swap state is excluded by construction (the flip and the re-stamp are
# one record), not by a crash that happens to land inside the swap; no Perl-driven
# stop can reach the inside of that record.

my $node = PostgreSQL::Test::Cluster->new('upgrade_crash');
$node->init;
# Autovacuum's amvacuumcleanup seals the pending list. On a slow runner its first
# worker (~naptime after start) could fire before the crash and move the pending and
# segment counts pinned below, so the fixture keeps it off.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

# --- (a) Round-trip: build, forge legacy v5, upgrade, assert the current version +
#     unchanged query.

$node->safe_psql('postgres', 'CREATE TABLE rt(id int primary key, body text)');
$node->safe_psql('postgres',
	qq{INSERT INTO rt SELECT g, 'apple berry ' || (g % 6) FROM generate_series(1, 200) g});
$node->safe_psql('postgres', 'CREATE INDEX rt_bm25 ON rt USING bm25_native (body)');

# Single-key ORDER BY (no secondary tiebreak column) is the house discriminating
# style for ranked-sequence assertions -- a tiebreak key can mask a rank collapse.
my $ranked_a =
	qq{SET enable_seqscan=off; SELECT id FROM rt WHERE body \@\@\@ 'apple' ORDER BY body &\@\@ 'apple' LIMIT 5};
my $before_a = $node->safe_psql('postgres', $ranked_a);
ok($before_a ne '', 'round-trip: baseline ranked query returns rows');

$node->safe_psql('postgres', qq{SELECT bm25_debug_stamp_version('rt_bm25', 5, 0, 0)});
is($node->safe_psql('postgres', qq{SELECT format_version FROM bm25_stats('rt_bm25')}),
	'5', 'round-trip: forged legacy format_version=5 before upgrade');

my $summary_a = $node->safe_psql('postgres', qq{SELECT bm25_upgrade('rt_bm25')});
# The target version in these assertions tracks BM25_FORMAT_VERSION and must be bumped
# with it (v6 -> v7 for the pending-document spanning change #57; v7 -> v8 for the
# pending per-field doclen array, #184).
like($summary_a, qr/^upgraded 0 segments: v5 -> v8$/,
	'round-trip: metadata-only upgrade summary (no segment rewrite registered)');

is($node->safe_psql('postgres', qq{SELECT format_version FROM bm25_stats('rt_bm25')}),
	'8', 'round-trip: format_version reaches 8 after upgrade');
is($node->safe_psql('postgres', qq{SELECT min_read_version FROM bm25_stats('rt_bm25')}),
	'5', 'round-trip: min_read_version reaches 5 (BM25_OLDEST_READABLE) after upgrade -- '
	   . 'both lazy floors (the v7 spanning one and the v8 per-field-doclen one) are '
	   . 'raised by WRITES, not by an upgrade, and this fixture only ever built');

my $after_a = $node->safe_psql('postgres', $ranked_a);
is($after_a, $before_a, 'round-trip: ranked query unchanged after upgrade');

# --- (b) Crash recovery on either side of the swap. The synthetic transform forces
#     the segment-rewrite + atomic-swap path (the one whose atomicity is actually in
#     question -- the metadata-only path in (a) is a single buffer write with nothing
#     to tear).

$node->safe_psql('postgres', 'CREATE TABLE cu(id int primary key, body text)');
$node->safe_psql('postgres',
	qq{INSERT INTO cu SELECT g, 'quartz granite ' || (g % 6) FROM generate_series(1, 2000) g});
$node->safe_psql('postgres', 'CREATE INDEX cu_bm25 ON cu USING bm25_native (body)');

# Build up multiple sealed segments plus a pending tail, so the rewrite has real
# multi-segment work to do: the upgrade seals the tail, then re-emits every segment
# and retires the old ones in the swap record. Each INSERT below writes WAL under an
# xid, so its commit is synchronous and also flushes the xid-less seal/merge WAL
# written before it.
$node->safe_psql('postgres',
	qq{INSERT INTO cu SELECT g, 'quartz extra' FROM generate_series(2001, 2500) g});
$node->safe_psql('postgres', qq{SELECT bm25_seal('cu_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO cu SELECT g, 'quartz more' FROM generate_series(2501, 3000) g});
$node->safe_psql('postgres', qq{SELECT bm25_merge('cu_bm25')});
$node->safe_psql('postgres',
	qq{INSERT INTO cu SELECT g, 'quartz pending' FROM generate_series(3001, 3200) g});

my $ranked_b =
	qq{SET enable_seqscan=off; SELECT id FROM cu WHERE body \@\@\@ 'quartz' ORDER BY body &\@\@ 'quartz' LIMIT 5};
my $before_b = $node->safe_psql('postgres', $ranked_b);
ok($before_b ne '', 'crash: baseline ranked query returns rows before upgrade');
my $heap_b = $node->safe_psql('postgres', qq{SELECT count(*) FROM cu WHERE body LIKE '%quartz%'});

# The WAL-logged state that tells the landings apart. format_version alone cannot:
# the as-built index and the upgraded one both read 8. pending_ndocs and the
# retired-range count move only in records the upgrade writes (the seal's publish
# record and the swap record), and relation extension cannot fake them the way it
# fakes bm25_debug_npages -- smgrzeroextend reaches the file without WAL, so a lost
# upgrade still leaves the relation longer.
sub cu_state
{
	my ($n) = @_;
	my ($fv, $mrv, $nsegs, $pending) = split /\|/,
	  $n->safe_psql('postgres',
		qq{SELECT format_version, min_read_version, nsegs, pending_ndocs FROM bm25_stats('cu_bm25')});
	return {
		fv      => $fv,
		mrv     => $mrv,
		nsegs   => $nsegs,
		pending => $pending,
		retired => $n->safe_psql('postgres', qq{SELECT bm25_debug_retired_count('cu_bm25')}) };
}

# Ranked order and match count must come back unchanged on EITHER landing: the
# rewrite is semantically identity (every doc's tid/key/doclen/postings replayed),
# so the same top-5 sequence must come back whether it is read from the old
# segments or the new ones, with no doc lost or duplicated.
sub check_queries
{
	my ($landing) = @_;
	is($node->safe_psql('postgres', $ranked_b), $before_b,
		"$landing: ranked query returns the same rows after recovery");
	is($node->safe_psql('postgres',
			qq{SET enable_seqscan=off; SELECT count(*) FROM cu WHERE body \@\@\@ 'quartz'}),
		$heap_b, "$landing: no doc loss or duplication after recovery");
}

# (b1) Pre-swap landing. Forge the legacy v5 metapage, then CHECKPOINT: the forge's
# commit flushes nothing (see the header), so without the checkpoint the crash loses
# it and recovery lands on the as-built v8 index -- which the format_version and
# min_read_version assertions below then catch.
$node->safe_psql('postgres', qq{SELECT bm25_debug_stamp_version('cu_bm25', 5, 0, 0)});
$node->safe_psql('postgres', 'CHECKPOINT');
my $forged = cu_state($node);
$node->stop('immediate');
$node->start;

my $b1 = cu_state($node);
is($b1->{fv}, '5', 'pre-swap crash: the forged legacy format_version survived recovery');
is($b1->{mrv}, '0', 'pre-swap crash: the forged legacy min_read_version survived recovery');
is($b1->{pending}, $forged->{pending},
	"pre-swap crash: the pending tail is still pending ($b1->{pending} docs)");
is($b1->{retired}, $forged->{retired}, 'pre-swap crash: no segment was retired');
check_queries('pre-swap crash');

# (b2) Post-swap landing. bm25_debug_enable_synthetic_transform sets a BACKEND-LOCAL
# flag, so it MUST run in the same session as bm25_upgrade() -- one safe_psql -- or
# the toggle is silently lost and this exercises the metadata-only path instead.
my $upgrade_start = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $summary_b = $node->safe_psql('postgres',
	qq{SELECT bm25_debug_enable_synthetic_transform(true); SELECT bm25_upgrade('cu_bm25')});
like($summary_b, qr/^upgraded [1-9]\d* segments: v5 -> v8$/m,
	'post-swap crash: the upgrade took the segment-rewrite path');
my $upgrade_end = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

# The upgrade's commit flushed nothing. pg_switch_wal() XLogFlush()es through its
# XLOG_SWITCH record, which lies past every record the upgrade wrote. A CHECKPOINT
# would flush too, but it would also write the swapped pages out, and recovery would
# then have nothing of the upgrade left to replay.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $log_offset = -s $node->logfile;
$node->stop('immediate');
$node->start;

# Proof that recovery REPLAYED the upgrade rather than finding it on disk: redo began
# at or before the upgrade's first record and ran at least to its last. ("redo done
# at" names the last record replayed; the XLOG_SWITCH record starts at or past
# $upgrade_end, so it is at or beyond it exactly when that switch was replayed.)
my $recovery_log = slurp_file($node->logfile, $log_offset);
my ($redo_start) = $recovery_log =~ /redo starts at (\S+)/;
my ($redo_done)  = $recovery_log =~ /redo done at (\S+)/;
ok(defined $redo_start && defined $redo_done, 'post-swap crash: recovery ran redo');
is($node->safe_psql('postgres',
		qq{SELECT '} . ($redo_start // 'FFFFFFFF/FFFFFFFF') . qq{'::pg_lsn <= '$upgrade_start'::pg_lsn
		   AND '} . ($redo_done // '0/0') . qq{'::pg_lsn >= '$upgrade_end'::pg_lsn}),
	't',
	'post-swap crash: the redo window covered the whole upgrade, swap record included '
	  . "(redo " . ($redo_start // '?') . " .. " . ($redo_done // '?')
	  . ", upgrade $upgrade_start .. $upgrade_end)");

my $b2 = cu_state($node);
is($b2->{fv}, '8', 'post-swap crash: the re-stamped format_version was replayed');
# BM25_OLDEST_READABLE: the upgrade raises the forged 0 to Max(0, 5). A lazily-raised
# floor of 8 cannot appear here -- it is written by pending APPENDS, and every append
# in this fixture happened before the forge reset the floor to 0.
is($b2->{mrv}, '5', 'post-swap crash: min_read_version is the upgrade\'s floor');
is($b2->{pending}, '0', 'post-swap crash: the pending tail the upgrade sealed stayed sealed');
cmp_ok($b2->{retired}, '>', $b1->{retired},
	"post-swap crash: the swap record's retired ranges were replayed "
	  . "($b1->{retired} -> $b2->{retired})");
check_queries('post-swap crash');

$node->stop;
bm25_check_logs($node);
done_testing();
