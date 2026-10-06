use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# A pending append that needs a fresh page writes ONE Generic WAL record (issue #300).
#
# It used to write three: init the new page, link the old tail to it, then the
# data + metapage record. A crash after the first left an initialised PENDING page
# that nothing linked -- an orphan only the VACUUM orphan sweep ever freed. That
# sweep is now gated on durable evidence that orphans can exist, and an appender
# writes none, so an append must never be able to leave such a page.
#
# The suite parks an INSERT right after bm25_page_alloc handed it a page
# (pause point pending_append_alloc), flushes everything already in WAL with
# pg_switch_wal() (an INSERT's records are not flushed until it commits), and
# crashes the server. After recovery no page may be an initialised, non-DELETED
# PENDING page outside the chain. Against the three-record shape -- with the pause
# point moved to just after the init record, the matching point in that sequence --
# the page the init record wrote survives recovery unlinked, and the check fails.
#
# The parked backend holds three content locks (metapage, old tail, new page), so it
# cannot be cancelled and it stalls any checkpoint; only the immediate stop ends it.

use constant {
	PAGE_PENDING => 1 << 1,
	PAGE_DELETED => 1 << 9,
};

my $node = PostgreSQL::Test::Cluster->new('append_one_record_crash');
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
INSERT INTO d VALUES (1, 'anchor document already on the chain');
CHECKPOINT;
});

my $pause_key = 1651323445;
my $point = 10;    # pending_append_alloc

my $holder = $node->background_psql('postgres');
$holder->query_safe("SELECT pg_advisory_lock($pause_key, $point)");

my $ins = $node->background_psql('postgres', on_error_stop => 0);
$ins->query_safe("SET application_name = 'parked_insert'");
$ins->query_safe("SET bm25_native.debug_pause = 'pending_append_alloc'");
# ~600 distinct terms: far more than the tail page has left, so the first part
# needs a fresh page linked from the old tail.
$ins->query_until(qr/started/, q{\echo started
INSERT INTO d SELECT 2, string_agg('term' || k, ' ') FROM generate_series(1, 600) k;
});

$node->poll_query_until('postgres', q{
SELECT EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
                WHERE a.application_name = 'parked_insert'
                  AND l.locktype = 'advisory' AND NOT l.granted)})
  or die 'the INSERT never reached pending_append_alloc';

$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
eval { $ins->quit };
eval { $holder->quit };
$node->start;

my $npages = $node->safe_psql('postgres', q{SELECT bm25_debug_npages('d_bm25')});
my %flags = map { split /:/ } split /,/, $node->safe_psql('postgres', q{
SELECT string_agg(b || ':' || bm25_debug_page_flags('d_bm25', b), ',')
  FROM generate_series(1, bm25_debug_npages('d_bm25')::int - 1) b});
my %chain = map { $_ => 1 } grep { length } split /,/, $node->safe_psql('postgres', q{
SELECT string_agg(p::text, ',')
  FROM generate_series(0, 1000) n, LATERAL bm25_debug_pending_nth_page('d_bm25', n) p
 WHERE p IS NOT NULL});

my @stray = sort { $a <=> $b } grep {
	($flags{$_} & PAGE_PENDING) && !($flags{$_} & PAGE_DELETED) && !$chain{$_}
} keys %flags;

cmp_ok(scalar keys %chain, '>=', 1, 'the pre-crash chain survived recovery');
is(scalar @stray, 0,
	'no initialised PENDING page is left outside the chain after a crash mid-append')
  or diag("npages $npages; unlinked PENDING pages: @stray; chain: "
	  . join(',', sort { $a <=> $b } keys %chain));
is($node->safe_psql('postgres', q{
SET enable_seqscan = off;
SELECT count(*) FROM d WHERE body @@@ 'anchor'}), '1',
	'the committed document is still found through the index');

$node->stop;
bm25_check_logs($node);
done_testing();
