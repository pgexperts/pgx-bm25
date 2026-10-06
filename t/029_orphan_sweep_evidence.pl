use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# The orphan sweep's gate (issue #300): the evidence that makes a VACUUM sweep must
# survive a crash and an ERROR, and a crash-restart must make the next VACUUM sweep
# even when no bracket was left open.
#
#   (A) Crash after an unpublished build. The seal's orphan bracket is a metapage WAL
#       record written before the build's first allocation, so recovery replays it
#       with the orphan pages, and the first VACUUM after the restart frees them.
#   (B) A crash-RESTART, not a postmaster restart: a backend is SIGKILLed and the
#       postmaster reinitialises shared memory in place (restart_after_crash), keeping
#       its start time. No bracket is open, so only the crash epoch -- a value that
#       lives in a named DSM segment, re-created with shared memory -- can tell the
#       next VACUUM to sweep. A postmaster-start-time marker would miss this.
#   (C) An ERROR in bm25_reclaim_retired between compacting a descriptor and freeing
#       the ranges it dropped leaks those ranges; its bracket stays open, and the next
#       VACUUM -- same server lifetime, so the epoch cannot help -- sweeps them.
#
# Each scenario then checks the other half of the gate: once the sweep has run, an
# idle VACUUM must NOT sweep. That is asserted behaviourally -- throttled so that a
# sweep of this index outlasts the statement timeout by a wide margin -- and is what
# a build that sweeps on every VACUUM fails. The negative controls for (B) and (C)
# are the source reverts recorded with the change: an epoch taken from the
# postmaster start time fails (B), and an unbracketed reclaim_retired fails (C).

my $node = PostgreSQL::Test::Cluster->new('orphan_sweep_evidence');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
restart_after_crash = on
checkpoint_timeout = 1h
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

use constant PAGE_SEGCAT  => 1 << 2;
use constant PAGE_DELETED => 1 << 9;

my $pause_key = 1651323445;
# Part (B) SIGKILLs one backend; the end-of-run log check exempts exactly its PID.
my $victim_pid;
my %point = (reclaim_retired_compacted => 12);

# A table whose index is big enough (over 500 pages) that a throttled sweep takes
# many times the 5 s statement timeout below.
sub big_table
{
	my ($t) = @_;
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		INSERT INTO $t SELECT g, (SELECT string_agg('w' || ((g * k) % 4999), ' ')
		                            FROM generate_series(1, 40) k)
		  FROM generate_series(1, 20000) g;
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
	});
}

