-- 148_keymap_cancel_analyze_memory -- issue #305, three bounded-cost items.
--
-- (1) XCUT-05: bm25_keymap_write held a buffer content lock at every instant of the
-- KEYMAP chain, so interrupts were held from its first page to its last and a
-- cancel arriving mid-chain waited for the whole chain. Each page boundary now
-- unlocks the finished tail before a CHECK_FOR_INTERRUPTS (ADR 0041's chain_ensure
-- remedy). Shown with bm25_native.debug_cancel_at, which marks the query cancelled
-- at the first KEYMAP page boundary exactly as a pg_cancel_backend arriving then
-- would, and with the work counter, which counts KEYMAP pages written: the cancelled
-- build must stop one page after the injection, not after the chain. Before the fix
-- the cancel was serviced only after the last KEYMAP page, so the cancelled build
-- reported the same work as an uncancelled one.
--
-- WHY A CANCEL INJECTED IN-SESSION. A timeout cannot be aimed at the KEYMAP chain
-- (it is a few milliseconds of a CREATE INDEX whose heap scan checks interrupts per
-- tuple, sql/80's negative result (1)), and a debug_pause park needs a second
-- session to cancel it -- and a cancel that lands while the backend is parked is
-- serviced by the lock wait, not by the code under test.
--
-- (2) TEXT-03(a): bm25_analyze copied every kept lexeme and never freed the
-- dictionary's result array, so about half of what a long document's analysis held
-- live was garbage. Pinned as the analysis's bytes per word, read from
-- pg_backend_memory_contexts while the analysis's per-tuple memory is still live
-- (the subquery's row has not been released when the outer target list runs the
-- scalar subquery). Measured as a SLOPE between two input sizes so the fixed costs
-- of the session drop out. Measured 80 bytes per word after the fix and 147
-- before (PG 18, english stemming of a 5-letter word); the bound sits between.
--
-- (3) SCAN-08 has no case here. The @@@ union collector's new ceiling is 2^25
-- term-document entries, so reaching it needs a 512 MB array and a budget above
-- 1.5 GB in one backend, which is not a regression-suite fixture. It was checked by
-- hand with 34,000 one-word rows and a query repeating that word 1,000 times (34M
-- entries) under max_match_memory = 4GB: before the fix it failed with XX000
-- "invalid memory alloc request size", after it with 54000 PROGRAM_LIMIT_EXCEEDED.
SET jit = off;
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------------ the lever's surface
-- Same names and check hook as debug_pause: an unknown point is an error rather
-- than a cancel that silently never fires.
SET bm25_native.debug_cancel_at = 'keymap_rotaton';
SET bm25_native.debug_cancel_at = 'keymap_rotation';
RESET bm25_native.debug_cancel_at;
-- PGC_SUSET: a lever that cancels maintenance midway belongs to whoever may park it.
CREATE ROLE regress_bm25_148;
SET ROLE regress_bm25_148;
SET bm25_native.debug_cancel_at = 'keymap_rotation';
RESET ROLE;
DROP ROLE regress_bm25_148;

-- ------------------------------------------------- (1) KEYMAP chain cancellation
-- 100,000 int8 keys are about 100 KEYMAP pages. The other chains are identical with
-- and without a key, so a keyless build's work is the keyed build's work minus its
-- KEYMAP pages, and each comparison below is between two builds of this corpus.
CREATE TABLE kc (id int8, body text) WITH (autovacuum_enabled = off);
INSERT INTO kc SELECT g, 'w' || (g % 7) FROM generate_series(1, 100000) g;

-- Builds the index, reports the work, and rolls the build back so the next call
-- starts from the same empty state. set_config(..., true) is local to the inner
-- block's subtransaction, so the lever does not outlive the build it aims at.
CREATE FUNCTION kc_build(keyed bool, cancel_at text,
                         OUT units bigint, OUT cancelled bool) AS $$
BEGIN
    PERFORM bm25_debug_work_reset();
    cancelled := false;
    BEGIN
        PERFORM set_config('bm25_native.debug_cancel_at', cancel_at, true);
        IF keyed THEN
            CREATE INDEX kc_idx ON kc USING bm25_native (body) INCLUDE (id)
                WITH (key_field = 'id');
        ELSE
            CREATE INDEX kc_idx ON kc USING bm25_native (body);
        END IF;
        units := bm25_debug_work_units();
        RAISE EXCEPTION 'roll back';
    EXCEPTION
        WHEN query_canceled THEN
            units := bm25_debug_work_units();
            cancelled := true;
        WHEN raise_exception THEN
            NULL;
    END;
END
$$ LANGUAGE plpgsql;

SELECT units AS keyless, cancelled AS keyless_cancelled FROM kc_build(false, '') \gset
SELECT units AS keyed, cancelled AS keyed_cancelled FROM kc_build(true, '') \gset
SELECT units AS cut, cancelled AS cut_cancelled FROM kc_build(true, 'keymap_rotation') \gset

-- Non-vacuity: the uncancelled builds completed, the KEYMAP chain is long enough
-- for "stopped early" to mean something, and the injected cancel was taken.
SELECT :'keyless_cancelled' AS keyless_cancelled, :'keyed_cancelled' AS keyed_cancelled,
       :keyed - :keyless > 50 AS keymap_spans_many_pages,
       :'cut_cancelled' AS cut_cancelled;
-- The assertion: the cancel injected at the first KEYMAP page boundary is serviced
-- there, after one page. Before the fix this printed the whole chain (about 100).
SELECT :cut - :keyless AS keymap_pages_before_cancel;

-- The restructured writer still writes the right chain: every document's key, once,
-- across all of its pages.
CREATE INDEX kc_idx ON kc USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
SELECT count(*) AS ndocs, count(DISTINCT key_int8) AS distinct_keys,
       min(key_int8) AS min_key, max(key_int8) AS max_key
  FROM bm25_debug_seg_keymap('kc_idx', 0);
SET enable_seqscan = off;
SELECT id FROM kc WHERE body @@@ 'w3' ORDER BY id LIMIT 3;
RESET enable_seqscan;

-- ------------------------------------------------- (2) analyze bytes per word
CREATE FUNCTION analyze_bytes(nwords int) RETURNS numeric AS $$
DECLARE
    b numeric;
BEGIN
    SELECT (SELECT sum(total_bytes) FROM pg_backend_memory_contexts)
      INTO b
      FROM (SELECT bm25_debug_analyze_positions(repeat('abcde ', nwords),
                                                'standard', 'none', 'english') AS p
            OFFSET 0) s
     WHERE array_length(p, 1) = nwords;
    IF b IS NULL THEN
        RAISE EXCEPTION 'analysis did not produce one token per word';
    END IF;
    RETURN b;
END
$$ LANGUAGE plpgsql;

SELECT (analyze_bytes(2000000) - analyze_bytes(1000000)) / 1000000 < 100
       AS analysis_holds_under_100_bytes_per_word;

-- pg_regress shares ONE database across suites; leave no extension behind.
DROP FUNCTION kc_build(bool, text), analyze_bytes(int);
DROP TABLE kc;
DROP EXTENSION bm25_native;
