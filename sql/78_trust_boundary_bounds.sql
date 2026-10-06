-- Trust-boundary review (2026-08): extends the H6 decode-boundary principle
-- (ADR 0027) to the on-disk trust boundaries H6 deliberately scoped OUT of.
-- H6 bounded the SEGMENT HEADER's field_count and the posting-block decode;
-- this fix bounds:
--   * the METAPAGE's own field_count (bm25_meta_validate) -- a DIFFERENT
--     struct from the segment header H6 covered, read on every scan-start
--     snapshot and every bm25_meta_read;
--   * the DICT entry decode (bm25_dictentry_validate) at the two remaining
--     walkers that duplicated H6's unchecked pattern -- five walkers share it
--     now, not three;
--   * the KEYMAP's key_type/key_size PAIR (bm25_seg_keymeta) and the per-key
--     page-span + pagebytes-underflow bounds in the decode loop
--     (bm25_seg_key);
--   * the accumulator's key_size magnitude (bm25_accum_set_keymeta) and the
--     key-width-change guard (bm25_accum_set_doc_key, upgraded from an
--     Assert -- compiled out in exactly the build where it matters -- to a
--     real ereport);
--   * the merge idmap docid bound (bm25_merge.c);
--   * the retired-list page-kind and entry-count bounds (bm25_fsm.c);
--   * strnlen bounds on field_name/stemmer_name (bm25_analyzer.c);
--   * (H6, 2026-08) the PAGE KIND every page carries and no reader tested: the
--     pending chain's (bm25_pending.c), bm25_pending_truncate's cycle cap, and
--     every segment chain's (bm25_seg_page_validate_kind, bm25_seg_read.c).
--
-- bm25_seg_page_validate did NOT cover any of this: it compared the page
-- opaque's seg_gen and nothing else, so a torn page, a bit flip, or a hostile
-- page image in a restored data directory passes it carrying arbitrary bytes
-- everywhere else on the page. It still does not cover the value decodes above;
-- it now covers the page KIND as well as the gen (bm25_seg_page_validate is the
-- want_kind == 0 wrapper), which is a different axis: gen tells you WHICH
-- segment a page belongs to and cannot tell you WHICH CHAIN inside it, because
-- every page of one segment carries the same gen.
--
-- Same reasoning as 69_decode_boundary: "a regression suite cannot produce a
-- corrupt page", so most of what follows runs the SAME validators the fixed
-- code paths call -- extracted to their own functions where the check was
-- previously inline, exactly as bm25_dictentry_validate was already extracted
-- for H6 -- over caller-supplied values. Two validators ARE reachable through
-- real DDL/DML without any page corruption (the key-width-change guard, and
-- the metapage field_count is NOT -- see the two sections below), and those
-- run as real SQL against a real index instead of a probe. The positive
-- direction (guards don't reject what the writer emits) is covered by the
-- other 78 suites, every one of which decodes real pages through these paths;
-- a short positive control closes this suite too.
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------------ metapage field_count
-- ADR 0027 bounded the SEGMENT HEADER's field_count; the METAPAGE carries its
-- OWN, separate field_count (BM25MetaPageData, a different struct), read by
-- bm25_meta_read and bm25_scan_snapshot on every query. It is fixed by the
-- index's own column list at CREATE INDEX time and stays well inside
-- BM25_MAX_FIELDS by construction, so unlike most validators below there is no
-- real DDL path to a hostile value either -- this probe drives
-- bm25_meta_validate itself over a fully-formed, otherwise-valid metapage
-- struct (correct magic/format_version/min_read_version) so the version gates
-- that run first never mask the check under test.
SELECT bm25_debug_meta_field_count_validate(1)  AS fc_min;
SELECT bm25_debug_meta_field_count_validate(32) AS fc_max;      -- BM25_MAX_FIELDS
SELECT bm25_debug_meta_field_count_validate(0);                 -- one below the floor
SELECT bm25_debug_meta_field_count_validate(33);                -- one past BM25_MAX_FIELDS
SELECT bm25_debug_meta_field_count_validate(65535);              -- nowhere close

-- ------------------------------------------------------------ DICT entry decode
-- H6 (ADR 0027) bounded the DICT entry decode in the segment's dict LOOKUP and
-- ITERATOR via bm25_dictentry_validate. This fix threads the SAME call through
-- the three remaining walkers that duplicated the identical unchecked
-- MAXALIGN(sizeof+termlen) pattern: the whole-dictionary scan behind
-- bm25_debug_segterms, and bm25_debug_terms / bm25_debug_postings
-- (bm25_segment.c) -- five walkers in total, not three. The last two back
-- debug SRFs that are deliberately gated on table SELECT only (H13), not AM
-- identity beyond that, so an unprivileged reader who can already SELECT the
-- indexed table could otherwise aim a corrupt-termlen page at them.
--
-- termlen fills a REAL BM25DictEntry struct on the C stack (a field, not a raw
-- bytea -- host-endian/padding independent, same reasoning as
-- bm25_debug_block_validate's header-by-fields approach); avail stands in for
-- the page bytes left from this entry onward (end - cur). On this platform
-- sizeof(BM25DictEntry) is 20 bytes and MAXALIGN rounds to a multiple of 8, so
-- a termlen-0 entry's total on-page length is MAXALIGN(20) = 24, not 20 --
-- the header can exactly fit while the (padded) entry still does not.
SELECT bm25_debug_dictentry_validate(0, 24)  AS termlen0_exact_fit;   -- MAXALIGN(20+0)=24
SELECT bm25_debug_dictentry_validate(5, 32)  AS termlen5_exact_fit;   -- MAXALIGN(20+5)=32
SELECT bm25_debug_dictentry_validate(0, 19);   -- one byte short of the RAW header (20)
SELECT bm25_debug_dictentry_validate(0, 20);   -- header exactly fits, but MAXALIGN(20)=24 does not
SELECT bm25_debug_dictentry_validate(0, 23);   -- one byte short of the MAXALIGN'd entry
SELECT bm25_debug_dictentry_validate(5, 31);   -- one byte short of MAXALIGN(20+5)=32
SELECT bm25_debug_dictentry_validate(70000, 1000); -- termlen out of uint16 range

-- ------------------------------------------------------------ keymap key_type/key_size
-- bm25_seg_keymeta is the ONE place every caller (merge, bm25_debug_seg_keymap,
-- and the scan-start key discovery in bm25_scan_rank.c/bm25_wand.c) learns a
-- segment's key config, unconditionally, against the FIRST keyed segment
-- header -- not gated on any docid actually being ranked. key_type=0
-- (BM25_KEY_NONE) is itself rejected here: a segment with no KEYMAP chain
-- never reaches this check (bm25_seg_keymeta returns false first), so a
-- KEYMAP header claiming NONE is already a contradiction.
SELECT bm25_debug_keymeta_validate(1, 4);    -- BM25_KEY_INT4 / 4
SELECT bm25_debug_keymeta_validate(2, 8);    -- BM25_KEY_INT8 / 8
SELECT bm25_debug_keymeta_validate(3, 16);   -- BM25_KEY_UUID / 16
SELECT bm25_debug_keymeta_validate(4, 16);   -- BM25_KEY_TEXT / 16
SELECT bm25_debug_keymeta_validate(0, 4);    -- BM25_KEY_NONE: unknown to this check
SELECT bm25_debug_keymeta_validate(5, 16);   -- past BM25_KEY_TEXT: unrecognized type
SELECT bm25_debug_keymeta_validate(1, 8);    -- INT4 with INT8's width
SELECT bm25_debug_keymeta_validate(3, 15);   -- UUID one byte narrower than 16
SELECT bm25_debug_keymeta_validate(4, 17);   -- TEXT one byte wider than BM25_KEY_MAX_SIZE

-- ------------------------------------------------------------ keymap per-key bounds
-- bm25_seg_key's root-page header step (key_size width + the pagebytes-
-- underflow guard) followed by the per-page span check, run in that order.
-- sizeof(BM25KeymapHeader) is 8 bytes on this platform; `pagebytes` below is
-- the notional page's TOTAL content bytes (pd_lower - page header), matching
-- what bm25_seg_key has BEFORE subtracting the header.
SELECT bm25_debug_seg_key_bounds(4, 48, 0)   AS well_formed_int4;   -- 40 bytes of key[] after the header
SELECT bm25_debug_seg_key_bounds(16, 24, 0)  AS well_formed_max_width;  -- key_size at BM25_KEY_MAX_SIZE
SELECT bm25_debug_seg_key_bounds(0, 48, 0);    -- key_size 0
SELECT bm25_debug_seg_key_bounds(16, 48, 0)  AS key_size_exactly_at_limit;
SELECT bm25_debug_seg_key_bounds(17, 48, 0);   -- key_size one past BM25_KEY_MAX_SIZE
-- Header AND span both exactly fit: 8-byte header + exactly one 4-byte key.
SELECT bm25_debug_seg_key_bounds(4, 12, 0)   AS header_and_span_exactly_fit;
-- The arithmetic-underflow case: pagebytes is a uint32 only lower-bounded by
-- pd_lower >= SizeOfPageHeaderData, so a header-sane corrupt page can still
-- carry pagebytes < sizeof(hdr); without the guard, pagebytes -= sizeof(hdr)
-- would wrap to just under UINT32_MAX instead of erroring.
SELECT bm25_debug_seg_key_bounds(4, 8, 0);     -- header exactly fits; ZERO bytes left for any key
SELECT bm25_debug_seg_key_bounds(4, 7, 0);     -- one byte SHORT of the header: underflow guard fires
SELECT bm25_debug_seg_key_bounds(4, 0, 0);     -- pagebytes 0: underflow guard fires
-- Per-key span: pagebytes 18 => 10 bytes of key[] after the header (room for
-- two whole 4-byte keys plus 2 bytes of trailing slack, exactly like the
-- writer leaves on a real page -- see bm25_keymap_write's "whole-key spans").
SELECT bm25_debug_seg_key_bounds(4, 18, 6)   AS span_exactly_at_limit;  -- 6+4 == 10
SELECT bm25_debug_seg_key_bounds(4, 18, 7);    -- 7+4 == 11 > 10: one past

-- ------------------------------------------------------------ accumulator key_size
-- bm25_accum_set_keymeta's key_size bound is a MAGNITUDE check only, gated on
-- key_type != BM25_KEY_NONE (bm25_pending_drain's "idempotent-guarded on
-- key_type so a keyless index never allocates" comment) -- it does NOT check
-- key_type/key_size CORRESPONDENCE the way bm25_seg_keymeta_validate above
-- does, so a key_type/key_size pair that would fail THAT check can still pass
-- THIS one; the two exist for different reasons (this one sizes an
-- allocation, that one gates a decode).
SELECT bm25_debug_accum_set_keymeta(0, 0)     AS none_key_zero_size;    -- NONE: check skipped entirely
SELECT bm25_debug_accum_set_keymeta(0, 9999)  AS none_key_any_size;     -- NONE: still skipped, however large
SELECT bm25_debug_accum_set_keymeta(1, 1)     AS int4_undersized_but_in_bound; -- magnitude-only: not type-checked here
SELECT bm25_debug_accum_set_keymeta(1, 16)    AS key_size_exactly_at_limit;
SELECT bm25_debug_accum_set_keymeta(1, 0);     -- key_size 0 with a real key_type
SELECT bm25_debug_accum_set_keymeta(1, 17);    -- one past BM25_KEY_MAX_SIZE

-- ------------------------------------------------------------ merge idmap docid bound
-- old_docid is decoded off a source segment's POST pages; idmap[] is
-- allocated exactly ndocs wide, so the valid range is [0, ndocs).
SELECT bm25_debug_merge_docid_validate(0, 5) AS min_valid;
SELECT bm25_debug_merge_docid_validate(4, 5) AS max_valid;      -- ndocs - 1
SELECT bm25_debug_merge_docid_validate(5, 5);  -- == ndocs: one past
SELECT bm25_debug_merge_docid_validate(6, 5);  -- past ndocs
SELECT bm25_debug_merge_docid_validate(0, 0);  -- degenerate empty segment: no valid docid at all

-- ------------------------------------------------------------ retired-list page bounds
-- bm25_reclaim_retired's two checks, in the SAME order it runs them: page-kind
-- first (flags & BM25_PAGE_RETIRED), then the entry-count ceiling
-- (BM25_RETIRED_PER_PAGE, the construction-time BLCKSZ/sizeof(entry) constant
-- -- 169 on this platform/block size). flags is a bitmask test, so bits
-- alongside BM25_PAGE_RETIRED (256) do not matter.
SELECT bm25_debug_retired_page_bounds(256, 0)   AS zero_entries;
SELECT bm25_debug_retired_page_bounds(256, 169) AS entries_exactly_at_limit;
SELECT bm25_debug_retired_page_bounds(256 + 2, 1) AS retired_bit_plus_other_bits;
SELECT bm25_debug_retired_page_bounds(0, 0);      -- missing BM25_PAGE_RETIRED entirely
SELECT bm25_debug_retired_page_bounds(256, 170);  -- one past BM25_RETIRED_PER_PAGE
-- Order matters: a page that fails BOTH checks reports the page-kind error,
-- not the count error -- matching bm25_reclaim_retired's own check order.
SELECT bm25_debug_retired_page_bounds(0, 99999);

-- ------------------------------------------------------------ pending page kind
-- H6: bm25_page_init stamps a BM25_PAGE_* kind on every page and, before this
-- fix, no reader on the pending path tested it. bm25_pending_iter_begin is the
-- ONE canonical entry point for all eleven pending walkers, so the gate its peer
-- chain families already carried (bm25_retired_page_flags_validate guards all
-- three retired walkers) now lives there -- and in bm25_pending_truncate, the
-- only chain walker in the extension that DESTROYS pages.
--
-- Bit values are the raw BM25_PAGE_* masks: PENDING 2, DICT 8, POST 16,
-- SEGCAT 4, DELETED 512.
-- The rejections below go through a plpgsql wrapper rather than a bare SELECT,
-- for a reason specific to this fix: WHICH errcode is raised is the load-bearing
-- part. \set VERBOSITY terse prints the message and hides the SQLSTATE, and the
-- whole point of these two guards is that they raise XX002
-- (ERRCODE_INDEX_CORRUPTED) and NOT 40001
-- (ERRCODE_T_R_SERIALIZATION_FAILURE) -- which bm25_scan_build_ranking CATCHES
-- and silently retries three times with a fresh snapshot. A kind mismatch is not
-- retryable and must propagate. The regexp_replace is this file's established
-- block-number normalisation, kept for uniformity even though these two probes
-- pin the block themselves.
CREATE FUNCTION pk_probe(sql text) RETURNS text AS $$
BEGIN
    EXECUTE sql;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, 'block [0-9]+', 'block N');
END; $$ LANGUAGE plpgsql;

SELECT bm25_debug_pending_page_flags_validate(2) AS pending_accepted;
-- DELETED is ORed onto a live kind by bm25_page_mark_deleted, never assigned, so
-- a tombstoned pending page must still read as a pending page. This is exactly
-- why the check is `flags & PENDING`, not equality -- and exactly why it is NOT
-- a cycle guard (see the cap probe below).
SELECT bm25_debug_pending_page_flags_validate(2 + 512) AS pending_plus_deleted_accepted;
-- Every other kind is rejected. These are the pages a stray nextblk actually
-- lands on: a DICT or POST page's bytes frequently look like a valid
-- BM25PendingDocHeader, which is what made the old failure mode silent.
SELECT pk_probe('SELECT bm25_debug_pending_page_flags_validate(8)')   AS dict_rejected;
SELECT pk_probe('SELECT bm25_debug_pending_page_flags_validate(16)')  AS post_rejected;
SELECT pk_probe('SELECT bm25_debug_pending_page_flags_validate(4)')   AS segcat_rejected;
SELECT pk_probe('SELECT bm25_debug_pending_page_flags_validate(0)')   AS no_kind_rejected;
-- DELETED with no live kind underneath: not a pending page either.
SELECT pk_probe('SELECT bm25_debug_pending_page_flags_validate(512)') AS deleted_only_rejected;

-- --------------------------------------------------- pending truncate cycle cap
-- bm25_pending_truncate had only a `blk < nblocks` extent bound; an in-extent
-- cycle re-stamped and re-recorded the same pages forever (cancellable via
-- CHECK_FOR_INTERRUPTS, but non-terminating). The guard is a visited-COUNT cap,
-- not a reachable[] bitmap: this runs on aminsert once per seal, where a
-- palloc0(nblocks) would be a ~13 MB allocation per seal on a 100 GB index to
-- guard a walk of a few hundred pages. nblocks is a strict upper bound on the
-- DISTINCT blocks the walk can legally visit, so reaching it proves a repeat.
--
-- The cap is extracted and driven directly here, so its arithmetic is pinned
-- without building a corrupt chain (bm25_debug_stamp_chain_next's 'pending'
-- mode, sql/113, can forge a pending nextblk when a real cycle is wanted).
SELECT bm25_debug_pending_cycle_cap_validate(0, 100)  AS first_page_ok;
SELECT bm25_debug_pending_cycle_cap_validate(99, 100) AS last_legal_page_ok;
SELECT pk_probe('SELECT bm25_debug_pending_cycle_cap_validate(100, 100)')
         AS visited_equals_extent;
SELECT pk_probe('SELECT bm25_debug_pending_cycle_cap_validate(101, 100)')
         AS visited_past_extent;
-- A zero extent admits nothing: the walk's own `blk < nblocks` bound means it
-- never enters the loop body on an empty relation, so reaching the cap there is
-- itself a corrupt state.
SELECT pk_probe('SELECT bm25_debug_pending_cycle_cap_validate(0, 0)')
         AS empty_extent_admits_nothing;

-- ------------------------------------------------------------ segment page kind
-- SEGREAD-06: seg_gen cannot distinguish chains WITHIN one segment. Every page
-- of a segment -- DICT, POST, NORMS, LIVE, DOCMAP, KEYMAP, POS and the header --
-- carries the SAME seg_gen, so a corrupt BM25SegmentHeader.dict_root aimed at
-- that segment's own NORMS chain passed gen validation and was decoded as
-- BM25DictEntry records. bm25_dictentry_validate kept that memory-safe; the
-- result was a silently wrong dictionary rather than the loud
-- ERRCODE_INDEX_CORRUPTED the rest of the trust boundary promises.
--
-- The kind mismatch raises ERRCODE_INDEX_CORRUPTED and NOT
-- ERRCODE_T_R_SERIALIZATION_FAILURE, deliberately: bm25_scan_build_ranking
-- CATCHES the latter and retries the scan three times with a fresh snapshot,
-- which is right for a concurrently reclaimed segment and wrong for a wrong-kind
-- page. A kind mismatch is not retryable and must propagate. (In the validator,
-- the gen check therefore runs FIRST, so a page that fails both -- a reclaimed
-- page already re-initialized as another kind -- still reports the retryable
-- error and the retry still works.)
--
-- Bit values as above, plus NORMS 32, LIVE 64, DOCMAP 128, KEYMAP 2048,
-- POS 4096.
SELECT bm25_debug_seg_page_kind_validate(8, 8)     AS dict_accepted;
SELECT bm25_debug_seg_page_kind_validate(16, 16)   AS post_accepted;
SELECT bm25_debug_seg_page_kind_validate(32, 32)   AS norms_accepted;
SELECT bm25_debug_seg_page_kind_validate(64, 64)   AS live_accepted;
SELECT bm25_debug_seg_page_kind_validate(128, 128) AS docmap_accepted;
SELECT bm25_debug_seg_page_kind_validate(2048, 2048) AS keymap_accepted;
SELECT bm25_debug_seg_page_kind_validate(4096, 4096) AS pos_accepted;
SELECT bm25_debug_seg_page_kind_validate(4, 4)     AS segcat_accepted;
-- Same DELETED-is-ORed reasoning as the pending probe above.
SELECT bm25_debug_seg_page_kind_validate(8 + 512, 8) AS dict_plus_deleted_accepted;
SELECT bm25_debug_seg_page_kind_validate(32 + 512, 32) AS norms_plus_deleted_accepted;
-- want_kind == 0 is the pass-through the bm25_seg_page_validate wrapper uses:
-- it accepts anything, including a page with no kind bits at all.
SELECT bm25_debug_seg_page_kind_validate(8, 0) AS want_kind_zero_passthrough;
SELECT bm25_debug_seg_page_kind_validate(0, 0) AS want_kind_zero_accepts_unstamped;
-- The SEGREAD-06 shape itself, both directions: a chain expecting one kind that
-- reaches another kind FROM THE SAME SEGMENT (same seg_gen, so gen validation
-- passes and only this check stands).
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(32, 8)')    AS norms_as_dict;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(8, 32)')    AS dict_as_norms;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(128, 64)')  AS docmap_as_live;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(64, 128)')  AS live_as_docmap;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(16, 4096)') AS post_as_pos;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(2, 8)')     AS pending_as_dict;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(0, 8)')     AS unstamped_as_dict;
-- The segment HEADER page is bm25_page_init(pg, BM25_PAGE_SEGCAT): there is no
-- separate BM25_PAGE_SEGHDR bit, and adding one would need a
-- format-version/upgrade story (ADR 0009). Header and catalog pages therefore
-- share the SEGCAT bit and stay indistinguishable HERE -- documented and
-- covered, not unchecked: the gen half separates them, because the builder
-- stamps the header with seg_gen = gen while catalog pages keep seg_gen = 0 and
-- gens are always >= 1. Every OTHER kind is still ruled out at the header sites.
SELECT bm25_debug_seg_page_kind_validate(4, 4)  AS header_and_catalog_share_segcat;
SELECT pk_probe('SELECT bm25_debug_seg_page_kind_validate(8, 4)') AS dict_as_header;

