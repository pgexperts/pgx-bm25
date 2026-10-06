use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# The share-mode orphan sweep against concurrent appenders (issue #300).
#
# The sweep marks what is reachable, then stamps every unreachable page DELETED and
# records it free. It now holds the seal/merge singleton in ShareLock, so appenders
# run beside it -- and an appender can pop a page that was DELETED and unreachable
# when the sweep marked, re-init it as a pending page and link it into the chain. If
# the sweep then stamped that page, it would free a live page: the next allocation
# would hand it out again and two structures would share it. The sweep's per-page
# rule skips an unreachable PENDING page whose chain epoch (#291) is 0 or at least
# e_floor = min(next_gen, head epoch, tail epoch) read under the singleton.
#
#   (A) The chain exists when the sweep marks: the VACUUM is parked after its seal
#       (merge_start), documents start a chain, and once the sweep has marked
#       (orphan_sweep_marked) more documents grow that chain onto reused pages.
#   (B) The chain is empty when the sweep marks; documents appended after the mark
#       start a chain on reused pages (epoch drawn at or above the captured next_gen).
#   (C) An older binary's page in the chain AFTER the mark: the tail's epoch is set to
#       0 at the second pause (bm25_debug_set_pending_tail_epoch), so every page
#       appended after it copies 0 -- below e_floor, and only the "epoch 0" clause
#       keeps the sweep off them.
#   (D) The same 0 tail BEFORE the mark: e_floor takes the tail's 0 and skips every
#       pending page.
#
# Each scenario checks that the post-mark appends really landed on pages inside the
# sweep's range (reused, not extended), or it would be passing without the race.
# After the VACUUM no chain page may be DELETED and every document must be found;
# then another load, seal and VACUUM must find them all too (no double allocation).
# Without the per-page rule (A) and (B) stamp live chain pages; without the epoch-0
# clause (C) does. Against an ExclusiveLock sweep every post-mark INSERT times out.

my $node = PostgreSQL::Test::Cluster->new('sweep_appender_race');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');

use constant PAGE_DELETED => 1 << 9;

my $pause_key = 1651323445;
my %point = (merge_start => 9, orphan_sweep_marked => 11);

sub burn
{
	$node->safe_psql('postgres', 'SELECT txid_current()') for 1 .. 5;
}

sub chain
{
	my ($t) = @_;
	return grep { length } split /,/, $node->safe_psql('postgres', qq{
		SELECT string_agg(p::text, ',' ORDER BY n)
		  FROM generate_series(0, 5000) n,
		       LATERAL bm25_debug_pending_nth_page('${t}_idx', n) p
		 WHERE p IS NOT NULL});
}

# $n documents of ~20 distinct terms, ids from $from.
sub load
{
	my ($t, $from, $n, $word) = @_;
	my $sql = qq{SET lock_timeout = '2s';
		INSERT INTO $t SELECT g, '$word ' || (SELECT string_agg('$word' || g || 'x' || k, ' ')
		                                     FROM generate_series(1, 20) k)
		  FROM generate_series($from, @{[ $from + $n - 1 ]}) g;};
	my ($rc, $out, $err) = $node->psql('postgres', $sql);
	return ($rc, $err);
}