sub evidence
{
	return $node->safe_psql('postgres',
		"SELECT ops_begun || ',' || ops_done || ',' || CASE WHEN swept_this_epoch THEN 't' ELSE 'f' END
		   FROM bm25_debug_sweep_evidence('$_[0]_idx')");
}

# Sweeps run by one fresh session's VACUUM.
sub vacuum_sweeps
{
	return $node->safe_psql('postgres',
		"VACUUM $_[0]; SELECT bm25_debug_orphan_sweeps()");
}

# The idle-VACUUM half: an unthrottled VACUUM (it leaves the heap all-visible, so the
# throttled one's heap pass skips it), then a throttled, time-limited one. Neither may
# sweep, and the throttled one must finish.
sub idle_vacuum_does_not_sweep
{
	my ($t, $label) = @_;
	my ($rc, $out, $err) = $node->psql('postgres', qq{
		VACUUM $t;
		SET vacuum_cost_delay = 20;
		SET vacuum_cost_limit = 1;
		SET statement_timeout = '5s';
		VACUUM $t;
		SELECT bm25_debug_orphan_sweeps();});
	is($rc, 0, "$label: a throttled idle VACUUM finishes inside the timeout") or diag($err);
	is($out, '0', "$label: and neither idle VACUUM swept");
}

sub flags
{
	my ($t, $blk) = @_;
	return $node->safe_psql('postgres', "SELECT bm25_debug_page_flags('${t}_idx', $blk)");
}

# ---- (A) crash after an unpublished build ----------------------------------------
{
	big_table('ta');
	is(vacuum_sweeps('ta'), '1', '(A) a new index is swept by its first VACUUM');
	my $orphan = $node->safe_psql('postgres', q{
		INSERT INTO ta SELECT g, 'crash orphan ' || g FROM generate_series(20001, 20100) g;
		SELECT bm25_debug_seal_unpublished('ta_idx')});
	like($orphan, qr/^\d+$/, "(A) a build left an unpublished segment at block $orphan");
	# Maintenance records carry no xid, so nothing has flushed them yet.
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
	$node->stop('immediate');
	$node->start;

	my ($b, $d, $e) = split /,/, evidence('ta');
	cmp_ok($b, '>', $d, '(A) the open orphan bracket survived the crash');
	is(flags('ta', $orphan) & PAGE_DELETED, 0, '(A) the orphan is still unfreed after recovery');
	is(vacuum_sweeps('ta'), '1', '(A) the first VACUUM after the crash swept');
	is(flags('ta', $orphan) & PAGE_DELETED, PAGE_DELETED, '(A) and freed the orphan');
	($b, $d, $e) = split /,/, evidence('ta');
	ok($b == $d && $e eq 't', '(A) the sweep retired the evidence');
	idle_vacuum_does_not_sweep('ta', '(A)');
}

# ---- (B) crash-restart: only the epoch says "sweep" ------------------------------
{
	big_table('tb');
	is(vacuum_sweeps('tb'), '1', '(B) a new index is swept by its first VACUUM');
	is(evidence('tb'), '0,0,t', '(B) no bracket open, swept in this server lifetime');
	idle_vacuum_does_not_sweep('tb', '(B) before the crash');

	# VACUUM assigns no xid, so the sweep's completion record is flushed only
	# asynchronously; a crash before the walwriter gets to it would revert
	# swept_epoch to 0 and make this part pass for the wrong reason (it would then
	# not distinguish the shared-memory nonce from the postmaster start time).
	# pg_switch_wal() XLogFlush()es everything written so far.
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

	my $pm_start = $node->safe_psql('postgres', 'SELECT pg_postmaster_start_time()');
	my $victim = $node->background_psql('postgres');
	$victim_pid = $victim->query_safe('SELECT pg_backend_pid()');
	kill 'KILL', $victim_pid;
	eval { $victim->quit };
	$node->poll_query_until('postgres', 'SELECT true')
	  or die 'the server did not come back after the backend crash';
	is($node->safe_psql('postgres', 'SELECT pg_postmaster_start_time()'), $pm_start,
		'(B) the postmaster survived: this was a crash-restart, not a restart');

	is(evidence('tb'), '0,0,f', '(B) after the crash-restart the last sweep is from an earlier epoch');
	is(vacuum_sweeps('tb'), '1', '(B) so the next VACUUM sweeps');
	is(evidence('tb'), '0,0,t', '(B) and records this epoch');
	idle_vacuum_does_not_sweep('tb', '(B) after the crash');
}

# ---- (C) ERROR between reclaim_retired's compaction and its frees ----------------
{
	my $t = 'tc';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		INSERT INTO $t SELECT g, (SELECT string_agg('w' || ((g * k) % 4999), ' ')
		                            FROM generate_series(1, 40) k)
		  FROM generate_series(1, 20000) g;
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);
	});
	# The four small segments' headers will be SEGCAT-kind pages carrying a nonzero
	# gen that the CREATE INDEX segment's header is not among.
	my $hdr_q = qq{SELECT string_agg(b::text, ',') FROM generate_series(1,
		bm25_debug_npages('${t}_idx')::int - 1) b
		WHERE (bm25_debug_page_flags('${t}_idx', b) & (4 | 512)) = 4
		  AND bm25_debug_page_seg_gen('${t}_idx', b) > 0};
	my %built = map { $_ => 1 } split /,/, $node->safe_psql('postgres', $hdr_q);
	$node->safe_psql('postgres', qq{
		INSERT INTO $t SELECT g, 'layer one ' || g FROM generate_series(20001, 20050) g;
		SELECT bm25_seal('${t}_idx');
		INSERT INTO $t SELECT g, 'layer two ' || g FROM generate_series(20051, 20100) g;
		SELECT bm25_seal('${t}_idx');
		INSERT INTO $t SELECT g, 'layer three ' || g FROM generate_series(20101, 20150) g;
		SELECT bm25_seal('${t}_idx');
	});
	is(vacuum_sweeps($t), '1', '(C) a new index is swept by its first VACUUM');
	$node->safe_psql('postgres', qq{
		INSERT INTO $t SELECT g, 'layer four ' || g FROM generate_series(20151, 20200) g;
		SELECT bm25_seal('${t}_idx');});

	my %before = map { $_ => 1 } grep { !$built{$_} } split /,/, $node->safe_psql('postgres', $hdr_q);
	is(scalar keys %before, 4, '(C) four small segment headers');
	my $nsegs_q = "SELECT nsegs FROM bm25_stats('${t}_idx')";
	my $nsegs = $node->safe_psql('postgres', $nsegs_q);
	$node->safe_psql('postgres', "SELECT bm25_merge('${t}_idx')");
	is($node->safe_psql('postgres', $nsegs_q), $nsegs - 3,
		'(C) bm25_merge folded the four small segments into one');
	my @retired = grep { $before{$_} } split /,/, $node->safe_psql('postgres', $hdr_q);
	is(scalar @retired, 4, '(C) the merged-away segment headers are retired, unfreed');
	# The merge's own sweep evidence is retired by this VACUUM, which cannot reclaim
	# the ranges yet: their retire_xid is ahead of the horizon.
	is(vacuum_sweeps($t), '1', '(C) the VACUUM after the merge swept (the swap orphaned the old catalog)');
	$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;

	# Opened here, after (A) and (B) have crashed the server twice.
	my $holder = $node->background_psql('postgres');
	$holder->set_query_timer_restart();
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{reclaim_retired_compacted})");
	my $v = $node->background_psql('postgres', on_error_stop => 0);
	$v->set_query_timer_restart();
	$v->query_safe("SET application_name = 'vacc'");
	$v->query_safe("SET bm25_native.debug_pause = 'reclaim_retired_compacted'");
	$v->query_until(qr/started/, "\\echo started\nVACUUM $t;\n");
	$node->poll_query_until('postgres', q{SELECT EXISTS (SELECT 1 FROM pg_locks l
		JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'vacc'
		AND l.locktype = 'advisory' AND NOT l.granted)})
	  or die '(C) VACUUM never reached reclaim_retired_compacted';
	$node->safe_psql('postgres',
		"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = 'vacc'");
	$v->query('SELECT 1');
	like($v->{stderr}, qr/canceling statement due to user request/,
		'(C) VACUUM was cancelled after compacting, before freeing');
	$v->quit;
	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{reclaim_retired_compacted})");
	$holder->quit;

	is($node->safe_psql('postgres', "SELECT bm25_debug_retired_count('${t}_idx')"), '0',
		'(C) the descriptor no longer lists the dropped ranges');
	my @leaked = grep { !(flags($t, $_) & PAGE_DELETED) } @retired;
	cmp_ok(scalar @leaked, '>=', 1, '(C) and their pages were never freed');
	my ($b, $d) = split /,/, evidence($t);
	cmp_ok($b, '>', $d, '(C) the interrupted reclaim left its bracket open');

	is(vacuum_sweeps($t), '1', '(C) the next VACUUM, same server lifetime, swept');
	my @still = grep { !(flags($t, $_) & PAGE_DELETED) } @leaked;
	is(scalar @still, 0, '(C) and freed every leaked range header')
	  or diag("still live-looking: @still");
	idle_vacuum_does_not_sweep($t, '(C)');
	is($node->safe_psql('postgres', qq{SET enable_seqscan = off;
		SELECT count(*) FROM $t WHERE body @@@ 'layer'}), '200',
		'(C) the index still answers');
}

$node->stop;
# Part (B) SIGKILLs one backend on purpose; nothing else may crash. The exemption
# is that one PID. The postmaster names the child by backend type from PG18
# ("client backend (PID n)") but generically through PG17 ("server process (PID
# n)"), so both spellings are accepted -- for that PID only.
die 'part (B) did not record the backend it killed' unless $victim_pid;
bm25_check_logs($node,
	{ allow => qr/(?:client backend|server process) \(PID \Q$victim_pid\E\) was terminated by signal 9: Killed/ });
done_testing();