DROP FUNCTION pk_probe(text);

-- ------------------------------------------------------------ field_name/stemmer_name
-- field_name/stemmer_name are fixed-width NUL-padded arrays, NUL-terminated
-- only by WRITER convention. Unlike the validators above, this fix does not
-- reject anything -- strnlen just caps the scan at the array's own declared
-- size, so the worst a corrupt page can do is report the full width verbatim.
-- Well-formed: a short NUL-padded name reads back exactly its content.
SELECT bm25_debug_bounded_name_bytes(
         convert_to('alpha', 'UTF8') || decode(repeat('00', 64 - 5), 'hex'), 64)
         AS short_name;
-- Hostile: BM25_FIELD_NAME_LEN (64) bytes with NO NUL anywhere. The reader
-- must return all 64 bytes, never scan past them looking for a NUL that
-- is not there.
SELECT length(bm25_debug_bounded_name_bytes(decode(repeat('61', 64), 'hex'), 64))
         AS capped_at_field_name_len;
-- Same defense, BM25_STEMMER_NAME_LEN (32).
SELECT length(bm25_debug_bounded_name_bytes(decode(repeat('62', 32), 'hex'), 32))
         AS capped_at_stemmer_name_len;
-- The probe's own defensive check (production always hands it a buffer of
-- exactly the declared width): a shorter input than maxlen is refused rather
-- than read past.
SELECT bm25_debug_bounded_name_bytes('\x616263'::bytea, 64);

