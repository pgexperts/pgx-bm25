\echo Use "CREATE EXTENSION bm25_native" to load this file. \quit

/* bm25_native 1.0: consolidated schema script.
 * Folds the prior extension's 0.1 -> 0.2 -> 0.3 upgrade lineage into a
 * single CREATE EXTENSION script under the bm25_native identity; the
 * resulting catalog state is unchanged from 0.3 (see
 * docs/adr/0013-bm25-native-rebrand.md for the rebrand rationale and the
 * prior identity, and git history for the individual upgrade steps). */

CREATE FUNCTION bm25_handler(internal) RETURNS index_am_handler
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE ACCESS METHOD bm25_native TYPE INDEX HANDLER bm25_handler;

-- Match operator: does the document match the query term(s)?
-- STABLE (not IMMUTABLE): bm25_match now tokenizes via bm25_analyze, which calls
-- ts_lexize over a Snowball dict resolved from the system catalog -- a catalog/dict-
-- cache lookup, so the result depends on database state, not just the arguments.
--
-- ANALYZER CAVEAT (#151 SQL-12). When this runs OFF the index -- as a Filter rather
-- than an Index Cond -- it evaluates with the english/default analyzer regardless of
-- the index's reloptions, because it receives two bare text values and cannot discover
-- which index (if any) covers its left argument. On a non-english index the filter path
-- can therefore disagree with the index path about the same row. See the C function for
-- why this is structural rather than unfixed, and sql/99_query_semantics, which pins the
-- divergence so it stays a tested property.
/* derivation: re-measured on the Task-2 5,183-row corpus (Apple M3 Max,
 * PG18, fully cached), enable_indexscan/enable_bitmapscan off to force the
 * seqscan path:
 *   filtered: EXPLAIN (ANALYZE, TIMING OFF)
 *     SELECT count(*) FROM docs5k WHERE body @@@ 'negligence liability';
 *     Execution Time ~10.47ms (median of 5 runs: 10.822/10.418/10.467/
 *     10.398/10.470)
 *   bare:     EXPLAIN (ANALYZE, TIMING OFF) SELECT count(*) FROM docs5k;
 *     Execution Time ~0.198ms (median of 5 runs: 0.207/0.199/0.197/
 *     0.194/0.198)
 * us/eval = (10467 - 198) / 5183 = 1.98 us -- the marginal cost bm25_match
 * adds per row, on THIS corpus's short synthetic bodies (a few words each,
 * far shorter than the ~150-250 word abstracts the original investigation
 * calibrated against -- shorter documents tokenize faster, so this floor
 * is corpus-dependent by construction, not a contradiction).
 * Convert us to cost units via the bare scan's own modeled-vs-actual
 * ratio (its Seq Scan node: cost=0.00..97.83 for 5183 rows, actual
 * 198us): 198us / 97.83 units = 2.02 us/unit.  1.98us / 2.02(us/unit) =
 * 0.98 cost units added per eval; /cpu_operator_cost(0.0025) = procost
 * truth-equivalent ~= 392 on this corpus.
 * That floor sits inside the margin the spec already establishes: the
 * original investigation's SciFact-abstract calibration puts the honest
 * procost at ~5.6e4 (a lower bound on the error, not a target), and its
 * own sweep found procost >= 100 already fixes every mis-planned cell.
 * 5000 -- within the mandated [1e3,1e4] bound -- lands close to the
 * geometric mean of this corpus's ~392 floor and the investigation's
 * 5.6e4 ceiling (sqrt(392 * 56000) ~= 4685), giving roughly an order of
 * magnitude of margin in both directions: safely above what even a
 * cheap-to-tokenize corpus demands, safely below the real-document
 * worst case. Frozen at 5000 (confirms Task 1's provisional value). */
CREATE FUNCTION bm25_match(text, text) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE COST 5000;
-- STABLE, matching how core declares its own selectivity estimators (eqsel,
-- scalarltsel, ...). Inert in practice -- the planner reaches an oprrest/oprjoin
-- through OidFunctionCall from an operator's catalog entry, never by evaluating a
-- FuncExpr, and the `internal` arguments make both unreachable from SQL -- so this
-- is about not diverging gratuitously from the pattern being copied (#148 SQL-13).
CREATE FUNCTION bm25_matchsel(internal, oid, internal, integer)
  RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION bm25_matchjoinsel(internal, oid, internal, smallint, internal)
  RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
-- custom selectivity: blind-constant estimators for @@@; see docs/adr/0010-planner-estimates.md
CREATE OPERATOR @@@ (LEFTARG = text, RIGHTARG = text,
  PROCEDURE = bm25_match, RESTRICT = bm25_matchsel, JOIN = bm25_matchjoinsel);

-- Operator class: strategy 1 = @@@ (ordering operator added in Task 11)
CREATE OPERATOR CLASS text_bm25_ops DEFAULT FOR TYPE text USING bm25_native AS
  OPERATOR 1 @@@ (text, text);

-- Metapage introspection: returns corpus statistics for the named index.
-- Columns are ordered to match the C implementation (values[0..12]) and must not
-- be reordered: bm25_stats() in src/bm25_meta.c fills a values[13] array
-- POSITIONALLY and heap_form_tuple maps slot i onto the i'th OUT parameter, so
-- reordering here silently transposes columns rather than failing.
-- values[0..7] are the original 8 columns; v4 added analyzer_fingerprint,
-- field_config_blkno, field_count (values[8..10]); v6 added min_read_version,
-- feature_flags (values[11..12]).
CREATE FUNCTION bm25_stats(index regclass,
  OUT ndocs bigint, OUT total_len bigint, OUT format_version int,
  OUT nsegs int, OUT pending_ndocs bigint,
  OUT k1 float8, OUT b float8,
  -- SEALED SEGMENTS ONLY: total_len / ndocs, both of which count sealed,
  -- tombstone-adjusted documents and nothing else.  It is NOT the avgdl the
  -- scorer uses, which also folds in the pending list (see bm25_stats.c's
  -- corpus-stat pass), so the two differ by exactly the un-sealed documents and
  -- agree only right after a seal.  Named for what it is (HDL-13, issue #154):
  -- the column used to be called `avgdl`, which invited reading it as the number
  -- that produced the scores.
  OUT sealed_avgdl float8,
  OUT analyzer_fingerprint bigint, OUT field_config_blkno bigint,
  OUT field_count int,
  OUT min_read_version int, OUT feature_flags bigint)
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Tokenizer debug functions: index-default config (text) + explicit-config
-- overload (text, tokenizer, stopwords, language). STABLE: both perform a
-- catalog lookup to resolve the Snowball dict OID.
-- For development and regression testing only; not part of the query path.
--
-- The overload's second argument is the TOKENIZER, and it was named `analyzer` in
-- the C code until #148 HDL-07 -- a different knob wearing the same name. It is now
-- validated through the very function the `tokenizer` reloption uses
-- (bm25_validate_tokenizer), so "standard" is the only accepted value on BOTH
-- surfaces. It previously accepted any string and discarded it. Note there is no
-- `analyzer` argument in this signature at all, so the reloption's reserved
-- `analyzer` knob has no counterpart here to diverge from.
CREATE FUNCTION bm25_debug_tokenize(text) RETURNS text[]
  AS 'MODULE_PATHNAME', 'bm25_debug_tokenize' LANGUAGE C STABLE STRICT;
CREATE FUNCTION bm25_debug_tokenize(text, text, text, text) RETURNS text[]
  AS 'MODULE_PATHNAME', 'bm25_debug_tokenize_cfg' LANGUAGE C STABLE STRICT;
-- The POSITION of each token, parallel to the array above. Added for #151 (TEXT-05):
-- the compound-splitting path was exercised but only its token COUNTS were asserted, so
-- position advancing per lexeme rather than per run had no assertion anywhere.
CREATE FUNCTION bm25_debug_analyze_positions(text, text, text, text) RETURNS int[]
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

-- Analyzer fingerprint of an index, recomputed from its current reloptions.
-- Matches the scan-time gate's query-side computation; returns bigint (uint32
-- widened to avoid int4 sign confusion). STABLE: opens the index relation.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_analyzer_fingerprint(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

-- Runs the SAME scan-start fingerprint gate with a caller-supplied query fp, so
-- the ERROR (require_analyzer_match=true) / WARNING (false) behavior is testable
-- deterministically without fabricating a second analyzer. Returns true when the
-- supplied fp matches the index's stored fingerprint. For regression testing only.
CREATE FUNCTION bm25_debug_fingerprint_gate(index regclass, claimed_fp bigint)
  RETURNS boolean
  AS 'MODULE_PATHNAME', 'bm25_debug_fingerprint_gate'
  LANGUAGE C STRICT;

/* ROWS on the set-returning probes (#148 SQL-09).
 *
 * Every SRF gets prorows = 1000 and procost = 1 by omission, and the SRFs in
 * this file span six orders of magnitude of real cardinality: bm25_debug_postings
 * returns one row PER POSTING IN THE WHOLE INDEX, bm25_debug_seg_lenfields returns
 * one row per FIELD (at most BM25_MAX_FIELDS). Joining a debug SRF to a real table
 * planned as though both were a 1000-row scan.
 *
 * The estimates below are order-of-magnitude SHAPE markers, not measurements, and
 * they are deliberately coarse -- an SRF's true cardinality is a function of the
 * index it is handed, which prorows cannot see. Four buckets:
 *   ROWS 10      per-field walkers (bounded by BM25_MAX_FIELDS)
 *   ROWS 100     per-segment / per-k walkers
 *   ROWS 10000   per-term and per-document walkers
 *   ROWS 100000  per-posting and per-position walkers
 * plus COST 5000 -- the bm25_match order, see its derivation above -- on the four
 * probes that drive a whole ranking build per call (bm25_debug_rank,
 * bm25_debug_wand_rank, bm25_debug_rank_key, bm25_wand_stats).
 *
 * Blast radius is the regression suite's own plans: all of these are REVOKEd from
 * PUBLIC and none is on the query path. That is why this is cheap rather than
 * urgent -- but a wrong estimate in a test fixture is still a wrong estimate, and
 * these probes are how the suite reasons about the index.
 */

-- Dictionary debug function: returns (term, df) for every entry in the index.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_terms(index regclass,
  OUT term text, OUT df int) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Postings debug function: returns (term, tf, doclen) for every posting.
-- Walks each dict entry's posting page; for development and regression testing
-- only; not part of the query path.
CREATE FUNCTION bm25_debug_postings(index regclass,
  OUT term text, OUT tf int, OUT doclen int) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100000;

-- Field-config debug accessor: one row per configured field from the per-index
-- field-config page (M3: exactly one, the default field, id 0 with empty name).
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_fieldcfg(index regclass,
  OUT field_id int, OUT field_name text,
  OUT k1 float8, OUT b float8, OUT boost float8,
  OUT stemmer_name text) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10;

-- Key-identity stamp accessor (#292): the (key_type, key_size, key_attno) the build
-- resolved for key_field and stamped on the field-config page, which every INSERT is
-- checked against. stamped = false (the rest NULL) for an index built before the
-- stamp existed. key_attno is the 0-based INDEX column; key_column is its name.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_keystamp(index regclass,
  OUT stamped bool, OUT key_type int, OUT key_size int,
  OUT key_attno int, OUT key_column text) RETURNS record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY, mutating: strip the key-identity stamp, leaving the field-config page a
-- pre-#292 binary writes, so the suites can exercise the unstamped fallback. Returns
-- whether a stamp was present. Owner-only (bm25_index_open_owned).
CREATE FUNCTION bm25_debug_clear_keystamp(index regclass) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- BM25 math probe: compute the raw score for a single term given corpus stats.
-- Args: N (corpus size), df, tf, doclen, avgdl, k1, b.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_score(bigint, int, int, int, float8, float8, float8)
  RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Ranking debug function: returns (tid, score) for every document that matches
-- the query, in descending score order.  Calls the exhaustive OR-sum scorer
-- (bm25_scan_build_ranking) directly without a real executor scan so rankings
-- can be verified in regression tests independent of planner/operator wiring.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_rank(index regclass, query text,
  OUT tid tid, OUT score float8) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000 COST 5000;

-- Block-Max WAND ranking probe (M2b Task 8): returns the exact top-k (tid, score)
-- rows produced by the WAND driver (bm25_wand_build_ranking) for a bag-of-words
-- query, into a throwaway scan opaque WITHOUT touching the live scan seam. Must be
-- bit-identical to bm25_debug_rank's top-k prefix (same tids, order, and score
-- bits) -- the equivalence sql/43_wand_parity.sql gates on. Development and
-- regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_wand_rank(index regclass, query text, k int,
  OUT tid tid, OUT score float8) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100 COST 5000;

-- Varbyte (LEB128) roundtrip probe: encode v then decode; returns decoded value
-- and byte count.  For development and regression testing only.
CREATE FUNCTION bm25_debug_varbyte_roundtrip(bigint,
  OUT decoded bigint, OUT nbytes int)
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Decode-boundary probes (review ref H6). The on-disk validators only fire on an
-- already-corrupt page, which a regression suite cannot produce, so these run the
-- SAME validators over caller-supplied bytes. Pure functions of their argument --
-- no relation is opened. For development and regression testing only.
CREATE FUNCTION bm25_debug_varbyte_decode_bytes(bytea) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Header FIELDS, not a raw struct bytea: the latter would make the suite depend
-- on host endianness and padding. Last argument is the bytes the notional page
-- has left from the header onward (pend - cur).
CREATE FUNCTION bm25_debug_block_validate(ndocs int, docid_bytes int, tf_bytes int,
                                          field_rle_bytes int, impact_bytes int,
                                          avail int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_impact_decode_bytes(bytea) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Trust-boundary probes (2026-08): extend the H6 decode-boundary principle to the
-- validators added for the metapage field_count, the KEYMAP key_type/key_size and
-- per-key page-span bounds, the accumulator key_size bound, the merge idmap docid
-- bound, and the retired-list page-kind/entry-count bounds. Same reasoning as the
-- H6 probes above: each on-disk validator only fires on an already-corrupt page,
-- which a regression suite cannot produce, so these run the SAME validators
-- (extracted to their own functions where the check was previously inline) over
-- caller-supplied values. Pure functions of their arguments -- no relation is
-- opened. For development and regression testing only.
CREATE FUNCTION bm25_debug_dictentry_validate(termlen int, avail int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_meta_field_count_validate(field_count int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_keymeta_validate(key_type int, key_size int) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- pagebytes/local_off are bigint so the underflow/overrun boundary cases (which
-- exceed a uint32 in the intermediate arithmetic the C probe performs) can be
-- expressed directly rather than relying on int wraparound in the SQL argument.
CREATE FUNCTION bm25_debug_seg_key_bounds(key_size int, pagebytes bigint,
                                          local_off bigint) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_accum_set_keymeta(key_type int, key_size int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Trust-boundary probes added with the #143 sweep. Each drives a validator that sits
-- in front of a value read off a page and used as a ReadBuffer argument, a page
-- offset, a loop bound or an arithmetic operand.
--
-- blkno is bigint so InvalidBlockNumber (4294967295) is expressible: it is the value
-- that matters, because it is also P_NEW, so an unchecked corrupt block pointer
-- EXTENDS the relation instead of failing the read.
CREATE FUNCTION bm25_debug_seg_blkno_validate(blkno bigint) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_seg_page_off_validate(off int, pagebytes int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- The half of the posting-block contract that can only be checked after the docid
-- deltas are expanded, so it cannot live in bm25_debug_block_validate.
CREATE FUNCTION bm25_debug_block_last_docid_validate(last_docid bigint, run_last bigint)
  RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Returns whether the segment would be chosen for a tombstone-fraction rewrite.
-- Exists to reach the live_ndocs > ndocs case, which needs a corrupt SEGCAT entry.
CREATE FUNCTION bm25_debug_merge_tombstone_trigger(ndocs bigint, live_ndocs bigint)
  RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Merge-replay field_id bound. which: 0 = bm25_accum_add_posting,
-- 1 = bm25_accum_add_positions_to_last. The field RLE decode bounds an id to
-- BM25_MAX_FIELDS while the accumulator's per-doc arrays are field_count wide, so
-- [field_count, BM25_MAX_FIELDS) is the gap these two checks close.
CREATE FUNCTION bm25_debug_accum_field_bound(field_count int, field_id int,
                                             which int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_merge_docid_validate(old_docid bigint, ndocs bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION bm25_debug_retired_page_bounds(flags int, n int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Runs the SAME bound the orphan sweep applies to every segment header block
-- number it takes off the segment catalog or a retired descriptor, before that
-- value indexes the reachable[] bitmap.  bigint arguments so an out-of-uint32
-- value reaches the C guard rather than failing an int4 cast first.
-- The probe always reports the "segment catalog entry" source label; the retired
-- descriptor call site shares this one validator and differs only in that string.
CREATE FUNCTION bm25_debug_segment_blkno_bounds(blkno bigint, nblocks bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- Exercises the SAME strnlen-bound defensive read bm25_debug_fieldcfg now uses for
-- field_name/stemmer_name, against a caller-chosen maxlen (BM25_FIELD_NAME_LEN /
-- BM25_STEMMER_NAME_LEN in the suite) and a buffer with no NUL byte anywhere in it.
CREATE FUNCTION bm25_debug_bounded_name_bytes(bytea, maxlen int) RETURNS text
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Trust-boundary probes (2026-08 page-content-bytes sweep): bm25_page_content_bytes
-- is the decode boundary for a page's content byte count (pd_lower minus the page
-- header), now the single read-side derivation for every page-content length in
-- the tree; bm25_chain_span_validate is chain_read_at's per-record memcpy span
-- check. Same reasoning as the trust-boundary probes above: each only fires on an
-- already-corrupt page, which a regression suite cannot produce, so these run the
-- SAME functions over caller-supplied values. Pure functions of their arguments --
-- no relation is opened. For development and regression testing only.
CREATE FUNCTION bm25_debug_page_content_bytes(pd_lower int, pd_upper int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- local_off/len/pagebytes are bigint for the same reason bm25_debug_seg_key_bounds'
-- pagebytes/local_off are: the boundary cases can exceed a uint32 in the
-- intermediate arithmetic the C probe performs.
CREATE FUNCTION bm25_debug_chain_span_validate(local_off bigint, len bigint,
                                               pagebytes bigint) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- bm25_pending_term_entry_span bounds one on-page pending term entry's header +
-- MAXALIGN'd (termlen + pos_bytes) span; shared by the pending iterator and the
-- drain's per-part re-decode. termlen/pos_bytes fill a real struct on the C stack
-- (see bm25_debug_dictentry_validate above), `avail` stands in for (end - cur).
CREATE FUNCTION bm25_debug_pending_term_entry_span(termlen int, pos_bytes int,
                                                    avail int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- bm25_pending_tail_space_check (#136) is the same kind of boundary one level up: the
-- pending append used to budget each record against the metapage's CACHED
-- pending_tail_free and never against the tail page it had just locked, so a counter
-- that overstated the page turned the append's memcpy into a write past pd_upper and
-- past the end of the Generic WAL scratch image. No bm25_debug_* function can falsify
-- that counter (none writes the metapage's pending fields), so the check is driven here
-- over a caller-supplied header instead. runneed/claimed_free are bigint so the
-- out-of-range arguments can be expressed at the SQL boundary; returns the free byte
-- count the check computed.
CREATE FUNCTION bm25_debug_pending_tail_space(pd_lower int, pd_upper int,
                                              runneed bigint, claimed_free bigint)
  RETURNS bigint AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Page-kind probes (2026-08 H6 sweep). bm25_page_init stamps a BM25_PAGE_* kind
-- on every page and, until this fix, NO reader on the segment or pending paths
-- tested it: a stray or corrupt nextblk, or a recycled page, was decoded as
-- whatever the walker expected to find. The two validators below are the new
-- decode boundaries -- one for the pending chain (eleven walkers share the
-- single bm25_pending_iter_begin entry point, plus the truncate recycler), one
-- for every segment chain (bm25_seg_page_validate_kind, 15 call sites plus the
-- two that previously had no validation at all).
--
-- No bm25_debug_* lever can aim a chain at a wrong-kind page -- that needs a
-- corrupt nextblk or a corrupt root pointer inside a segment header -- so, the
-- same answer this file gives three times already, these are pure probes over
-- caller-supplied values. No relation is opened. For development and regression
-- testing only.
--
-- flags/want_kind are the raw BM25_PAGE_* bitmasks (bm25_format.h):
--   META 1, PENDING 2, SEGCAT 4, DICT 8, POST 16, NORMS 32, LIVE 64,
--   DOCMAP 128, RETIRED 256, DELETED 512, FIELDCFG 1024, KEYMAP 2048, POS 4096.
-- Both are bitmask TESTS (flags & want_kind), never equality: BM25_PAGE_DELETED
-- is ORed onto a live kind by bm25_page_mark_deleted.
CREATE FUNCTION bm25_debug_pending_page_flags_validate(flags int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- The chain-epoch half of a captured pending chain's page check (#291), the same
-- kind of pure probe: a page passes when page_epoch is 0 (a chain an older binary
-- started) or below epoch_bound (the next_gen the scan captured); otherwise it was
-- re-initialized after the snapshot and the walk raises 40001.  The real trigger
-- needs a hot standby and a concurrent recycle (t/025_standby_pending_recycle.pl).
CREATE FUNCTION bm25_debug_pending_epoch_validate(page_epoch bigint, epoch_bound bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- want_kind = 0 is the "don't validate" pass-through the bm25_seg_page_validate
-- wrapper uses; expected_gen is fixed at 0 inside the probe so only the kind
-- half is under test.
CREATE FUNCTION bm25_debug_seg_page_kind_validate(flags int, want_kind int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
-- bm25_pending_truncate's cycle cap. It is the only chain walker that DESTROYS
-- pages, and the page-kind check above does NOT double as a cycle guard
-- (bm25_page_mark_deleted ORs DELETED and leaves PENDING set, so a revisited
-- page still passes it). A real in-extent cycle cannot be built from SQL, so the
-- cap is extracted and driven here. bigint arguments so an out-of-uint32 value
-- reaches the C guard rather than failing an int4 cast first.
CREATE FUNCTION bm25_debug_pending_cycle_cap_validate(visited bigint, nblocks bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Field-id RLE codec round-trip (C2): encode a per-block field_ids[] as
-- (field_id, run_length) varbyte pairs, then decode back to the flat array.
-- Proves the encoder/decoder pair (shared with encode_block/bm25_seg_scan_postings)
-- round-trips.  For development and regression testing only.
--
-- IMMUTABLE, joining the sibling probes marked that way on the file's own stated
-- ground -- "pure functions of their arguments -- no relation is opened" -- which
-- this one, bm25_debug_accum, bm25_debug_accum_multi and bm25_debug_topk all meet
-- while having been left VOLATILE by omission (#148 SQL-13). ONE honest caveat,
-- shared by the two accumulator probes below: they fold their input through
-- bm25_tokenize, whose case fold is str_tolower() under the DATABASE DEFAULT
-- COLLATION, so the result is a function of the database as well as of the
-- arguments. That is exactly the dependency core accepts when it declares lower()
-- and upper() IMMUTABLE, and these four are REVOKEd from PUBLIC, take no regclass
-- and are set-returning (evaluate_function refuses to fold a proretset function at
-- all), so the marking buys consistency and costs no new folding surface.
CREATE FUNCTION bm25_debug_field_rle_roundtrip(field_ids int[],
  OUT idx int, OUT field_id int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT ROWS 100;
-- Decodes caller-supplied raw (field_id, run_length) varbyte-pair bytes against a
-- caller-chosen target count `n`, decoupled from however the bytes were produced --
-- unlike the round trip above (which always decodes the same n it just encoded),
-- this can drive bm25_field_rle_decode's symmetric tail check in EITHER direction:
-- a stream whose runs sum to less than n (undershoot) or more than n (overshoot).
CREATE FUNCTION bm25_debug_field_rle_decode_bytes(bytea, n int) RETURNS int[]
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Accumulator debug function: drives BM25Accum from a text array and returns
-- one row per posting (term, df, local_docid, tf, doclen).  Verifies dense
-- docid assignment, per-term df, per-doc tf, and doclen accounting.
-- For development and regression testing only; not part of the query path.
-- IMMUTABLE for the reason recorded above bm25_debug_field_rle_roundtrip,
-- including the database-collation caveat, which applies to this probe.
CREATE FUNCTION bm25_debug_accum(text[],
  OUT term text, OUT df int, OUT local_docid int, OUT tf int, OUT doclen int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT ROWS 10000;

-- Multi-field accumulator probe (C2): feeds field0[i] as field_id 0 and field1[i]
-- as field_id 1 for doc i, then returns per-(term,field) postings so the suite can
-- assert (term,field_id) is the posting key and doclen is tracked per field.
-- For development and regression testing only; not part of the query path.
-- IMMUTABLE for the reason recorded above bm25_debug_field_rle_roundtrip,
-- including the database-collation caveat, which applies to this probe.
CREATE FUNCTION bm25_debug_accum_multi(field0 text[], field1 text[],
  OUT term text, OUT field_id int, OUT df int,
  OUT local_docid int, OUT tf int, OUT doclen int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT ROWS 10000;

-- Sealed-segment dictionary debug function: walks segment 0's chained DICT and
-- returns (term, df) per entry.  Verifies the segment builder's seal output.
-- For development and regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_segterms(index regclass,
  OUT term text, OUT df int) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Manual seal: drain the pending list into a fresh immutable segment.  Normally
-- triggered opportunistically once the pending list crosses
-- bm25_native.seal_threshold; this function forces an immediate seal
-- regardless of size.
CREATE FUNCTION bm25_seal(regclass) RETURNS void
  AS 'MODULE_PATHNAME', 'bm25_seal_sql' LANGUAGE C STRICT;

-- Pending-list append debug function: tokenizes the text arg and appends it to
-- the named index's pending list with a synthetic TID, exercising the append +
-- drain machinery without the (Task 7) aminsert wiring.  For development and
-- regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_pending_append(regclass, text) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Segment catalog debug function: returns one row per sealed segment with
-- (ndocs, live_ndocs, total_len, nterms, has_positions).  Verifies catalog
-- snapshot and multi-segment sealing.  has_positions is the segment header's
-- pos_root != Invalid -- the M4 writer-observability witness: it reports what the
-- WRITER built (a POS chain or none), independent of the reader's store_positions
-- gate.  For development and regression testing only.
CREATE FUNCTION bm25_debug_segcat(index regclass,
  OUT ndocs bigint, OUT live_ndocs bigint, OUT total_len bigint, OUT nterms int,
  OUT has_positions bool, OUT total_tokens bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- Per-segment postings decode debug function: for each sealed segment, look up
-- the term in that segment's dictionary and decode its block postings, returning
-- (seg, local_docid, tf) per posting.  Exercises the df-bounded postings reader
-- over the shared segment-wide chain.  For development and regression testing only.
CREATE FUNCTION bm25_debug_seg_postings(index regclass, qterm text,
  OUT seg int, OUT local_docid bigint, OUT tf bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Per-field posting introspection (C2): like bm25_debug_seg_postings but surfaces
-- the field_id decoded from the per-block field-id RLE stream.  For development
-- and regression testing only; not on the query path.
CREATE FUNCTION bm25_debug_field_postings(index regclass, qterm text,
  OUT seg int, OUT local_docid bigint, OUT tf bigint, OUT field_id int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Block-impact round-trip probe (M2b Task 3, block-max WAND): for each segment,
-- look up qterm and walk its blocks with the header-only reader (no posting
-- decode), returning one row per (segment, block, field) impact-table entry:
-- (seg, block_no, nfields, field_id, max_tf, min_doclen). block_no is the
-- physical page the block lives on. Proves the impact table Task 2 stamped at
-- seal/merge time round-trips correctly. For development and regression
-- testing only; not on the query path.
CREATE FUNCTION bm25_debug_block_impacts(index regclass, qterm text,
  OUT seg int, OUT block_no bigint, OUT nfields int, OUT field_id int,
  OUT max_tf int, OUT min_doclen int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- Block-span probe (issue #289): one row per block of qterm's run in each
-- segment, in chain order -- (seg, block_ord, ndocs, first_docid, last_docid),
-- ndocs being the block's posting count. A block whose first_docid equals the
-- previous block's last_docid is a straddle (one document's postings split across
-- two blocks). For development and regression testing only.
CREATE FUNCTION bm25_debug_block_spans(index regclass, qterm text,
  OUT seg int, OUT block_ord int, OUT ndocs int, OUT first_docid bigint,
  OUT last_docid bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- WAND safe upper bound (M2b Task 4): the per-block score upper bound
-- (bm25_wand.c:bm25_block_ub) for qterm's FIRST live-segment block, using
-- live scan-time idf/avgdl/k1/b/boost. Paired with bm25_debug_term_contrib
-- below so a regression test can assert the bound dominates a REAL per-
-- posting contribution rather than only comparing it to itself.
-- bm25_block_ub itself IS on the live query path, from two call sites in
-- bm25_wand.c: wand_cursor_sweep_global_ub (run unconditionally by
-- bm25_wand_cursor_open) and bm25_wand_cursor_block_max (the pivot deep
-- check). Cursors are opened per query term per sealed segment by
-- bm25_wand_segment, under bm25_wand_build_ranking, which
-- bm25_scan_build_ranking_once (bm25_scan_rank.c) takes by DEFAULT --
-- bm25_native.wand_top_k defaults to 100, so every ordinary ranked (&@@)
-- scan runs this bound. Only THIS SRF is development and regression surface:
-- it merely exposes the bound to SQL so a test can compare it against the
-- actual per-posting contributions. Do not read "debug" as licence to relax
-- it -- the bound must dominate every real contribution, and a 1-ULP-low
-- change would silently prune documents out of every production top-k.
CREATE FUNCTION bm25_debug_block_ub(index regclass, qterm text) RETURNS float8
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Companion to bm25_debug_block_ub: one row per posting in qterm's FIRST
-- block (the exact same block the bound above covers), (local_docid, contrib)
-- with contrib the REAL per-posting BM25 contribution (bm25_termscore),
-- computed from the identical live stats. max(contrib) is the "true_max" the
-- bound must dominate -- see sql/43_wand_parity.sql. For development and
-- regression testing only.
CREATE FUNCTION bm25_debug_term_contrib(index regclass, qterm text,
  OUT local_docid bigint, OUT contrib float8)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- Bounded top-k min-heap probe (M2b Task 5, block-max WAND): offers
-- (scores[i], tid(0,tids[i])) for each i (scores and tids must have equal
-- length) into a heap of capacity k, then returns (rank, score, tid) in
-- final scan order (score desc, tid asc), rank starting at 1. No index, no
-- real corpus -- exercises the heap's own comparator (bm25_wand.c:worse /
-- bm25_topk_drain_sorted) against hand-picked scores/tids, including
-- score-tied entries whose tie MUST resolve by ascending tid to match the
-- exhaustive scan's comparator (bm25_scan_rank.c:scored_desc). Development and
-- regression testing only. IMMUTABLE for the reason recorded above
-- bm25_debug_field_rle_roundtrip; this one has no fold and so no caveat at all.
CREATE FUNCTION bm25_debug_topk(k int, scores float8[], tids int[],
  OUT rank int, OUT score float8, OUT tid int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT ROWS 100;

-- Per-term per-segment pull cursor probe (M2b Task 6, block-max WAND): drives
-- BM25WandCursor linearly to exhaustion over qterm's first live segment,
-- returning (tid, cursor_score) per doc. cursor_score must be bit-identical
-- to bm25_debug_rank's per-doc score for the same single-term query -- the
-- equivalence sql/43_wand_parity.sql checks -- proving the cursor's score_doc
-- reproduces the exhaustive scorer's per-doc, per-term contribution exactly.
-- Development and regression testing only; not on the query path.
CREATE FUNCTION bm25_debug_cursor_scan(index regclass, qterm text,
  OUT tid tid, OUT cursor_score float8)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Block-skip probe (M2b Task 7, block-max WAND): drives
-- bm25_wand_cursor_next_geq(target) over qterm's first live segment and
-- reports whether it landed on the same docid an independent linear (next())
-- walk to the same target would have (landed_ok), plus how many blocks the
-- skip bypassed header-only (blocks_skipped). target is a local docid.
-- Development and regression testing only; not on the query path.
CREATE FUNCTION bm25_debug_cursor_skip(index regclass, qterm text, target int,
  OUT landed_ok bool, OUT blocks_skipped int)
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- WAND instrumentation probe (M2b Task 11, C-TEST): runs a real WAND build
-- (bm25_wand_build_ranking) over the given query with a live block-skip
-- counter threaded through every cursor it opens, returning
-- (blocks_examined, blocks_skipped, docs_scored, deep_check_skips) totals for
-- the whole build. The only gate in the suite that can catch a "WAND" that
-- always decodes every block and never actually prunes -- bit-exact parity
-- against the exhaustive scorer (sql/43_wand_parity.sql) cannot: a full decode
-- of every block still produces the exact right answer. blocks_skipped alone
-- conflates plain pivot ALIGNMENT skips (every disjunctive WAND needs those)
-- with the block-max DEEP CHECK's own multi-cursor shallow-skip;
-- deep_check_skips isolates the latter -- see sql/44_wand_skip.sql Gate 1 vs
-- Gate 1b. Development and regression testing only; not on the query path.
CREATE FUNCTION bm25_wand_stats(index regclass, query text, k int,
  OUT blocks_examined int, OUT blocks_skipped int, OUT docs_scored int,
  OUT deep_check_skips int)
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT COST 5000;

-- Position introspection (M4): for each segment, look up qterm and decode every
-- position-bearing posting's frame via the lockstep POS cursor, returning
-- (seg, local_docid, field_id, position) -- one row per stored position.  A field
-- with store_positions=false yields NO rows (the reader gates on the per-field bit).
-- Exercises the POS chain, page-spanning frames, and the tf-count==tf self-check.
-- For development and regression testing only; not on the query path.
CREATE FUNCTION bm25_debug_seg_positions(index regclass, qterm text,
  OUT seg int, OUT local_docid bigint, OUT field_id int, OUT pos int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100000;

-- Per-segment KEYMAP introspection (C5 key_field): header {key_type,key_size,
-- keymap_root} plus, per dense local docid, the raw key bytes and a type-decoded
-- convenience column.  When the index has no key_field, one header-only row with
-- local_docid NULL / keymap_root Invalid (the ctid-fallback witness).  For
-- development and regression testing only; not on the query path.
CREATE FUNCTION bm25_debug_seg_keymap(index regclass, seg int,
  OUT local_docid bigint, OUT key_type int, OUT key_size int,
  OUT keymap_root bigint, OUT key_bytes bytea,
  OUT key_int4 int, OUT key_int8 bigint, OUT key_uuid uuid, OUT key_text text)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Key-resolved ranking (C5 key_field): like bm25_debug_rank but emits each ranked
-- row's decoded key_field value instead of its ctid (proves the scorer stashed
-- key[docid] beside the TID).  Only the column matching the index key_type is
-- populated; keyless index / pending-only rows yield NULL key columns.
CREATE FUNCTION bm25_debug_rank_key(index regclass, query text,
  OUT key_int4 int, OUT key_int8 bigint, OUT key_uuid uuid, OUT key_text text,
  OUT score float8)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000 COST 5000;

-- Live-docs test: returns true if local_docid in the named segment is live
-- (bit set in the segment's livedocs bitmap).  Immediately after seal, before
-- any deletes, every doc is live.  For development and regression testing only.
CREATE FUNCTION bm25_debug_seg_doc_live(index regclass, seg int, docid bigint)
  RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- SEGREAD-11 call-site probe (issue #154).  Reads a real segment header into a
-- private copy, substitutes the caller's chain root, and runs one of four bounded
-- walkers over it: dict_lookup, dict_iter, chain, chain_cursor.  An out-of-extent
-- root must raise ERRCODE_INDEX_CORRUPTED naming the chain, not the buffer
-- manager's short-read error.  Writes nothing.  Regression testing only.
CREATE FUNCTION bm25_debug_seg_chain_extent(index regclass, seg int,
                                            walker text, root bigint)
  RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- SEGREAD-11 call-site probe for the POST chain (issue #154).  Walks the segment's
-- FIRST dictionary term's postings with the caller's root substituted, and returns
-- how many postings were emitted.  A count, not a bool, because the defect here is
-- silence: a df > 0 term whose post_root is InvalidBlockNumber used to emit zero
-- postings and raise nothing.  root = -1 walks the real root and must count df.
-- Writes nothing.  Regression testing only.
CREATE FUNCTION bm25_debug_seg_postings_count(index regclass, seg int, root bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- SEGREAD-14 call-site probe (issue #154).  Positions one chain cursor on the
-- segment's NORMS chain, then deliberately reuses it for the DOCMAP read of the
-- same doc-id and returns the TID that comes back.  With the cursor's chain root
-- checked this is the row's real ctid; without it the read resumes inside the
-- NORMS chain.  Writes nothing.  Regression testing only.
CREATE FUNCTION bm25_debug_chain_cursor_crosstalk(index regclass, seg int,
                                                  docid bigint)
  RETURNS tid
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- SEGCAT extent-sample probe (issue #225).  Runs one of the four catalog walkers
-- (scan_snapshot, segcat_read, find_entry, locate_entry) with a caller-chosen
-- pre-lock extent sample in place of a fresh one; nblocks = -1 samples normally.
-- A stale sample is what a seal extending the relation mid-walk looks like, and a
-- healthy catalog must still read back in full.  find_absent and locate_absent
-- (issue #244) run the two lookups for a target that is never in the catalog, so
-- they walk the whole chain without first reading it.  Writes nothing.  Regression
-- testing only.
CREATE FUNCTION bm25_debug_segcat_walk(index regclass, walker text, nblocks bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY corruption lever (issue #225): WAL-log an arbitrary nextblk onto the
-- page-th page of a chain -- 'segcat' or 'pending' (seg = -1 for both; 'pending'
-- added for issue #243) or segment seg's 'keymap', 'post', 'live', 'docmap' or
-- 'norms' (the last four added for issues #293/#294) -- and
-- return the block stamped, so a walker's extent bound can be driven by a real
-- corrupt link.  Leaves the chain corrupt by construction -- drop the table after
-- using it.  Index owner only, like the other stamp levers.
CREATE FUNCTION bm25_debug_stamp_chain_next(index regclass, chain text, seg int,
                                            page int, nextblk bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY corruption lever (issue #244): WAL-log pd_lower back to the page header
-- on the page-th page of the segment catalog, so it reads as holding no entries while
-- keeping its nextblk, and return the block stamped.  No SQL path writes a catalog
-- page with no entries that links onward; the catalog walkers must reject one.
-- Leaves the catalog corrupt -- drop the table after using it.  Index owner only.
CREATE FUNCTION bm25_debug_stamp_segcat_empty(index regclass, page int)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY corruption levers for the validated chain walkers (issues #293/#294).
-- Each WAL-logs one forged value onto a real page of a segment chain and leaves the
-- index corrupt -- drop the table after using it.  Index owner only.
--   stamp_chain_lower:  pd_lower of the page-th page of `chain` (any chain
--                       stamp_chain_next takes), so it holds content_bytes bytes.
--   stamp_post_block:   field 'ndocs' or 'last_docid' of the block-th posting block
--                       on the page-th page of segment seg's POST chain.
--   stamp_docmap_tid:   segment seg's DOCMAP cell for docid, through an intact chain.
--   stamp_seg_root:     segment seg's header root for 'live', 'docmap' or 'norms';
--                       returns the old root.
--   livedocs_clear:     tombstone one document as bm25_bulkdelete does, but without
--                       its liveness read, to reach the write path's own checks.
CREATE FUNCTION bm25_debug_stamp_chain_lower(index regclass, chain text, seg int,
                                             page int, content_bytes int)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_stamp_post_block(index regclass, seg int, page int,
                                            block int, field text, value bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_stamp_docmap_tid(index regclass, seg int, docid bigint,
                                            tid tid)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_stamp_seg_root(index regclass, seg int, chain text,
                                          root bigint)
  RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_livedocs_clear(index regclass, seg int, docid bigint)
  RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY raw corruption lever (issues #302/#303/#309): write `bytes` at byte
-- offset `off` of block `blkno` (any existing block, the metapage included) under that
-- page's EXCLUSIVE lock and a Generic WAL full-page image, and return the bytes it
-- replaced.  It checks the range and that the result still passes PostgreSQL's page
-- header check, with pd_lower past the header and an all-zero pd_lower..pd_upper hole
-- (Generic WAL zeroes the hole, so nothing poked or already there may live in it), and
-- nothing about bm25 structure, which is what it exists to forge.  Take offsets from
-- bm25_debug_layout(), never from literals.  Leaves the index as corrupt as the bytes
-- make it: drop the table afterwards, or poke the returned bytes back, which restores
-- every byte but pd_lsn and pd_checksum.  Index owner only.
CREATE FUNCTION bm25_debug_poke_page(index regclass, blkno bigint, off int, bytes bytea)
  RETURNS bytea
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The on-disk layout bm25_debug_poke_page addresses: one (struct, field, off, size)
-- row per member of each bm25 page struct, from offsetof/sizeof, plus a field '*' row
-- with the struct's whole size (its stride as an array element).  Offsets are relative
-- to the struct.  Lower-case pseudo-structs: 'page' (where the contents and the
-- special-area opaque start) and 'impact_table'/'impact_entry' (the packed per-block
-- impact encoding).  Regression testing only.
CREATE FUNCTION bm25_debug_layout(OUT struct text, OUT field text, OUT off int,
                                  OUT size int)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- KEYMAP cursor chain-identity probe (issue #225): parks one reader's KEYMAP cursor
-- on segment seg_a, then reads segment seg_b's key `docid` through it and returns
-- the bytes, which must equal seg_b's real key.  Writes nothing.  Regression testing
-- only.
CREATE FUNCTION bm25_debug_keymap_cursor_crosstalk(index regclass, seg_a int,
                                                   seg_b int, docid bigint)
  RETURNS bytea
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Reader-vs-one-shot differential (issue #225).  For each docid of the array, in
-- the order given, reads one per-doc value ('doclen' or 'key') through ONE
-- BM25SegReader carried across the whole array and through the matching one-shot
-- root walk, and returns both beside the doc's TID.  A descending or scattered order drives the cursor's
-- backward-jump fallback.  Writes nothing.  Regression testing only.
CREATE FUNCTION bm25_debug_seg_reader_diff(index regclass, seg int, what text,
  docids int[],
  OUT local_docid bigint, OUT tid tid, OUT reader_bytes bytea,
  OUT oneshot_bytes bytea, OUT reader_int bigint, OUT oneshot_int bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10000;

-- Tombstone introspection: one row per local doc-id across all sealed segments,
-- with its segment generation and live/dead state.  Walks the catalog, reads each
-- segment header, and tests every bit in the livedocs bitmap.  For development and
-- regression testing only; not part of the query path.
CREATE FUNCTION bm25_debug_tombstone(index regclass,
  OUT gen int, OUT local_docid bigint, OUT live boolean)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100000;

-- Global-stats reader: returns (ndocs, total_len) from the metapage cache.
-- The sealer/merger keep these live and tombstone-adjusted; one read gives the
-- corpus-wide N and sum(doclen) for avgdl/idf.  For development and regression
-- testing only; not on the query path.
CREATE FUNCTION bm25_debug_global_stats(index regclass,
  OUT ndocs bigint, OUT total_len bigint)
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Per-field sum(doclen) reader (C2): returns (field_id, total_len) from the named
-- segment's header length-prefixed total_len_by_field[] array.  Verifies the
-- per-field sum(doclen) survives seal.  For development and regression testing only.
CREATE FUNCTION bm25_debug_seg_lenfields(index regclass, seg int,
  OUT field_id int, OUT total_len bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10;

-- TEST-ONLY corruption lever: WAL-log an arbitrary field_count onto one sealed
-- segment's header page.  The validator family above can be tested as pure
-- functions because a validator is a pure function; whether a validator is
-- actually CALLED on a given path cannot be, so this hands the real path a real
-- corrupt page.  Leaves that segment permanently unreadable by construction --
-- drop the table after using it.
CREATE FUNCTION bm25_debug_stamp_seg_field_count(index regclass, seg int,
  field_count int) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Per-field corpus stats (C3): one (field_id, ndocs_field, total_len_field) row
-- per field, summed over all live segments + pending -- the BM25F avgdl/idf inputs
-- (avgdl_field = total_len_field / ndocs_field).  Witnesses the ndocs_by_field[]
-- header serialization + the corpus-stat accessor.  Regression testing only.
CREATE FUNCTION bm25_debug_field_stats(index regclass,
  OUT field_id int, OUT ndocs_field bigint, OUT total_len_field bigint)
  RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 10;

-- Page-count probe: RelationGetNumberOfBlocks of the named index.  Used by the
-- VACUUM-reclaim suite to assert the relation does not grow unboundedly across
-- repeated seal/reclaim cycles (a pg_freespacemap-free alternative to
-- pg_freespace).  For development and regression testing only.
CREATE FUNCTION bm25_debug_npages(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY: force a format_version onto the metapage to exercise the v3-reject
-- migration gate. Not part of the query path.
CREATE FUNCTION bm25_debug_stamp_version(regclass, int, int, bigint) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- TEST-ONLY debug levers for the format-negotiation suite (roadmap #4).
-- bm25_debug_stamp_version widens from a single format_version poke to all three
-- negotiation fields; the 2-arg form from 0.1 is dropped in favor of this one.
-- bm25_debug_write_optional_region emulates a future additive on-disk region that
-- this (v6) binary must structurally ignore. Neither is part of the query path.
CREATE FUNCTION bm25_debug_write_optional_region(regclass) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- bm25_debug_check_optional_region reads that region back and reports whether it is
-- still intact and still covered by pd_lower -- the probe for the pd_lower-RAISE
-- invariant (an assigning metapage writer would have let GenericXLog zero it as page
-- hole). bm25_debug_alloc_unknown_page / bm25_debug_page_flags are the twin probe for
-- the orphan sweep's unknown-page-kind guard: allocate a page carrying a flag bit
-- outside BM25_PAGE_ALL_KNOWN (a stand-in for a future page type this build cannot
-- reach), then assert VACUUM left it alone. Both guards are what make the additive
-- contract true; see docs/adr/0009-format-stability.md.
CREATE FUNCTION bm25_debug_check_optional_region(regclass) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_alloc_unknown_page(regclass) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_page_flags(regclass, int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- One page's raw seg_gen: a segment page's generation, a pending page's chain epoch
-- (#291), 0 for the rest.  bigint because seg_gen is unsigned 32-bit.
CREATE FUNCTION bm25_debug_page_seg_gen(regclass, int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- Companion probe for the page-recycle horizon (issue #135): whether one page's
-- retire_xid is valid, i.e. whether its reuse waits on the cluster horizon.  A bool
-- rather than the raw xid8 so expected output is stable.  Paired with
-- bm25_debug_page_flags it pins that every recycled pending page is horizon-gated:
-- bm25_page_mark_deleted ORs DELETED in, so a drained pending page still matches
-- (flags & (BM25_PAGE_DELETED | BM25_PAGE_PENDING)).
CREATE FUNCTION bm25_debug_page_retire_xid_valid(index regclass, blkno int) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- The metapage's pending_head, NULL when the pending list is empty/detached.
-- bm25_stats() exposes pending_ndocs but not the anchor, and pending_ndocs cannot
-- see issue #131: VACUUM's pending sweep drives it to 0 before the defective seal
-- runs, so it reads identically pre- and post-fix.  The anchor block number is the
-- state that latches, so it is what the suite must assert on.
CREATE FUNCTION bm25_debug_pending_head(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- Invalidate every pending record's TID on ONE page, as VACUUM's sweep does, and
-- return the count.  Makes a PARTIALLY SWEPT chain reproducible without racing a
-- clock: stranding a continuation record in the field needs a VACUUM cancelled
-- between two specific pages, and a statement_timeout tuned against the sweep's
-- runtime strands a fragment only sometimes (measured: 30ms stranded nothing, 15ms
-- stranded and PANICked a cassert build).  Ownership-gated like every write-side
-- debug helper.
CREATE FUNCTION bm25_debug_pending_invalidate_page(index regclass, blkno int) RETURNS int
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- The n'th block of the pending chain in CHAIN (== append) order, n = 0 being the
-- head; NULL when the chain is shorter.  Needed to target a page partway down the
-- chain: bm25_debug_pending_head can only reach the first document, so it can strand
-- a continuation only in the "no preceding active document" shape.  Not `head + 1`,
-- which works only while the allocator hands out pending pages sequentially.
CREATE FUNCTION bm25_debug_pending_nth_page(index regclass, n int) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- VACUUM pending-sweep extent-sample probe (issue #243).  Runs the real pending
-- sweep with a caller-chosen extent sample in place of a fresh one (nblocks = -1
-- samples normally) and a callback that reports nothing dead, and returns the number
-- of chain pages visited.  A stale sample is what an INSERT linking a new tail page
-- mid-sweep looks like, and a healthy chain must still be walked in full.  Writes
-- nothing.  Regression testing only.
CREATE FUNCTION bm25_debug_pending_sweep(index regclass, nblocks bigint) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- Run a seal's build phase and stop before its publish record: the pending list is
-- drained into orphan segment pages and the catalog entries are discarded, leaving
-- the anchor, catalog and stats untouched.  Returns the first orphan segment's header
-- block, NULL when nothing was built.  This is the on-disk state a seal or merge
-- leaves when it dies between its orphan build and its publish record, which the
-- crash suites cannot reach by timing an immediate stop (issue #226: the maintenance
-- calls assign no xid, so an unflushed in-flight operation is lost whole).
-- Ownership-gated like every write-side debug helper.
CREATE FUNCTION bm25_debug_seal_unpublished(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- Orphan-sweep gate probes (issue #300). TEST-ONLY. bm25_debug_sweep_evidence reports
-- the metapage's orphan-op bracket counters and whether the last completed sweep ran in
-- this server lifetime (the crash epoch itself is random, so it is reduced to a bool).
-- bm25_debug_orphan_sweeps is a process-local count of sweeps this backend ran past the
-- gate. bm25_debug_set_pending_tail_epoch is MUTATING and owner-only: it overwrites the
-- pending tail page's chain epoch (an older binary's 0 page) and returns the old one,
-- NULL for an empty chain.
CREATE FUNCTION bm25_debug_sweep_evidence(index regclass, OUT ops_begun bigint,
                                          OUT ops_done bigint, OUT swept_this_epoch bool)
  RETURNS record AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_orphan_sweeps() RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION bm25_debug_set_pending_tail_epoch(index regclass, epoch bigint) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
-- bm25_debug_enable_synthetic_transform routes bm25_upgrade through the
-- segment-rewrite path (a full re-emit of every segment via the merge swap, with
-- the version re-stamp folded into the swap's WAL record) instead of the
-- metadata-only re-stamp, standing in for a real breaking transform so the
-- rewrite+swap+restamp machinery can be exercised end to end. Backend-local; not
-- part of the query path.
CREATE FUNCTION bm25_debug_enable_synthetic_transform(bool) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Online, no-REINDEX format upgrade (roadmap #4 Task 5). Seals pending, then
-- re-stamps the metapage to this build's format if the index is behind; a
-- no-op NOTICE when already current. See src/bm25_upgrade.c.
CREATE FUNCTION bm25_upgrade(index regclass) RETURNS text
  AS 'MODULE_PATHNAME', 'bm25_upgrade_sql' LANGUAGE C STRICT;

-- Order-by "distance" = -score (ascending distance == descending relevance).
-- Concurrency (R3): this is ALSO the function PostgreSQL calls to materialize the
-- &@@ sort key as a resjunk column on EVERY ranked query (whenever a secondary
-- ORDER BY key or a direct SELECT of the distance forces the value to be
-- materialized) -- so it deliberately does NOT fail loud under concurrent scored
-- scans; doing so would break any statement with two coexisting ranked scans.
-- Instead it resolves by IDENTITY (#138): it walks the active scored scans and
-- returns the stashed per-row distance of the one whose own order-by RHS matches
-- this call's, byte for byte, and which is actually positioned on a row. Taking the
-- registry HEAD was the earlier behaviour and was wrong -- a non-driving scan
-- projected the driving scan's distance. Row ORDER is always index-correct (the
-- index supplies xs_orderbyvals regardless). Residual, deliberate: two live scans
-- ranking the BYTE-IDENTICAL query still resolve recency-first, because nothing in
-- the operator's signature can separate them. Degrades to +inf off the index (seqscan, or no active
-- scored scan at all) -- that is not an error.
-- Volatility (review ref H2): STABLE PARALLEL RESTRICTED, not IMMUTABLE PARALLEL
-- SAFE. The body reads bm25_scored_scan_head() -- backend-local scan state -- so
-- the same arguments give different answers on different rows. IMMUTABLE invited
-- constant-folding by eval_const_expressions and made the function legal in index
-- expressions, CHECK constraints and partition pruning, none of which have a
-- scored scan; PARALLEL SAFE let a worker evaluate it with no scan state at all,
-- silently returning +inf as the sort key. bm25_score(tid)/bm25_score_key() are
-- already PARALLEL RESTRICTED over the IDENTICAL state.
--
-- Not VOLATILE, which is what the behavior literally is (the value changes within
-- a single scan). Measured: under VOLATILE the planner refuses the index ordering
-- path and inserts a Sort, which evaluates this outside any scored scan and
-- returns rows in the WRONG order -- it silently breaks ranking. STABLE is the
-- strictest marking that keeps the amcanorderbyop path, and is honest enough:
-- within one index scan the value is a projection of that scan's current row.
CREATE FUNCTION bm25_distance(text, text) RETURNS float8
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
CREATE OPERATOR &@@ (LEFTARG = text, RIGHTARG = text, PROCEDURE = bm25_distance);

-- Accessor: positive BM25 score of the current scan's row.  PARALLEL RESTRICTED:
-- it reads backend-local scan state, which is per-worker and not shareable.
-- Concurrency (R3): resolves ctid to its scored scan by ROW IDENTITY (an intrusive
-- registry of active scans replaces the old single backend-global slot). Fast path:
-- the scan whose just-emitted row matches ctid, most-recently-registered first,
-- except that a call site projected directly on its own scan's emitted rows keeps
-- that scan's score when another live scan's current row coincides (#242). Where a
-- collision cannot be attributed, or the projection is decoupled from its scan
-- (PL/pgSQL FOR-loop prefetch, Sort/Materialize, cursors), it can return another
-- concurrent scan's score; see src/bm25_score.c.
-- Fallback: when no scan behind the registry head can still emit, an O(1)
-- per-scan hash covers a decoupled projection (ctid read after the row was
-- emitted, or in arbitrary order). Returns NULL when no scored scan owns this row (none active, the row
-- isn't in the ranking, or >=2 active with no current-row match). Under concurrent
-- scored scans it is not guaranteed to be the owning scan's number (see above).
CREATE FUNCTION bm25_score(tid) RETURNS float8
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Accessor: score of the current scored scan's row, resolved by its key_field value
-- (M5).  Four thin SQL overloads (int/bigint/uuid/text) over one C body; the C code
-- decodes the argument by the ARGUMENT's OWN type (get_fn_expr_argtype), never the
-- active scan's key_type -- decoding a mismatched Datum as another type's key would,
-- e.g., run DatumGetUUIDP over an integer and dereference it as a pointer (a crash).
-- Use when only the user id is projected; bm25_score(tid) stays the primary accessor.
-- Concurrency (R3): same row-identity resolution as bm25_score(tid) -- a current-row
-- fast path (recency-first across the active-scan registry, with the same #242
-- call-site override and residuals) then an O(1) hash fallback when exactly one
-- scan is active. A key-width mismatch between
-- concurrently-active scans (e.g. an int key_field vs. a uuid one) is guarded
-- before any byte comparison, never read out of bounds.
-- Returns NULL when the scan has no key_field, the key is absent from the ranking,
-- or no scored scan owns the row.
CREATE FUNCTION bm25_score_key(int)    RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(bigint) RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(uuid)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(text)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key' LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Query-qualified accessors (#253): the same scores, resolved by naming the scan
-- instead of inferring it. The caller passes the query it ranked by (the RHS of its
-- ORDER BY ... &@@, text or jsonb, byte-identical), and optionally the heap the row
-- came from (tableoid; oid casts to regclass implicitly). Candidates are the scans
-- ranking that query, by the identity rule the &@@ distance projection uses (ADR
-- 0061), narrowed to that heap when given; each candidate's WHOLE ranking is probed
-- rather than its current row, so a projection decoupled from its scan (PL/pgSQL
-- FOR prefetch, Sort/Materialize, a cursor) still resolves. Two candidates holding
-- the row with different scores give NULL rather than a guess. This covers the
-- residual shapes the one-argument forms above can misattribute: key-join
-- collisions, decoupled projections, and ctids colliding across the children of a
-- Merge Append (pass tableoid). The one-argument forms are unchanged.
-- Volatility and parallel marking as for the one-argument forms (VOLATILE by
-- omission, PARALLEL RESTRICTED): the identical backend-local registry is read, and
-- sql/103 pins that no scan-state reader differs from the others.
-- bm25_score_key(key, query) has no regclass form. It needs none to stay correct:
-- a key held by two ranked rows with different scores gives NULL, whether the rows
-- are in two heaps or in one (a non-unique key_field).
CREATE FUNCTION bm25_score(tid, text) RETURNS float8
  AS 'MODULE_PATHNAME', 'bm25_score_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score(tid, jsonb) RETURNS float8
  AS 'MODULE_PATHNAME', 'bm25_score_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score(tid, text, regclass) RETURNS float8
  AS 'MODULE_PATHNAME', 'bm25_score_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score(tid, jsonb, regclass) RETURNS float8
  AS 'MODULE_PATHNAME', 'bm25_score_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(int, text)    RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(bigint, text) RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(uuid, text)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(text, text)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(int, jsonb)    RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(bigint, jsonb) RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(uuid, jsonb)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;
CREATE FUNCTION bm25_score_key(text, jsonb)   RETURNS float8 AS 'MODULE_PATHNAME','bm25_score_key_query_jsonb' LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Snippet highlighter (M4 C-SNIPPET, D8/D11): a <mark>-wrapped excerpt of the passed
-- field VALUE around the current scored scan's query terms -- for a jsonb query, every
-- leaf outside a must_not, of any kind (#308). Re-analyzes the text with
-- the active scan's cached analyzer config (D8 -- the stored positions are NOT consulted)
-- and returns SQL NULL on zero hits / no active scan / NULL field / NULL max_num_chars
-- (callers typically drop NULL snippets); max_num_chars <= 0 errors. NOT STRICT: the C body
-- applies the SQL defaults, so a NULL tag or escape takes its default, and returns NULL on a
-- NULL field itself.
--
-- VOLATILE + PARALLEL RESTRICTED, matching bm25_score over the identical backend-local
-- state (#148 SQL-05). This was STABLE, under a comment that claimed it was marked
-- "exactly like bm25_score" while bm25_score is VOLATILE -- three functions read the
-- same scored-scan registry and carried TWO different markings between them
-- (bm25_distance STABLE, bm25_snippet STABLE, bm25_score/bm25_score_key VOLATILE),
-- and only bm25_distance's was argued. STABLE is the one direction that is not merely
-- inconsistent but unsafe here:
-- evaluate_function folds a STABLE call whose arguments are ALL Const when
-- context->estimate is set, and the four SQL defaults below are themselves Consts, so
-- bm25_snippet('a literal') is a wholly-Const call that gets evaluated during
-- estimation with no scored scan live and yields NULL. Contrived (the field is
-- normally a Var), but it is a real folding surface and VOLATILE closes it at no cost:
-- unlike bm25_distance, this function is not on the amcanorderbyop path, so nothing
-- depends on it staying foldable/sortable -- see the volatility note above
-- bm25_distance for the case where that distinction bites.
--
-- VOLATILE is not free, and the cost is a PLAN SHAPE rather than a wrong answer.
-- make_sort_input_target (planner.c) postpones UNCONDITIONALLY for any tlist column
-- containing a volatile function, so a query with an ORDER BY now projects the
-- snippet ABOVE the sort (an extra Result node) where the STABLE/procost-1 marking
-- kept it below; and contain_volatile_functions on a subquery targetlist blocks
-- subquery pull-up, so SELECT ... FROM (SELECT bm25_snippet(body) ...) t no longer
-- flattens. Both were checked: the snippet is still correct above the sort, because
-- the scored-scan registry is keyed on the SCAN and the scan node is not shut down
-- until the plan ends. Projecting later is the price of not being foldable, and it
-- is the right side of the trade for a per-row function that already costs 5000.
--
-- COST 5000, the same order as bm25_match(text,text) (#148 SQL-04). It carried
-- procost = 1 by omission while calling bm25_analyze over the query terms AND the
-- whole field value on every row, then running a window search and building a
-- StringInfo -- strictly more per-row work than bm25_match, whose own thirty-line
-- derivation above transfers directly and is not repeated. procost = 1 mis-prices
-- every path carrying the snippet projection, which is exactly the input to whether
-- the projection lands below or above a Sort/Limit/Gather.
-- Concurrency (R3): FAILS LOUD -- ERROR, FEATURE_NOT_SUPPORTED -- instead of guessing,
-- when another registered scored scan could still genuinely emit a future row.
-- Unlike bm25_score/bm25_score_key, a snippet carries no row identity to resolve
-- against, so under real concurrency there is no safe answer; this is the one
-- accessor that refuses rather than risk a silently-wrong excerpt. The ambiguity
-- check is NOT a raw "is another scan registered" count (a finished sibling, e.g.
-- one arm of a UNION ALL of ranked snippet queries, lingers in the registry until
-- its own endscan) -- it only errors when a scan behind the head could still
-- produce a future row, so independent finished siblings never false-trigger.
-- max_num_chars counts CHARACTERS of the original field text (not bytes, and not
-- output length): the same budget yields the same excerpt length in Japanese as in
-- English. Tags and ellipses are outside the budget, and so is the expansion of any
-- escaped character -- '&' spends one unit of budget and emits five bytes.
--
-- escape => true (the default) HTML/XML-escapes & < > " ' in the field text, because
-- the tag defaults below mean the documented use of this function is to render its
-- result as HTML, and markup stored in the indexed column would otherwise reach the
-- browser verbatim. This DIVERGES from ts_headline, which escapes nothing. The TAGS
-- are never escaped -- they are markup by contract -- so pass only trusted tags.
-- escape => false restores byte-verbatim field text for non-HTML consumers.
CREATE FUNCTION bm25_snippet(field text,
                             start_tag text DEFAULT '<mark>',
                             end_tag text DEFAULT '</mark>',
                             max_num_chars int DEFAULT 300,
                             escape boolean DEFAULT true) RETURNS text
  AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE PARALLEL RESTRICTED COST 5000;

-- Extend the opclass: search strategy 1 = @@@ match, order-by strategy 2 = &@@.
-- (CREATE OPERATOR CLASS in Task 2 implicitly created the same-named family.)
ALTER OPERATOR FAMILY text_bm25_ops USING bm25_native
  ADD OPERATOR 2 &@@ (text, text) FOR ORDER BY float_ops;

-- Merge-policy introspection: (gen, ndocs, live_ndocs, layer, chosen) per live
-- segment, with `chosen` reflecting bm25_merge_select. Debug/regression only.
CREATE FUNCTION bm25_debug_merge_plan(index regclass,
  OUT gen int, OUT ndocs bigint, OUT live_ndocs bigint,
  OUT layer int, OUT chosen bool) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- Budget-aware merge-policy introspection (BUILD-04): per live segment, the
-- predicted accumulator residency of replaying it, bm25_merge_select's raw pick,
-- and what survives the memory-budget trim at the CURRENT
-- bm25_maintenance_budget_bytes; predicted_chunks is what the chunker is expected
-- to emit for the kept set.  A separate function rather than extra columns on
-- bm25_debug_merge_plan, whose row type sql/19 and sql/20 already pin.
-- est_bytes is a sum of sizeof()s, so its value is platform-dependent: assert
-- relationships over it, never a number.  Debug/regression only.
CREATE FUNCTION bm25_debug_merge_budget_plan(index regclass,
  OUT gen int, OUT est_bytes bigint,
  OUT chosen bool, OUT kept bool, OUT predicted_chunks int) RETURNS SETOF record
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT ROWS 100;

-- Count RANGE entries on the persistent retired-free list (retired SEGMENTS
-- awaiting horizon reclamation; one entry per dropped segment). Debug/regression
-- only.
CREATE FUNCTION bm25_debug_retired_count(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Raw on-page bytes of one catalog / retired descriptor, padding hole included. The
-- #144 class (uninitialized struct padding reaching a WAL-logged page) changes no query
-- result, so it is invisible to every answer-checking assertion; these hand SQL the
-- actual bytes so a suite can assert the hole is zero. Development and regression
-- testing only.
CREATE FUNCTION bm25_debug_segcat_entry_bytes(index regclass, n int) RETURNS bytea
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION bm25_debug_retired_entry_bytes(index regclass, n int) RETURNS bytea
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Count PAGES on the retired-list chain (descriptor pages, not entries). Stays O(1)
-- once reclaim unlinks emptied non-head descriptors; grows ~1 per merge without it.
-- Debug/regression only.
CREATE FUNCTION bm25_debug_retired_pages(index regclass) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Force a drain + a merge down toward target_segment_count, reclaiming pages.
-- (bm25_seal(regclass) is already defined above / by Phase 1.)
CREATE FUNCTION bm25_merge(index regclass) RETURNS void
  AS 'MODULE_PATHNAME', 'bm25_merge_sql' LANGUAGE C STRICT;

-- M6: injection-safe jsonb query builders. Each returns a jsonb literal
-- describing one query node; the tree's shape comes from which builder was
-- CALLED, never from parsing a caller-supplied string, so a term whose value
-- is literally 'AND' or 'OR' (or any other query syntax) can only ever land
-- as a jsonb string VALUE, not as an operator. bm25_boolean/bm25_boost accept
-- jsonb (sub-)queries and so compose/nest arbitrarily deep before anything
-- is evaluated. All IMMUTABLE: unlike bm25_match(text,text), these builders
-- never tokenize or touch the catalog -- they only shuffle their own
-- arguments into jsonb.
--
-- All PARALLEL SAFE, and that clause is load-bearing rather than tidiness
-- (#148 SQL-02). standard_planner computes glob->parallelModeOK from
-- max_parallel_hazard(parse) over the RAW parse tree, BEFORE subquery_planner
-- runs eval_const_expressions -- so a single PARALLEL UNSAFE FuncExpr anywhere
-- in a query disables parallelism for the WHOLE plan, even though these builders
-- are IMMUTABLE and get folded to a Const that never executes. Omitting the
-- clause defaults to UNSAFE, so the six of them silently de-parallelised every
-- join, aggregate and CTE sitting above a jsonb bm25 predicate. (The bm25 index
-- scan is itself amcanparallel = false, so the loss landed entirely on the rest
-- of the query.) They read no relation, no GUC and no backend-local state.
--
-- PARALLEL SAFE here DEPENDS ON the pg_catalog qualification argued below, and the
-- two are worth joining explicitly because they are argued separately. These are
-- string-bodied LANGUAGE sql functions, so the body is re-parsed at CALL time --
-- including inside a parallel worker -- while max_parallel_hazard reads only the
-- WRAPPER's declared proparallel and never looks inside. An unqualified inner call
-- that resolved to a user function would therefore execute in a worker under this
-- declaration. ADR 0051 is what makes the claim true; do not strip a prefix below
-- and leave these clauses standing.
--
-- Every call inside these bodies is pg_catalog-QUALIFIED, and must stay that
-- way (M1 #58.1, ADR 0051). A LANGUAGE sql function with a STRING body (as
-- opposed to BEGIN ATOMIC) is re-parsed at CALL time under the CALLER's
-- search_path, so an unqualified jsonb_build_object/to_jsonb here is resolved
-- against whatever the caller can see -- the CVE-2018-1058 pattern. It is not
-- merely a schema-order race, either: once the candidate argument-type lists
-- differ, func_select_candidate ranks by COUNT OF EXACT INPUT-TYPE MATCHES, so
-- a user-created non-variadic jsonb_build_object(text,text,text,text) scores 2
-- against pg_catalog's VARIADIC "any" builtin's 0 and wins OUTRIGHT, whatever
-- the order of the path. A hijacked body would return an attacker-chosen tree
-- from a builder whose entire stated purpose above is that the tree's shape
-- comes from which builder was CALLED -- and would run with the caller's
-- privileges. sql/85_builder_search_path pins it.
--
-- Qualification is preferred over `SET search_path = pg_catalog`: it is the
-- CVE-2018-1058 remediation PostgreSQL gives extension authors, it costs
-- nothing at runtime, and a SET clause would add a GUC save/restore per call
-- and foreclose inlining permanently. (These builders do NOT inline today
-- regardless -- jsonb_build_object/to_jsonb are STABLE, and inline_function
-- refuses an IMMUTABLE-declared SQL function whose body calls mutable
-- functions -- so inlining is a door left open, not one being held.)
-- COALESCE below needs nothing: it is SQL GRAMMAR parsed into a CoalesceExpr,
-- never a function name resolved through the path.
--
-- Naming note: the design doc names the "match" ({analyzed OR-of-terms})
-- builder bm25_match(field text, terms text) -- the SAME (text,text) argument
-- signature as the pre-existing bm25_match(text,text) RETURNS bool (the @@@
-- (text,text) operator's seqscan anchor, declared near the top of this file).
-- PostgreSQL keys a
-- function's identity on name + input-argument-types only, not return type,
-- so the two cannot coexist under one name; CREATE FUNCTION errors "already
-- exists with same argument types". The (text,text) anchor is pre-existing,
-- untouched, and used only through the @@@ operator (never called by name in
-- any existing test or later M6 task brief), so the new builder is the one
-- renamed here rather than disturbing the established anchor + operator.
CREATE FUNCTION bm25_match_terms(field text, terms text) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('match', pg_catalog.jsonb_build_object('field', field, 'terms', terms)) $$;
CREATE FUNCTION bm25_term(field text, value text) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('term', pg_catalog.jsonb_build_object('field', field, 'value', value)) $$;
CREATE FUNCTION bm25_phrase(field text, phrase text, slop int DEFAULT 0, ordered boolean DEFAULT true)
  RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('phrase', pg_catalog.jsonb_build_object('field', field, 'phrase', phrase, 'slop', slop, 'ordered', ordered)) $$;
CREATE FUNCTION bm25_wildcard(field text, pattern text) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('wildcard', pg_catalog.jsonb_build_object('field', field, 'pattern', pattern)) $$;
-- must/should/must_not default to '{}' (empty array), not NULL, so a caller
-- can pass only e.g. must => ... and still get a well-formed node with all
-- three keys present; coalesce covers a caller passing NULL explicitly too.
CREATE FUNCTION bm25_boolean(must jsonb[] DEFAULT '{}', should jsonb[] DEFAULT '{}', must_not jsonb[] DEFAULT '{}')
  RETURNS jsonb LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('boolean', pg_catalog.jsonb_build_object(
            'must', pg_catalog.to_jsonb(coalesce(must,'{}')),
            'should', pg_catalog.to_jsonb(coalesce(should,'{}')),
            'must_not', pg_catalog.to_jsonb(coalesce(must_not,'{}')))) $$;
-- bm25_boost is the ONE builder whose rendering was not a pure function of its
-- arguments, so IMMUTABLE was a false claim (#148 SQL-01). Passing a float8
-- straight to jsonb_build_object routes it through JSONBTYPE_NUMERIC, which is
-- OidOutputFunctionCall -> float8out -- and float8out honours extra_float_digits,
-- a PGC_USERSET GUC. Measured on PG 18.3: the same call yields
-- {"weight": 0.3333333333333333} at extra_float_digits = 1 and
-- {"weight": 0.333333333333} at -3. IMMUTABLE is what lets a function into an
-- index expression and what licenses plan-time folding, so the two observable
-- consequences were a functional index that disagrees with a heap recheck across
-- a session-local SET, and a cached plan with the planning session's setting
-- baked in.
--
-- The fix keeps IMMUTABLE (the more useful marking for a builder that is
-- otherwise genuinely pure) and makes the RENDERING setting-independent instead:
-- float8 -> numeric goes through float8_numeric, which formats with a hard-coded
-- DBL_DIG and never reads the GUC, and numeric_out likewise ignores it. Verified
-- on PG 18.3: 0.333333333333333 at both extra_float_digits = 1 and -3.
--
-- The price, stated because it IS a behavior change. float8_numeric formats with
-- "%.*g" at DBL_DIG, so the weight is now recorded to 15 significant decimal
-- digits rather than float8's 17-digit shortest-exact form: a weight round-trips
-- through the tree with up to ~1e-16 relative error, which a BM25 score
-- multiplier cannot distinguish.
--
-- A NARROW BAND OF INPUTS stopped working, at the top of the range. That 15-digit
-- rounding is round-to-nearest, so a weight near DBL_MAX rounds OUTWARD past it and
-- numeric_float8 then overflows when the C parser reads it back, raising "out of
-- range for type double precision" where the value used to round-trip.
--
-- Measured on PG 18.3, exactly: the top FOUR doubles overflow -- DBL_MAX
-- (1.7976931348623157e308) and the three below it, down to
-- 1.7976931348623151e308. The fifth, 1.797693134862315e308, round-trips. So the
-- band is the top four representable values and nothing else; 1e308 is some 4e15
-- ULPs below it and is unaffected, which is why 1e308 is NOT a boundary pin.
-- sql/103_declaration_properties brackets the real edge with the adjacent pair
-- 1.797693134862315e308 (works) / 1.7976931348623151e308 (errors).
--
-- Accepted rather than worked around: these are absurd score multipliers and the
-- failure is a loud ERROR rather than a wrong number. But it IS a real new error on
-- previously-working input, so it is pinned rather than left to be rediscovered.
--
-- The cast target is written pg_catalog-QUALIFIED, and the honest reason is NOT
-- the CVE-2018-1058 one that governs the function calls in these bodies. An
-- UNQUOTED `numeric` is a grammar keyword: gram.y's Numeric production emits
-- SystemTypeName("numeric"), i.e. an explicitly pg_catalog-qualified TypeName, so
-- the caller's search_path cannot reach it. Measured on PG 18.3 -- with a
-- `CREATE DOMAIN shadow.numeric AS text` first on the path, `x::numeric` still
-- resolves to pg_catalog.numeric while the QUOTED `x::"numeric"` resolves to the
-- shadow. So the prefix here is redundant, and it is written anyway for two
-- reasons: the body then reads under one rule rather than two, and the next
-- builder that reaches for a type name which is NOT a grammar keyword (any
-- extension type, any domain) IS path-resolved and must carry it for real.
--
-- Unchanged by this: the other five builders render text/int/bool/jsonb only, and
-- those output functions are genuinely immutable; bm25_boolean's to_jsonb(jsonb[])
-- never invokes a type output function at all.
CREATE FUNCTION bm25_boost(weight float8, query jsonb) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  AS $$ SELECT pg_catalog.jsonb_build_object('boost', pg_catalog.jsonb_build_object('weight', weight::pg_catalog.numeric, 'query', query)) $$;

-- jsonb overloads of @@@/&@@: same opclass/family, same strategy numbers as
-- the (text,text) pair above, RHS is a query tree from the builders instead
-- of a bare term string. Neither C anchor is inert (see src/bm25_handler.c).
-- bm25_match_jsonb FAILS LOUD: its body unconditionally raises ERROR
-- (FEATURE_NOT_SUPPORTED). It is reachable ONLY when the planner applies
-- "col @@@ jsonb" as a filter / recheck / seqscan qual, and a jsonb query tree
-- cannot be matched from one heap column value with no index handle, segments
-- or analyzer -- it used to return false there, silently dropping every row.
-- So a jsonb predicate has to be answered by the bm25 index scan, which
-- evaluates the tree via bm25_rescan_parse_jsonb / bm25_query_eval instead.
-- bm25_distance_jsonb is not inert either: it runs for real on every ranked
-- query, resolving the owning scored scan by identity exactly like
-- bm25_distance(text, text) above -- see its own note below.
-- COST 5000 mirrors bm25_match(text,text); see the derivation above that
-- function's declaration. It is not re-derived here because this anchor is
-- never actually evaluated (it errors), so the value only has to stay high
-- enough to keep the planner off a seqscan.
CREATE FUNCTION bm25_match_jsonb(text, jsonb) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE COST 5000;
CREATE OPERATOR @@@ (LEFTARG = text, RIGHTARG = jsonb, PROCEDURE = bm25_match_jsonb,
  RESTRICT = bm25_matchsel, JOIN = bm25_matchjoinsel);
-- Concurrency (R3): same identity-resolve contract as bm25_distance(text,text) above
-- (it is the identical per-row resjunk-projection function, just for the jsonb
-- RHS) -- does NOT fail loud under concurrent scored scans, degrades to +inf
-- off the index. See bm25_distance's comment for the full rationale.
-- STABLE PARALLEL RESTRICTED for the same reason as bm25_distance(text,text); see
-- the volatility note there.
CREATE FUNCTION bm25_distance_jsonb(text, jsonb) RETURNS float8
  AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
CREATE OPERATOR &@@ (LEFTARG = text, RIGHTARG = jsonb, PROCEDURE = bm25_distance_jsonb);
ALTER OPERATOR FAMILY text_bm25_ops USING bm25_native
  ADD OPERATOR 1 @@@ (text, jsonb),
      OPERATOR 2 &@@ (text, jsonb) FOR ORDER BY float_ops;

-- M6 Task 2: parse a jsonb query tree against an index's baked field config
-- and render it back as canonical text (fields resolved to ids). Development
-- and regression only -- proves the parser + its validation ERRORs without
-- needing a real executor scan.
CREATE FUNCTION bm25_debug_query_parse(index regclass, query jsonb) RETURNS text
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- M6 Task 2: parse + bm25_query_flatten a jsonb query tree, rendering one line
-- per leaf in leaf_bit order (boost = the folded product of enclosing boosts,
-- negated = whether the leaf sits under a must_not at any depth). Exercises
-- the DFS/boost-fold/negation-propagation logic in isolation. That logic is
-- live: bm25_query_flatten's production callers are both on the query path --
-- bm25_scan_build_ranking_exhaustive (bm25_scan_rank.c) and bm25_rescan_parse_jsonb
-- (bm25_scan.c). Only this SRF is development and regression surface.
CREATE FUNCTION bm25_debug_query_flatten(index regclass, query jsonb) RETURNS text
  AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- M6 Task 2: unit-check bm25_glob_match without a full wildcard scan fixture.
-- bm25_glob_match has a live production caller: the wildcard dictionary
-- expansion bm25_dict_expand_wildcard (bm25_seg_dict.c), reached on the query
-- path from bm25_scan_build_ranking_exhaustive (bm25_scan_rank.c). Only this SRF is
-- development and regression surface.
-- IMMUTABLE STRICT, in that keyword order: every other pure probe in this file
-- writes it that way and this one had it inverted (#148 SQL-13). The order is
-- semantically free; the point is that a family of near-identical declarations
-- should be greppable as one shape.
CREATE FUNCTION bm25_debug_glob_match(pattern text, term text) RETURNS bool
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- Cancellable-work counter (issue #156): read and reset the backend-local tally of
-- work steps performed on the interrupt-checked loops -- POST pages decoded, POS
-- pages crossed, dictionary entries replayed, pending pages drained, build page
-- rotations. sql/66_scan_interrupts and sql/80_maintenance_interrupts use the pair
-- to assert that a cancelled scan or seal stopped after a small FRACTION OF THE
-- WORK, replacing wall-clock boundaries that were properties of the CI runner
-- rather than of the code (ADR 0070). For development and regression testing only;
-- not part of the query path.
--
-- VOLATILE by omission, and that is the correct marking, not an oversight: both
-- read or write mutable backend-local state, so neither may be folded, cached or
-- shipped to a parallel worker. PARALLEL UNSAFE by omission for the same reason --
-- the counter is process-local, so a worker's increments would be invisible.
--
-- NOT STRICT-annotated because neither takes an argument (STRICT would be inert).
-- Both are REVOKEd from PUBLIC by the allowlist loop below, like every other
-- bm25_debug_* function, and neither is named in it.
CREATE FUNCTION bm25_debug_work_reset() RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION bm25_debug_work_units() RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C;

/* ---------------------------------------------------------------------------
 * In-database documentation for the public surface (#148 SQL-10).
 *
 * This file carries several hundred lines of explanatory prose and, until now,
 * not one COMMENT ON -- so every word of it was invisible from \df+, \dx+,
 * obj_description(), and any generated API reference. A user who installed the
 * extension from a package had no in-database description of bm25_snippet's
 * escape default (which DIVERGES from ts_headline), or of the fact that
 * bm25_score returns NULL rather than a wrong number when no scored scan owns
 * the row. For a 1.0 release that is a documentation gap, not tidiness.
 *
 * SCOPE = exactly the PUBLIC-executable surface: the fourteen C names the REVOKE
 * allowlist below names as deliberately public, plus the six LANGUAGE sql
 * builders (which that loop does not reach -- it is scoped to lanname = 'c'),
 * plus the operators, the opclass and the access method. Every bm25_debug_*
 * function is deliberately left uncommented: a comment is user-facing
 * documentation, and those are not user-facing. The C declarations above remain
 * the authority; these are one- or two-sentence distillations, and when the two
 * disagree the comment is the one that is wrong.
 * --------------------------------------------------------------------------- */
COMMENT ON ACCESS METHOD bm25_native IS
  'BM25 ranked full-text index access method: segment-based inverted index with block-max WAND top-k retrieval.';
COMMENT ON OPERATOR CLASS text_bm25_ops USING bm25_native IS
  'Default text opclass for bm25_native: strategy 1 = @@@ (match), strategy 2 = &@@ (order by relevance).';

COMMENT ON FUNCTION bm25_handler(internal) IS
  'Access method handler for bm25_native. Not callable from SQL; referenced only by CREATE ACCESS METHOD.';

COMMENT ON OPERATOR @@@ (text, text) IS
  'Does the left-hand document match the right-hand query string? Answered by the bm25_native index when one covers the column; there a bare query matches every field of a multi-column index and a field:term query only the named field. Applied as a filter instead (no usable index, an OR, a row-level security policy), it sees only the left-hand column: a bare query matches that column alone, and a field-scoped or phrase query raises feature_not_supported.';
COMMENT ON OPERATOR @@@ (text, jsonb) IS
  'Does the left-hand document match the right-hand jsonb query tree (built with bm25_term, bm25_boolean, ...)? Requires a bm25_native index: this operator cannot be evaluated as a filter and raises feature_not_supported if the planner tries.';
COMMENT ON OPERATOR &@@ (text, text) IS
  'Relevance distance (= -score) for ORDER BY. Ascending distance is descending relevance; WHERE col @@@ ''query'' ORDER BY col &@@ ''query'' LIMIT k uses the index ordering path with no Sort. The @@@ predicate is required: an ORDER BY alone does not use the index, and every row then gets +infinity.';
COMMENT ON OPERATOR &@@ (text, jsonb) IS
  'Relevance distance (= -score) for ORDER BY, with a jsonb query tree on the right-hand side. See the (text, text) operator.';

COMMENT ON FUNCTION bm25_match(text, text) IS
  'Seqscan anchor for @@@ (text, text). Off the index it re-analyzes both sides with the DEFAULT (english) analyzer, so on a non-english index the filter path can disagree with the index path about the same row; it matches the left-hand value only, where the index matches every field of a multi-column index; and it raises feature_not_supported for a phrase or a field:term scope, which only the index can evaluate.';
COMMENT ON FUNCTION bm25_match_jsonb(text, jsonb) IS
  'Anchor for @@@ (text, jsonb). Always raises feature_not_supported: a jsonb query tree cannot be evaluated from one heap value with no index handle, segments or analyzer, so a jsonb predicate must be answered by the index scan.';
COMMENT ON FUNCTION bm25_matchsel(internal, oid, internal, integer) IS
  'Restriction selectivity estimator for @@@. A blind constant; see docs/adr/0010-planner-estimates.md.';
COMMENT ON FUNCTION bm25_matchjoinsel(internal, oid, internal, smallint, internal) IS
  'Join selectivity estimator for @@@. A blind constant; see docs/adr/0010-planner-estimates.md.';

COMMENT ON FUNCTION bm25_distance(text, text) IS
  'Ordering-operator body for &@@: the current scored scan row''s negated BM25 score. Degrades to +infinity off the index (seqscan, or no active scored scan), which is not an error.';
COMMENT ON FUNCTION bm25_distance_jsonb(text, jsonb) IS
  'Ordering-operator body for &@@ with a jsonb query tree. Same contract as bm25_distance(text, text), except that when no scored scan ranks the query it still parses the tree, without resolving field names, and raises the parse error for an invalid one.';

COMMENT ON FUNCTION bm25_score(tid) IS
  'Positive BM25 score of the row identified by ctid, in a query whose bm25_native index scan is ranked (ORDER BY ... &@@ ...). Returns NULL when no active scored scan owns the row. Projected directly on its own scan''s emitted rows, it returns that scan''s score even when another live scan''s current row is the same row (a correlated subquery ranking the same table). It can return another concurrent scan''s score when such a collision cannot be attributed (e.g. a join on the key with the accessor above the join) and when the projection is decoupled from its scan (a PL/pgSQL FOR loop, a Sort or Materialize above the scan, a cursor). bm25_score(tid, query [, tableoid]) resolves those shapes.';
COMMENT ON FUNCTION bm25_score_key(int) IS
  'Positive BM25 score of the current scored scan''s row, resolved by the index''s key_field value instead of by ctid. Returns NULL when the index has no key_field, the key is not in the ranking, or no scored scan owns the row. Under several concurrent ranked scans it behaves as bm25_score(tid) does, including the cases where it can return another concurrent scan''s score.';
COMMENT ON FUNCTION bm25_score_key(bigint) IS
  'Positive BM25 score of the current scored scan''s row, resolved by the index''s key_field value instead of by ctid. Returns NULL when the index has no key_field, the key is not in the ranking, or no scored scan owns the row. Under several concurrent ranked scans it behaves as bm25_score(tid) does, including the cases where it can return another concurrent scan''s score.';
COMMENT ON FUNCTION bm25_score_key(uuid) IS
  'Positive BM25 score of the current scored scan''s row, resolved by the index''s key_field value instead of by ctid. Returns NULL when the index has no key_field, the key is not in the ranking, or no scored scan owns the row. Under several concurrent ranked scans it behaves as bm25_score(tid) does, including the cases where it can return another concurrent scan''s score.';
COMMENT ON FUNCTION bm25_score_key(text) IS
  'Positive BM25 score of the current scored scan''s row, resolved by the index''s key_field value instead of by ctid. Returns NULL when the index has no key_field, the key is not in the ranking, or no scored scan owns the row. Under several concurrent ranked scans it behaves as bm25_score(tid) does, including the cases where it can return another concurrent scan''s score.';
COMMENT ON FUNCTION bm25_score(tid, text) IS
  'Positive BM25 score of the row identified by ctid under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score(tid) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor; pass tableoid as well when the ctid can repeat across tables (inheritance children, partitions). Returns NULL when no scan ranking that query holds the row, or when two such scans hold it with different scores.';
COMMENT ON FUNCTION bm25_score(tid, jsonb) IS
  'Positive BM25 score of the row identified by ctid under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score(tid) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor; pass tableoid as well when the ctid can repeat across tables (inheritance children, partitions). Returns NULL when no scan ranking that query holds the row, or when two such scans hold it with different scores.';
COMMENT ON FUNCTION bm25_score(tid, text, regclass) IS
  'Positive BM25 score of the row identified by ctid under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument, reading the heap named by the third argument (pass tableoid). Unlike bm25_score(tid) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor, and a ctid that repeats across inheritance children or partitions. Returns NULL when no scan ranking that query and heap holds the row, or when two such scans hold it with different scores.';
COMMENT ON FUNCTION bm25_score(tid, jsonb, regclass) IS
  'Positive BM25 score of the row identified by ctid under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument, reading the heap named by the third argument (pass tableoid). Unlike bm25_score(tid) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor, and a ctid that repeats across inheritance children or partitions. Returns NULL when no scan ranking that query and heap holds the row, or when two such scans hold it with different scores.';
COMMENT ON FUNCTION bm25_score_key(int, text) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(bigint, text) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(uuid, text) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(text, text) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(int, jsonb) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(bigint, jsonb) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(uuid, jsonb) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_score_key(text, jsonb) IS
  'Positive BM25 score of the row with this key_field value under the ranked scan whose ORDER BY ... &@@ query is byte-identical to the second argument. Unlike bm25_score_key(key) it names the scan instead of inferring it, so it stays correct for a join on the key, a PL/pgSQL FOR loop, a Sort or Materialize above the scan, and a cursor. Returns NULL when no scan ranking that query holds the key, or when the key belongs to two ranked rows with different scores (a non-unique key_field, or the same key in two tables: use bm25_score(ctid, query, tableoid)).';
COMMENT ON FUNCTION bm25_snippet(text, text, text, int, boolean) IS
  'A <mark>-wrapped excerpt of the passed field value around the current scored scan''s query terms. max_num_chars counts CHARACTERS of the source text; tags and ellipses are outside that budget. escape => true (the default) HTML-escapes & < > " '' in the field text -- this DIVERGES from ts_headline, which escapes nothing -- and never escapes the tags, so pass only trusted tags. Every query leaf outside a must_not highlights, including phrase and wildcard leaves. Returns NULL on zero hits, a NULL field, a NULL max_num_chars, or no active scored scan.';

COMMENT ON FUNCTION bm25_stats(regclass) IS
  'Corpus statistics for a bm25_native index: document and length totals, segment and pending counts, k1/b, sealed_avgdl (average document length over SEALED segments only -- not the scorer''s avgdl, which also includes the pending list), analyzer fingerprint, and format-negotiation fields. Requires SELECT on the indexed table.';
COMMENT ON FUNCTION bm25_seal(regclass) IS
  'Force an immediate seal: drain the pending list into a fresh immutable segment, regardless of bm25_native.seal_threshold. Waits for any VACUUM of the index to finish its dead-row pass, and inserts queue behind the wait (a page lock on the metapage, block 0, in pg_locks); do not run it concurrently with VACUUM on a write-heavy table. Index owner only.';
COMMENT ON FUNCTION bm25_merge(regclass) IS
  'Force a drain plus a merge down toward the target segment count, reclaiming pages. Waits for any VACUUM of the index to finish its dead-row pass, and inserts queue behind the wait (a page lock on the metapage, block 0, in pg_locks); do not run it concurrently with VACUUM on a write-heavy table. Index owner only.';
COMMENT ON FUNCTION bm25_upgrade(regclass) IS
  'Online, no-REINDEX format upgrade: seal pending, then re-stamp the metapage to this build''s format if the index is behind. A no-op NOTICE when already current. Waits for any VACUUM of the index to finish its dead-row pass, and inserts queue behind the wait (a page lock on the metapage, block 0, in pg_locks); do not run it concurrently with VACUUM on a write-heavy table. Index owner only.';

COMMENT ON FUNCTION bm25_match_terms(text, text) IS
  'Query-tree builder: a node matching the analyzed OR-of-terms of `terms` in `field`. STRICT: a NULL argument yields NULL; for all fields use raw jsonb without "field", e.g. ''{"match": {"terms": "red car"}}''::jsonb.';
COMMENT ON FUNCTION bm25_term(text, text) IS
  'Query-tree builder: a node matching `value` as a single analyzed term in `field`. STRICT: a NULL argument yields NULL; for all fields use raw jsonb without "field", e.g. ''{"term": {"value": "red"}}''::jsonb.';
COMMENT ON FUNCTION bm25_phrase(text, text, int, boolean) IS
  'Query-tree builder: a node matching `phrase` as a phrase in `field`. slop allows up to that many intervening tokens; ordered requires left-to-right order. STRICT: a NULL argument yields NULL; for all fields use raw jsonb without "field", e.g. ''{"phrase": {"phrase": "red car"}}''::jsonb.';
COMMENT ON FUNCTION bm25_wildcard(text, text) IS
  'Query-tree builder: a node matching a prefix wildcard such as ''judg*'' in `field`. STRICT: a NULL argument yields NULL; for all fields use raw jsonb without "field", e.g. ''{"wildcard": {"pattern": "judg*"}}''::jsonb.';
COMMENT ON FUNCTION bm25_boolean(jsonb[], jsonb[], jsonb[]) IS
  'Query-tree builder: a node requiring all of `must`, rewarding any of `should`, and excluding anything matching `must_not`. Each argument defaults to the empty array, so partial calls still yield a well-formed node.';
COMMENT ON FUNCTION bm25_boost(float8, jsonb) IS
  'Query-tree builder: multiplies the enclosed sub-query''s contribution by `weight`. The product of the weights enclosing each scoring leaf must lie in [1e-6, 1e6]; leaves under must_not are exempt. The weight is recorded as a numeric, so it is rounded to 15 significant decimal digits and is independent of extra_float_digits.';

/* ---------------------------------------------------------------------------
 * Lock down the debug surface.
 *
 * Every bm25_debug_* function is TEST-ONLY -- none is on the query path -- yet
 * PostgreSQL grants EXECUTE on a new function to PUBLIC by default, so all of
 * them shipped callable by any user in the database. The mutating ones
 * (bm25_debug_stamp_version, bm25_debug_write_optional_region,
 * bm25_debug_alloc_unknown_page, bm25_debug_pending_append) take a regclass and
 * write pages under a Generic WAL window; index_open() checks relkind and neither
 * ownership nor access method, so an unprivileged user could WAL-log a write onto
 * block 0 of ANY index in the database -- durable, and replayed on every standby.
 *
 * The mutating ones route through bm25_index_open_owned (ownership + AM identity,
 * checked before any page is touched; the function was called bm25_debug_open_index
 * before ADR 0029 renamed it). This REVOKE is the second, independent layer, and it
 * covers the read-only introspection functions too: they expose another user's index
 * contents.
 *
 * ALLOWLIST, NOT PREFIX MATCH (SQL-03). This loop used to select
 * `proname LIKE 'bm25\_debug\_%'`, which silently excused every debug function that
 * does not carry the prefix -- and one does not: bm25_wand_stats is a debug probe by
 * its own header comment and its home in src/bm25_debug.c, and it shipped
 * PUBLIC-executable for exactly that reason. Worse, sql/63_debug_privileges asserted
 * the result using a character-for-character copy of the same predicate, so the test
 * could only ever confirm the predicate was self-consistent, never that it was the
 * right predicate.
 *
 * Inverted, the default flips: every C function matching bm25% in this schema is
 * revoked unless it is named below as deliberately public. A new debug function is
 * covered whether or not whoever adds it thinks about privileges; a new PUBLIC one has
 * to be added to this list, which is a visible, reviewable act.
 *
 * NOTE THE RESIDUAL, because it is the same shape as the bug this replaces, one notch
 * out: the selection still keys on a NAME PREFIX (bm25%), so a C function this
 * extension installs under some other name would escape the loop, the pinned list in
 * sql/63_debug_privileges, and that suite's sanity floor -- all three. Every function
 * installed today matches, and the honest closure is to key on pg_depend extension
 * membership rather than on a name; that is a larger change than this fix and is not
 * made here.
 *
 * The count of covered functions is deliberately NOT stated here. The previous comment
 * claimed 46 and the true figure has been 61, 71 and 77 at various points since --
 * a number in a comment beside a loop that computes it is a maintenance liability with
 * no upside. sql/63_debug_privileges pins the actual public surface by NAME instead.
 *
 * Scoped to C functions in the install schema. Note this now cuts the other way from
 * the comment it replaces: while the predicate was bm25_debug_%, an unrelated function
 * had to share that prefix to be caught; now any unrelated C function named bm25%
 * co-installed in this schema has its PUBLIC grant revoked at CREATE EXTENSION time.
 * That is the cost of defaulting to closed, and it is the right side to err on.
 * --------------------------------------------------------------------------- */
DO $bm25_revoke$
DECLARE
  fn record;
BEGIN
  FOR fn IN
    SELECT p.oid::regprocedure AS sig
      FROM pg_proc p
      JOIN pg_namespace n ON n.oid = p.pronamespace
      JOIN pg_language l ON l.oid = p.prolang
     WHERE n.nspname = current_schema()
       AND l.lanname = 'c'
       AND p.proname LIKE 'bm25%'
       /* Deliberately PUBLIC: operator/AM anchors, query-path functions, and the
        * documented maintenance and reporting entry points. Name-keyed, so
        * one entry covers every bm25_score and bm25_score_key overload. Anything not here is
        * revoked -- including any bm25_debug_* function, and including
        * bm25_wand_stats, which is why this list exists. */
       AND p.proname NOT IN ('bm25_handler',
                             'bm25_match', 'bm25_match_jsonb',
                             'bm25_matchsel', 'bm25_matchjoinsel',
                             'bm25_distance', 'bm25_distance_jsonb',
                             'bm25_score', 'bm25_score_key', 'bm25_snippet',
                             'bm25_stats',
                             'bm25_seal', 'bm25_merge', 'bm25_upgrade')
  LOOP
    EXECUTE format('REVOKE ALL ON FUNCTION %s FROM PUBLIC', fn.sig);
  END LOOP;
END
$bm25_revoke$;
