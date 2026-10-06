use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use lib "$FindBin::RealBin/lib";
use Bm25LogCheck;

# Concurrent INSERT vs. seal (review ref C9).
#
# bm25_pending_append_multi serialized only on the metapage BUFFER content lock.
# LockPage (the lmgr heavyweight LOCKTAG_PAGE seal singleton) and LockBuffer are
# different lock managers and do not conflict, so the singleton the sealer holds
# across drain+build+commit did not exclude an appender at all -- and the
# production aminsert path appends BEFORE it even tries ConditionalLockPage.
#
# So the invariant the publish record relies on ("no append can land between the
# drain snapshot and the publish") was false, and bm25_segment_build_and_commit
# resets pending_head/tail/npages/ndocs unconditionally: a document appended
# during a seal was published by nobody, then had its page recycled by
# bm25_pending_truncate. Committed, heap-visible, and findable through the index
# only after a REINDEX.
#
# Shape of the reproduction: appends always land on the TAIL page, and the drain
# walks head -> tail, so it reaches the tail LAST. The window that matters is
# therefore not the drain itself but the stretch AFTER the drain has read the tail
# and BEFORE the publish commits -- i.e. the segment BUILD, which for a large
# pending list takes seconds. Hence: build a big pending list, fire the seal
# asynchronously, and stream inserts through the whole operation.
#
# Sizing is a compromise with CI cost: the 40000-row bulk load is ONE statement and
# cheap even under the cassert+UBSan job, while the racer loop is one statement per
# row and is kept to 2000. A narrower window makes the PRE-fix reproduction less
# certain, never the post-fix assertions -- with the fix every count below is exact
# however the two sessions interleave, so this cannot false-fail.
#
# Verified against the pre-fix build by hand with two psql sessions: it does not
# merely lose rows, it crashed the backend (reproduced twice). With the fix the
# same workload is clean.

my $node = PostgreSQL::Test::Cluster->new('pending_append_race');
$node->init;
$node->start;

# Every writing session raises seal_threshold (KB; range 64..INT_MAX) so the
# opportunistic aminsert seal never fires and the ONLY seal is the one this test
# starts explicitly. Set per session rather than in postgresql.conf so the test does
# not also depend on custom-GUC placeholder handling.
my $no_autoseal = q{SET bm25_native.seal_threshold = 4000000;};

$node->safe_psql('postgres', 'CREATE EXTENSION bm25_native');
$node->safe_psql('postgres', q{
	CREATE TABLE s (id serial primary key, body text);
	CREATE INDEX s_bm25 ON s USING bm25_native (body);
});

# Build a large pending list: no seal has run, so all of this is on the chain.
$node->safe_psql('postgres', $no_autoseal . q{
	INSERT INTO s (body)
	SELECT 'common bulk tok' || g FROM generate_series(1, 40000) g
});

my $bulk = $node->safe_psql('postgres', 'SELECT count(*) FROM s');
is($bulk, 40000, 'pending list built with 40000 documents');

# Fire the seal asynchronously (query_until with an empty pattern returns as soon
# as the statement is sent), then insert straight through its drain and build.
my $h = $node->background_psql('postgres');
$h->query_until(qr//, qq{SELECT bm25_seal('s_bm25');\n});

$node->safe_psql('postgres', $no_autoseal . q{
	DO $$
	BEGIN
	  FOR i IN 1..2000 LOOP
	    INSERT INTO s (body) VALUES ('common racer tok' || i);
	  END LOOP;
	END $$;
});

$h->quit;

# The whole point: a committed, heap-visible row must be findable through the
# index. Compare against the heap rather than a constant so the assertion holds
# however the seal and the inserts interleaved.
my $heap_racer = $node->safe_psql('postgres',
	q{SELECT count(*) FROM s WHERE body LIKE '%racer%'});
my $index_racer = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SELECT count(*) FROM s WHERE body @@@ 'racer'});
is($index_racer, $heap_racer,
	'every row inserted during the seal is findable through the index');

my $heap_all = $node->safe_psql('postgres', 'SELECT count(*) FROM s');
my $index_all = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SELECT count(*) FROM s WHERE body @@@ 'common'});
is($index_all, $heap_all, 'no document lost across the concurrent seal');
is($heap_all, 42000, 'both writers committed everything they inserted');

# A second seal must not resurrect or duplicate anything: BM25 scores are additive,
# so a doc drained twice would be double-counted as well as double-returned.
$node->safe_psql('postgres', q{SELECT bm25_seal('s_bm25')});
my $after = $node->safe_psql('postgres',
	q{SET enable_seqscan=off; SELECT count(*) FROM s WHERE body @@@ 'common'});
is($after, $heap_all, 'a follow-up seal neither loses nor duplicates documents');

$node->stop;
bm25_check_logs($node);
done_testing();