-- ------------------------------------------------------------ key-width change (real DDL)
-- bm25_accum_set_doc_key's key-width-change guard is DDL-reachable, not
-- on-disk corruption: bm25_resolve_fields re-resolves the key_field reloption
-- on EVERY aminsert, so an ALTER INDEX ... SET (key_field=...) between inserts
-- feeds mixed-width keys into the SAME accumulator when they later drain
-- together. Was an Assert-only guard (compiled out in a non-cassert build);
-- now a real ereport with a REINDEX hint rather than a generic corruption
-- error, because this is an ordinary reloption change the catalog accepts
-- with no cross-check against pending writes already queued under the old
-- key_field.
--
-- #292: an index this binary builds carries a key-identity stamp, so the second
-- INSERT below is refused before it reaches the chain. Only an UNSTAMPED index (one a
-- pre-#292 binary built, emulated here by clearing the stamp) with no segment yet can
-- still queue the mixed chain, and the drain now refuses it as a whole -- type and
-- width -- before bm25_accum_set_doc_key's width guard is reached; that guard remains
-- the backstop for any other producer of mixed widths.
CREATE TABLE kfw (id int, uid uuid, body text);
CREATE INDEX kfw_bm25 ON kfw USING bm25_native (body)
  INCLUDE (id, uid) WITH (key_field = 'id');
SELECT bm25_debug_clear_keystamp('kfw_bm25') AS cleared;
-- Pending doc #1: key_type=INT4 (4 bytes), resolved under key_field='id'.
INSERT INTO kfw VALUES (1, '11111111-1111-1111-1111-111111111111', 'alpha one');
ALTER INDEX kfw_bm25 SET (key_field = 'uid');
-- Pending doc #2: key_type=UUID (16 bytes), resolved under key_field='uid'.
INSERT INTO kfw VALUES (2, '22222222-2222-2222-2222-222222222222', 'alpha two');
-- Draining both pending docs in the same pass hits the mismatch.
SELECT bm25_seal('kfw_bm25');
DROP TABLE kfw;

-- ------------------------------------- merge-replay field_id (ADR 0040 sweep)
-- bm25_field_rle_decode bounds a decoded field id to BM25_MAX_FIELDS, which is right
-- for the scorer's BM25_MAX_FIELDS-wide arrays -- but the merge accumulator's
-- AccumDoc.doclen_by_field is palloc0'd only field_count wide. So an id in
-- [field_count, BM25_MAX_FIELDS) passes the decode and then indexes past the end of
-- the per-doc arrays: the POST pass reads doclen_by_field[fid] and the POS pass
-- reads store_pos[fid] out of bounds, baking garbage doclens into the merged
-- segment's norms. Both entry points were guarded only by an Assert, i.e. by
-- nothing in the production builds where a corrupt segment actually shows up.
--
-- Enumerated per ADR 0040 rather than fixed only where the issue named it: the
-- three OTHER Assert(field_id < field_count) sites in bm25_accum.c take
-- caller-controlled values (the column mapping, and the builder's own loop
-- variable), never on-disk bytes, and deliberately stay Asserts.
--
-- which: 0 = bm25_accum_add_posting, 1 = bm25_accum_add_positions_to_last.
-- Both directions for each, including the exactly-at-limit case, so a guard
-- that was off by one in either direction fails here.
SELECT bm25_debug_accum_field_bound(4, 0, 0) AS post_field0_ok;
SELECT bm25_debug_accum_field_bound(4, 3, 0) AS post_field3_at_limit_ok;
SELECT bm25_debug_accum_field_bound(4, 4, 0) AS post_field4_one_past;
-- The real corruption shape: a decoded id that is legal to the RLE bound
-- (< BM25_MAX_FIELDS) but past this accumulator's own field_count.
SELECT bm25_debug_accum_field_bound(4, 31, 0) AS post_field31_within_max_fields;
SELECT bm25_debug_accum_field_bound(1, 1, 0) AS post_single_field_one_past;

SELECT bm25_debug_accum_field_bound(4, 0, 1) AS pos_field0_ok;
SELECT bm25_debug_accum_field_bound(4, 3, 1) AS pos_field3_at_limit_ok;
SELECT bm25_debug_accum_field_bound(4, 4, 1) AS pos_field4_one_past;
SELECT bm25_debug_accum_field_bound(4, 31, 1) AS pos_field31_within_max_fields;

-- ------------------------------------------------------------ still decodes
-- Positive control: a real KEYED, multi-doc index still builds, seals, and
-- ranks through every validator exercised above (metapage field_count, DICT
-- entries via 5 walkers, KEYMAP type/width + per-key bounds, the accumulator
-- key path). If any guard were too strict this would fail rather than the
-- negative cases above passing.
CREATE TABLE tbb (id int PRIMARY KEY, title text, body text);
INSERT INTO tbb SELECT g, 'title ' || g, 'alpha beta gamma ' || g FROM generate_series(1, 300) g;
INSERT INTO tbb VALUES (999, 'title top', 'alpha alpha alpha alpha alpha');
CREATE INDEX tbb_idx ON tbb USING bm25_native (title, body)
  INCLUDE (id) WITH (key_field = 'id', store_positions = true);
SELECT bm25_seal('tbb_idx');
SET enable_seqscan = off;

SELECT count(*) AS alpha_matches FROM tbb WHERE title @@@ 'alpha';
SELECT id AS top_ranked FROM tbb WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha' LIMIT 1;
-- The keyed dictionary/keymap walk: every docid resolves through
-- bm25_seg_keymeta + bm25_seg_key exercised above, over real data.
SELECT count(*) AS keymap_rows FROM bm25_debug_seg_keymap('tbb_idx'::regclass, 0);
SELECT count(*) > 0 AS dict_entries_readable FROM bm25_debug_segterms('tbb_idx'::regclass);

RESET enable_seqscan;
DROP TABLE tbb;

-- ------------------------------------------- orphan-sweep segment header block
-- The reachability bitmap bm25_reclaim_orphans builds is sized to nblocks and
-- indexed by a segment header block number taken straight off the segment
-- catalog (and, in the second loop, off a retired RANGE descriptor). One of the
-- two writes was unbounded; the other silently SKIPPED an out-of-range entry.
-- Both now run this validator, so they cannot drift apart again, and both ERROR
-- rather than skip: the sweep FREES every page this bitmap leaves unmarked, so
-- an entry it has decided not to trust is not one it may quietly drop from the
-- reachable set.
--
-- A block past the extent is unambiguously corruption here, never a stale
-- pointer -- nothing in this extension ever shrinks the index relation.
--
-- WHAT THIS DOES NOT COVER: like every pure probe in this file, it pins the
-- validator's behaviour, not the fact that the two sweep loops CALL it. Reaching
-- those needs a corrupt segment catalog, which no lever can produce (the
-- field_count section below is the one place that gap was closable, because
-- there the corrupt value lives on a single page). The wiring is held by code
-- structure and by the comments at both call sites.
SELECT bm25_debug_segment_blkno_bounds(0, 100) AS blk0_ok;
SELECT bm25_debug_segment_blkno_bounds(99, 100) AS last_in_extent_ok;
\set VERBOSITY terse
SELECT bm25_debug_segment_blkno_bounds(100, 100) AS one_past_extent;
SELECT bm25_debug_segment_blkno_bounds(4294967295, 100) AS wild_blkno;
-- nblocks == 0 admits nothing: the sweep returns early on an empty relation, so
-- reaching the validator with a zero extent is itself a corrupt state.
SELECT bm25_debug_segment_blkno_bounds(0, 0) AS empty_extent_admits_nothing;
\set VERBOSITY default

-- ------------------------------------- segment header field_count, ON THE PAGE
-- Every other probe in this file is a pure function, because a validator is a
-- pure function. This one is not: the defect it gates was not a missing
-- validator but a validator that was not CALLED on one of its paths, and no pure
-- probe can tell those apart. bm25_debug_stamp_seg_field_count writes a real
-- corrupt field_count onto a real segment header so the real read path meets it.
--
-- Pre-fix, bm25_debug_seg_lenfields memcpy'd field_count uint64s into a
-- 32-element (256-byte) stack array with no check -- 99 of them is ~536 bytes
-- past the end. The validation now lives INSIDE
-- bm25_segheader_read_lenfields rather than at its call sites, so no present or
-- future caller can skip it.
CREATE TABLE fcnt (id int PRIMARY KEY, body text);
INSERT INTO fcnt SELECT g, 'alpha beta ' || g FROM generate_series(1, 50) g;
CREATE INDEX fcnt_idx ON fcnt USING bm25_native (body);
SELECT bm25_seal('fcnt_idx');
-- Healthy first: the accessor must still read a well-formed header.
SELECT count(*) > 0 AS lenfields_readable_before FROM bm25_debug_seg_lenfields('fcnt_idx'::regclass, 0);

-- The header's block number is in the message and is NOT portable -- it depends
-- on BLCKSZ and on how many pages the build emitted before it. Assert SQLSTATE
-- plus the message with the block number normalised away, so this pins WHICH
-- guard fired (not merely "some corruption error") without pinning page layout.
CREATE FUNCTION fcnt_lenfields_probe() RETURNS text AS $$
BEGIN
    PERFORM count(*) FROM bm25_debug_seg_lenfields('fcnt_idx'::regclass, 0);
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, 'block [0-9]+', 'block N');
END; $$ LANGUAGE plpgsql;

