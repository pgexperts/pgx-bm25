# Shared end-of-test server-log scan for the TAP suites (#309 CI-10).
#
# Role in the system. Most t/*.pl suites assert query answers after a crash,
# restart or replay. That leaves a whole class invisible: a backend that hits an
# Assert (TRAP) or a PANIC and is recovered by the postmaster, a standby that logs
# an invalid-page reference or stops on a wal_consistency_checking mismatch, can all
# leave the final answer intact. The evidence is only in each node's server log, so
# every suite ends by calling bm25_check_logs on every node it created.
#
# Usage, after the suite's last query (stopping the nodes first is better, so a
# shutdown-time failure is in the log too):
#
#   use FindBin;
#   use lib "$FindBin::RealBin/lib";
#   use Bm25LogCheck;
#   ...
#   bm25_check_logs($primary, $standby);
#   bm25_check_logs($node, { allow => qr/\(PID \Q$pid\E\) was terminated by signal 9/ });
#
# THIS RUN ONLY. A node's log is named by test and node, and PostgreSQL::Test
# never truncates it: under `make installcheck` it lives in a fresh tmp_check/log,
# but a direct `prove` writes ./log and every run appends to the same file. A
# whole-file scan would then keep failing on a line from an earlier failed run, and
# the start canary below could pass on an earlier run's start. So loading this
# module wraps PostgreSQL::Test::Cluster::new to record each node's log size at
# creation, and only what was written after that is scanned. That is why the `use`
# must come before the suite creates its first node (every suite puts it with its
# other `use` lines). A node whose logfile was rotated is scanned from the start of
# its current file.
#
# One ok() per node. `allow` exempts lines the suite causes on purpose (t/029
# SIGKILLs a backend); keep it as narrow as the deliberate event, and match every
# supported server's wording of it -- the postmaster calls a crashed backend
# "server process" through PG17 and "client backend" from PG18. The marker list
# is deliberately short and unambiguous: a pattern that also matched routine output
# would train people to add allow-lists until the check meant nothing.
package Bm25LogCheck;

use strict;
use warnings;
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(bm25_check_logs);

# TRAP: an Assert failure (cassert builds). PANIC: anything that forces a crash
# restart. "terminated by signal": the postmaster's report of a crashed child.
# "invalid page": a standby's or crash recovery's reference to a page that does
# not exist or is uninitialized. "inconsistent page found": wal_consistency_checking.
# "runtime error:": a UBSan report. The hardening job runs with halt_on_error=1,
# which ends the backend with exit code 1, not a signal, so the postmaster's crash
# report alone does not name it.
our $MARKERS = qr/TRAP:|PANIC:|terminated by signal|invalid page|inconsistent page found|runtime error:/;

# Record where each node's log stood when the node was created (see THIS RUN ONLY).
{
	no warnings 'redefine';
	my $orig_new = \&PostgreSQL::Test::Cluster::new;
	*PostgreSQL::Test::Cluster::new = sub {
		my $node = $orig_new->(@_);
		my $log = $node->logfile;
		$node->{_bm25_log_start} = [ $log, (-e $log ? -s $log : 0) ];
		return $node;
	};
}

sub bm25_check_logs
{
	my $opts = (@_ && ref $_[-1] eq 'HASH') ? pop @_ : {};
	my @nodes = @_;

	die "bm25_check_logs: no nodes given" unless @nodes;
	foreach my $node (@nodes)
	{
		die 'bm25_check_logs: node ' . $node->name
		  . ' was created before Bm25LogCheck was loaded'
		  unless $node->{_bm25_log_start};
		my ($file, $offset) = @{ $node->{_bm25_log_start} };
		$offset = 0 if $file ne $node->logfile;
		my $log = slurp_file($node->logfile, $offset);
		my @hits = grep {
			/$MARKERS/ && !(defined $opts->{allow} && /$opts->{allow}/)
		} split /\n/, $log;

		# Canary: every node these suites create was started at least once in this
		# run, and a started server logs this line. Without it, a log path that
		# moved or a node whose output went elsewhere would scan as clean.
		if ($log !~ /database system is ready to accept/)
		{
			fail('server log of node ' . $node->name . ' is readable and shows a start');
			next;
		}

		ok(!@hits, 'server log of node ' . $node->name
			  . ' has no TRAP, PANIC, crashed process or bad page')
		  or diag(join("\n", @hits > 10 ? @hits[0 .. 9] : @hits));
	}
	return;
}

1;