sub scenario
{
	my ($label, $t, %opt) = @_;

	# Free pages whose horizon has cleared: a chain sealed away (its truncate stamps
	# every page DELETED with a horizon), then xids burned past that horizon.
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, body text) WITH (autovacuum_enabled = off);
		CREATE INDEX ${t}_idx ON $t USING bm25_native (body);});
	load($t, 1, 600, 'seed');
	$node->safe_psql('postgres', "SELECT bm25_seal('${t}_idx')");
	burn();

	my $holder = $node->background_psql('postgres');
	my @pauses = $opt{before_mark} ? qw(merge_start orphan_sweep_marked) : qw(orphan_sweep_marked);
	$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point{$_})") for @pauses;
	my $v = $node->background_psql('postgres', on_error_stop => 0);
	$v->query_safe("SET application_name = 'vac_$t'");
	$v->query_safe("SET bm25_native.debug_pause = '" . join(',', @pauses) . "'");
	# The new index has never been swept, so this VACUUM sweeps.
	$v->query_until(qr/started/, "\\echo started\nVACUUM $t;\n");

	my $parked = qq{SELECT EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE a.application_name = 'vac_$t' AND l.locktype = 'advisory' AND NOT l.granted
		  AND l.objid = %d)};
	my $docs = 0;
	if ($opt{before_mark})
	{
		$node->poll_query_until('postgres', sprintf($parked, $point{merge_start}))
		  or die "$label: VACUUM never reached merge_start";
		my ($rc, $err) = load($t, 10001, 60, 'zfirst');
		is($rc, 0, "$label: documents appended after the seal, before the sweep") or diag($err);
		$docs += 60;
		if ($opt{zero_tail_before})
		{
			isnt($node->safe_psql('postgres',
				"SET lock_timeout = '2s'; SELECT bm25_debug_set_pending_tail_epoch('${t}_idx', 0)"), '',
				"$label: the tail now carries epoch 0, before the mark");
		}
		$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{merge_start})");
	}

	$node->poll_query_until('postgres', sprintf($parked, $point{orphan_sweep_marked}))
	  or die "$label: VACUUM never reached orphan_sweep_marked";
	my $nblocks = $node->safe_psql('postgres', "SELECT bm25_debug_npages('${t}_idx')");
	my %marked = map { $_ => 1 } chain($t);
	if ($opt{zero_tail_after})
	{
		isnt($node->safe_psql('postgres',
			"SET lock_timeout = '2s'; SELECT bm25_debug_set_pending_tail_epoch('${t}_idx', 0)"), '',
			"$label: the tail now carries epoch 0, after the mark");
	}
	my ($rc, $err) = load($t, 20001, 200, 'zsecond');
	is($rc, 0, "$label: documents appended while the sweep holds the singleton") or diag($err);
	$docs += 200;
	my @appended = grep { !$marked{$_} } chain($t);
	my @reused = grep { $_ < $nblocks } @appended;
	cmp_ok(scalar @reused, '>', 0,
		"$label: post-mark appends reused pages inside the sweep's range ("
		  . scalar(@reused) . ' of ' . scalar(@appended) . ')');

	$holder->query_safe("SELECT pg_advisory_unlock($pause_key, $point{orphan_sweep_marked})");
	$v->query('SELECT 1');
	is($v->{stderr}, '', "$label: the VACUUM finished");
	$v->quit;
	$holder->quit;

	my @stamped = grep {
		$node->safe_psql('postgres', "SELECT bm25_debug_page_flags('${t}_idx', $_)") & PAGE_DELETED
	} chain($t);
	is(scalar @stamped, 0, "$label: the sweep stamped no live chain page")
	  or diag("DELETED chain pages: @stamped");
	my $found = sub {
		$node->safe_psql('postgres', qq{SET enable_seqscan = off;
			SELECT (SELECT count(*) FROM $t WHERE body @@@ 'zfirst')
			     + (SELECT count(*) FROM $t WHERE body @@@ 'zsecond')});
	};
	is($found->(), $docs, "$label: every appended document is found");

	# Reuse everything again: a seal, more documents, a VACUUM. A page the sweep
	# had freed while live would now be handed out twice.
	$node->safe_psql('postgres', "SELECT bm25_seal('${t}_idx')");
	burn();
	load($t, 30001, 200, 'zsecond');
	$docs += 200;
	$node->safe_psql('postgres', "VACUUM $t");
	is($found->(), $docs, "$label: and still found after another load, seal and VACUUM");
}

scenario('(A) existing chain', 'ra', before_mark => 1);
scenario('(B) new chain', 'rb');
scenario('(C) epoch-0 tail after the mark', 'rc', before_mark => 1, zero_tail_after => 1);
scenario('(D) epoch-0 tail before the mark', 'rd', before_mark => 1, zero_tail_before => 1);

$node->stop;
bm25_check_logs($node);
done_testing();