SELECT bm25_debug_stamp_seg_field_count('fcnt_idx'::regclass, 0, 99);
SELECT fcnt_lenfields_probe() AS field_count_99_rejected;
-- 0 is rejected at the same boundary as too-large: it would make every per-field
-- loop a no-op and silently zero the corpus stats rather than smashing a frame.
SELECT bm25_debug_stamp_seg_field_count('fcnt_idx'::regclass, 0, 0);
SELECT fcnt_lenfields_probe() AS field_count_0_rejected;
-- The segment is deliberately unreadable now; the table goes with it.
DROP FUNCTION fcnt_lenfields_probe();
DROP TABLE fcnt;

-- --------------------------------- posting field_id vs the SEGMENT's field_count
-- The gap the section above cannot reach. bm25_field_rle_decode bounds a decoded
-- field id to BM25_MAX_FIELDS (32) and never to the segment's own field_count, so
-- an id in the field_count..32 gap passes every decode check. It used to meet only
-- Assert(field_id < field_count) in the scan callbacks and in
-- bm25_seg_doclen_field -- an abort in a cassert build, and NOTHING in a production
-- build, where field_count is the NORMS row stride and the read therefore lands
-- inside a LATER DOCUMENT's norms row and scores the doc against a stranger's
-- field length. Every array involved is BM25_MAX_FIELDS wide, so this was never an
-- out-of-bounds read; it was a silently wrong BM25 score. Those Asserts are now
-- real ERRCODE_INDEX_CORRUPTED checks.
--
-- No corrupt RLE stream is needed to reach the gap: stamping a SMALLER field_count
-- onto the segment header opens it from the other side. The index below has two
-- fields, so 'gamma' (present only in body) posts with field_id 1; stamped down to
-- field_count = 1, that legitimate posting is one past the segment's field count --
-- exactly the "lands in another document's norms" case, on real postings.
--
-- Issue #303.G then closed that side: bm25_field_corpus_stats, which every multi-field
-- ranked scan runs before any posting is read, refuses a segment header whose
-- field_count differs from the index's. So the probe below now pins that cross-check,
-- and the posting-side guard is reached through a corrupt field RLE instead, with the
-- header intact (140_decode_value_bounds).
CREATE TABLE fldid (id int PRIMARY KEY, title text, body text);
INSERT INTO fldid SELECT g, 'alpha title ' || g, 'beta gamma delta ' || g
  FROM generate_series(1, 60) g;
