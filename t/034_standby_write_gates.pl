# Write entry points refuse on a hot standby (#307 META-07, decision D23; delivers
# #309 CI-04(c4), "bm25_upgrade on a standby").
#
# Every SQL function that opens its index through bm25_index_open_owned writes pages:
# bm25_seal, bm25_merge, bm25_upgrade and the mutating bm25_debug_* levers. Core
# refuses INSERT, VACUUM and REINDEX during recovery on its own, so these were the
# only way to reach bm25's write path on a standby, and the gate did not check. They
# ran until XLogBeginInsert failed with XX000 internal_error -- after
# bm25_debug_alloc_unknown_page had already extended the standby's relation file --
# and bm25_upgrade alone carried a private check with a different SQLSTATE (55000).
# The gate now calls core's PreventCommandDuringRecovery, so every one of them fails
# with 25006 read_only_sql_transaction and core's "cannot execute ... during recovery"
# wording, before any lock or page read. The read-only surface is unaffected.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

my $primary = PostgreSQL::Test::Cluster->new('gates_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
# SQLSTATE + message, or 'ok'. Created on the primary so the standby inherits it;
# a PL/pgSQL exception block is a subtransaction, which a standby permits.
$primary->safe_psql('postgres', q{
	CREATE FUNCTION try_sqlstate(q text) RETURNS text LANGUAGE plpgsql AS $$
	BEGIN
		EXECUTE q;
		RETURN 'ok';
	EXCEPTION WHEN OTHERS THEN
		RETURN SQLSTATE || ' ' || SQLERRM;
	END $$});
$primary->safe_psql('postgres', 'CREATE TABLE docs(id int primary key, body text)');
$primary->safe_psql('postgres',
	q{INSERT INTO docs SELECT g, 'alpha beta ' || g FROM generate_series(1, 200) g});
$primary->safe_psql('postgres',
	'CREATE INDEX docs_bm25 ON docs USING bm25_native (body)');
$primary->safe_psql('postgres', q{SELECT bm25_seal('docs_bm25')});
# Leave documents in the pending list too, so a seal on the standby has real work
# to attempt rather than returning early.
$primary->safe_psql('postgres',
	q{INSERT INTO docs SELECT g, 'gamma delta ' || g FROM generate_series(201, 260) g});

$primary->backup('gates_backup');
my $standby = PostgreSQL::Test::Cluster->new('gates_standby');
$standby->init_from_backup($primary, 'gates_backup', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'standby is in recovery');
my $pending = $standby->safe_psql('postgres',
	q{SELECT pending_ndocs FROM bm25_stats('docs_bm25')});
ok($pending > 0, "standby replayed a non-empty pending list ($pending docs)");

my $size_before = $standby->safe_psql('postgres',
	q{SELECT pg_relation_size('docs_bm25')});

my $refusal = '25006 cannot execute bm25 index maintenance during recovery';
for my $call (
	q{SELECT bm25_upgrade('docs_bm25')},
	q{SELECT bm25_seal('docs_bm25')},
	q{SELECT bm25_merge('docs_bm25')},
	q{SELECT bm25_debug_alloc_unknown_page('docs_bm25')},
	q{SELECT bm25_debug_stamp_version('docs_bm25', 6, 6, 0)})
{
	my $got = $standby->safe_psql('postgres',
		"SELECT try_sqlstate(\$q\$$call\$q\$)");
	is($got, $refusal, "standby refuses: $call");
}

# Refused before any page was touched: alloc_unknown_page used to extend the
# standby's file before its WAL insert failed.
is($standby->safe_psql('postgres', q{SELECT pg_relation_size('docs_bm25')}),
	$size_before, 'standby index relation size unchanged');

# The gate refuses writes only. The read-only surface and the query path still
# work on the standby, and agree with the primary.
is($standby->safe_psql('postgres',
		q{SELECT ndocs + pending_ndocs FROM bm25_stats('docs_bm25')}),
	'260', 'bm25_stats still answers on the standby');
my $q = q{SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'gamma'};
is($standby->safe_psql('postgres', $q), $primary->safe_psql('postgres', $q),
	'standby @@@ count matches the primary');

# And the same functions still work on the primary: the check is recovery, not
# the function.
is($primary->safe_psql('postgres', q{SELECT try_sqlstate('SELECT bm25_seal(''docs_bm25'')')}),
	'ok', 'bm25_seal still runs on the primary');

$standby->stop;
$primary->stop;
bm25_check_logs($primary, $standby);
done_testing();