-- One row with a much higher tf for 'gamma' at the same doclen, so the ranked
-- probe below has a single unambiguous winner rather than a 60-way tie.
INSERT INTO fldid VALUES (999, 'alpha title top', 'gamma gamma gamma gamma');
CREATE INDEX fldid_idx ON fldid USING bm25_native (title, body);
SELECT bm25_seal('fldid_idx');
SET enable_seqscan = off;
-- Healthy first: field 1's postings must score normally on a well-formed header.
-- Written against the FIRST key column, like the positive control above: a bare
-- `col @@@ q` is field-UNSCOPED whichever key column names it, so 'gamma' still
-- matches through its body (field 1) postings -- which is the whole point here.
SELECT count(*) AS gamma_matches_before FROM fldid WHERE title @@@ 'gamma';
SELECT id AS top_ranked_before
  FROM fldid WHERE title @@@ 'gamma' ORDER BY title &@@ 'gamma' LIMIT 1;

-- Same normalisation as the probe above: pin SQLSTATE and the message (so this
-- proves WHICH guard fired, not merely "some corruption error") while keeping any
-- block number out of it, since that depends on BLCKSZ and on build layout.
CREATE FUNCTION fldid_scored_probe() RETURNS text AS $$
BEGIN
    PERFORM id FROM fldid WHERE title @@@ 'gamma' ORDER BY title &@@ 'gamma' LIMIT 1;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, 'block [0-9]+', 'block N');
END; $$ LANGUAGE plpgsql;

SELECT bm25_debug_stamp_seg_field_count('fldid_idx'::regclass, 0, 1);
SELECT fldid_scored_probe() AS field_id_past_segment_field_count_rejected;
-- The segment is deliberately unreadable now; the table goes with it.
DROP FUNCTION fldid_scored_probe();
RESET enable_seqscan;
DROP TABLE fldid;

DROP EXTENSION bm25_native;
