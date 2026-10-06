/* bm25_scan_match.c -- the M4 match filters the exhaustive scorer runs for a phrase
 * or proximity query: the per-(TID, term, field) position stash and the recheck
 * that runs the positional matcher over it, and the AND-of-terms presence set used
 * when a phrase query meets a position-less index with phrase_fallback = 'and'.
 *
 * Split out of bm25_scan.c (#228, ADR 0101) as a pure code move; nothing here
 * changed behaviour. The stash and presence types (PhrasePosList, PhraseStashEnt,
 * PhraseStashCtx, PhraseAndEnt, PhraseAndCtx) and the design notes for both
 * mechanisms are in bm25_scan.h, because bm25_scan_build_ranking_exhaustive
 * (bm25_scan_rank.c) builds these contexts and reads their entries. That scorer is
 * the only caller outside this file of the six functions declared there: the
 * segment and pending position stashers, the presence marker and its segment and
 * pending fillers, and phrase_recheck_tid. phrase_stash_add stays static.
 *
 * Nothing here calls into bm25_scan.c or bm25_scan_rank.c. The positional matcher
 * itself is in bm25_phrase.c.
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_scan.h"          /* the stash/presence types and this file's six externs */
#include "bm25_stats.h"         /* the match-set budget both mechanisms charge */
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "utils/hsearch.h"

/* One leaf-presence entry. Separate from the per-document charge because a
 * must_not leaf marks presence for documents that never score. */
#define BM25_MATCH_BYTES_PER_PRESENCE \
    (MAXALIGN(sizeof(PhraseAndEnt)) + BM25_HASH_ENTRY_OVERHEAD)

/* Append positions[] (npos ascending) into the (tid, qi, field) list, growing it. */
static void
phrase_stash_add(PhraseStashCtx *c, ItemPointer tid, uint32 field_id,
                 const uint32 *positions, uint32 npos)
{
    PhraseStashEnt *e;
    PhrasePosList  *pl;
    bool            found;
    MemoryContext   old;
    uint32          slot;

    if (field_id >= c->field_count)
        return;                             /* defensive: RLE field_id out of range */

    old = MemoryContextSwitchTo(c->cxt);
    e = (PhraseStashEnt *) hash_search(c->stash, tid, HASH_ENTER, &found);
    if (!found)
    {
        e->lists = (PhrasePosList *) palloc0(sizeof(PhrasePosList) *
                                             c->nq * c->field_count);
        /* #62.5: the entry plus its per-(term, field) list header block. The stash's
         * DOCUMENT count does track the accumulator (both callbacks gate on live +
         * pending-dedupe, and a must_not phrase is rejected before execution), but
         * its BYTES do not: the header block scales with nq * field_count and the
         * position arrays below scale with term frequency, which reaches 65535. That
         * is why the stash is charged rather than assumed covered.
         *
         * The 65535 bound holds on both ingest paths as of #158; before that it was
         * true only of pending-path documents, because ambuild had no per-document
         * token cap and AccumPosting.tf is uint32. THE CHARGE ITSELF WAS NEVER WRONG
         * and must not be "fixed": it is levied per position actually appended (the
         * bm25_match_charge below takes the real decoded frame length), so the
         * accounting tracked reality however large tf got. Only this sentence was
         * overstating what bounded it. */
        bm25_match_charge(c->budget,
                          MAXALIGN(sizeof(PhraseStashEnt)) + BM25_HASH_ENTRY_OVERHEAD +
                          (int64) sizeof(PhrasePosList) * c->nq * c->field_count);
    }
    /* Positions, charged at 2x because the list doubles and so briefly holds the old
     * copy alongside the new. Charged per position APPENDED rather than per byte
     * allocated: the doubling makes allocation lumpy, and over a whole scan the two
     * agree to within the 2x already applied. */
    bm25_match_charge(c->budget, (int64) npos * sizeof(uint32) * 2);
    slot = c->cur_qi * c->field_count + field_id;
    pl = &e->lists[slot];
    if (pl->n + npos > pl->cap)
    {
        uint32 newcap = Max(pl->cap ? pl->cap * 2 : 8u, pl->n + npos);
        pl->pos = (pl->pos == NULL)
            ? (uint32 *) palloc(sizeof(uint32) * newcap)
            : (uint32 *) repalloc(pl->pos, sizeof(uint32) * newcap);
        pl->cap = newcap;
    }
    memcpy(pl->pos + pl->n, positions, sizeof(uint32) * npos);
    pl->n += npos;
    MemoryContextSwitchTo(old);
}

/* Segment phrase pos_cb: fires per position-bearing posting in lockstep with the
 * POST scan. Gate identically to the SCORER's seg_posting_cb (live-docs + pending
 * dedupe) so the stash holds positions for exactly the segment postings that scored;
 * pending positions are stashed separately (pending wins) before the segment pass. */
void
seg_phrase_pos_cb(uint32 local_docid, uint32 field_id,
                  const uint32 *positions, uint32 npos, void *state)
{
    PhraseStashCtx *c = (PhraseStashCtx *) state;
    ItemPointerData tid;

    if (!bm25_seg_reader_doc_is_live(&c->rdr, local_docid))
        return;
    tid = bm25_seg_reader_docid_to_tid(&c->rdr, local_docid);
    if (c->pending_tids != NULL)
    {
        bool found;

        (void) hash_search(c->pending_tids, &tid, HASH_FIND, &found);
        if (found)
            return;                         /* pending wins: its positions were stashed */
    }
    phrase_stash_add(c, &tid, field_id, positions, npos);
}

/* Scan the pending list once for the current phrase term (cur_qi set by the caller),
 * decoding each matching (term, field) entry's TRUE positions from its delta-varbyte
 * posblob (the same codec the drain uses) and stashing them. This is what makes an
 * inserted-but-unsealed doc phrase-matchable (read-your-writes, 16_pending_ryw).
 * termlen/term identify the phrase term; only live (TID-valid) docs are stashed. */
void
pending_phrase_stash(PhraseStashCtx *c, BlockNumber pending_head,
                     uint32 epoch_bound, const char *term, int termlen)
{
    BlockNumber     blk = pending_head;
    PGAlignedBlock  copy;
    BM25PendingWalk w;

    bm25_pending_walk_init(&w, c->index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&w, blk); /* SHARE-locked */

        /* Fix (2026-08, crash/replica-safety pass): copy the page out under the
         * SHARE lock, then unlock immediately, before any decoding or stashing.
         * This loop used to palloc a scratch position array and call
         * phrase_stash_add (hash_search + repalloc, either of which can grow/
         * rehash, unbounded by this one page's size) while still holding the
         * lock -- extending its hold time on a page other backends (an insert
         * appending to the tail, a concurrent seal) need. A single fixed-size
         * BLCKSZ memcpy is cheap and bounded; everything downstream now runs
         * unlocked against the caller-owned copy and never touches the shared
         * buffer again. Mirrors bm25_meta_read's copy-then-use shape. */
        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) copy.data;
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;

            if (!ItemPointerIsValid(&dh->tid))
                continue;
            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                char *tterm = p + sizeof(BM25PendingTermEntry);

                if (te->termlen == termlen &&
                    memcmp(tterm, term, termlen) == 0 &&
                    /* Gate to mirror the sealed reader (bm25_seg_chain.c: no pos_cb for
                     * off fields): skip an off-field posting so pending and sealed treat
                     * off fields identically. Without this, a pending off-field posting
                     * would contribute positions that vanish after the next seal -- a
                     * match that flips across a seal. NULL => all fields on. */
                    (c->field_store_positions == NULL ||
                     te->field_id >= c->field_count ||
                     c->field_store_positions[te->field_id]))
                {
                    /* Decode the entry's tf ascending positions from its posblob
                     * (delta-varbyte, first delta = pos[0]); stash them for this
                     * (TID, cur_qi, field_id). Bounded scratch: at most tf uint32s. */
                    const uint8 *pb = (const uint8 *) (tterm + te->termlen);
                    const uint8 *pbend = pb + te->pos_bytes;
                    uint32       prev = 0;
                    uint16       r;
                    uint32      *tmp = (uint32 *) palloc(sizeof(uint32) *
                                                         Max(te->tf, 1));

                    for (r = 0; r < te->tf; r++)
                    {
                        uint32 delta;
                        pb += bm25_varbyte_decode(pb, pbend, &delta);
                        /* Issue #303.F: the sealed reader's ascent check
                         * (bm25_seg_scan_postings), for the same list on a pending
                         * page: the stash below is binary-searched and slot-assigned
                         * as strictly ascending. */
                        if ((r > 0 && delta == 0) || prev + delta < prev)
                            ereport(ERROR,
                                    (errcode(ERRCODE_INDEX_CORRUPTED),
                                     errmsg("bm25: pending position list of field %u does "
                                            "not ascend: delta %u after position %u",
                                            (uint32) te->field_id, delta, prev),
                                     errdetail("The record is on block %u of the pending chain.",
                                               blk),
                                     errhint("REINDEX the index.")));
                        prev += delta;
                        tmp[r] = prev;
                    }
                    phrase_stash_add(c, &dh->tid, te->field_id, tmp, te->tf);
                    pfree(tmp);
                }
                p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
            }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;   /* read off the unlocked copy */
        blk = next;
    }
}

void
phrase_and_mark(PhraseAndCtx *c, ItemPointer tid)
{
    PhraseAndEnt *e;
    bool          found;

    e = (PhraseAndEnt *) hash_search(c->presence, tid, HASH_ENTER, &found);
    if (!found)
    {
        e->mask = 0;
        /* #62.5: charged on its own account, not assumed to track the scoring
         * accumulator. The other per-document hashes on this path DO track `acc` --
         * pending_tids is written only for a doc that scored, and a must_not PHRASE
         * is rejected before execution, so every stashed doc is a scored one -- but
         * this hash also marks the leaves that carry NO score. A boolean query whose
         * must_not names a very common term grows it per document containing that
         * term while acc stays small. Charging into the shared total also stops this
         * hash and `acc` from each spending the whole budget on a boolean query that
         * fills both. */
        bm25_match_charge(c->budget, BM25_MATCH_BYTES_PER_PRESENCE);
    }
    e->mask |= (UINT64CONST(1) << c->cur_qi);
}

/* Segment presence cb for the AND fallback: mark (TID, cur_qi), gated identically to
 * the scorer (live + pending dedupe) so presence matches the scored set. */
void
seg_and_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    PhraseAndCtx   *c = (PhraseAndCtx *) state;
    ItemPointerData tid;

    (void) tf;
    /* Honor a field scope: a scoped AND query counts a term present only in the
     * queried field, mirroring seg_posting_cb's idf-0 gate on the scoring path. */
    if (c->qfield != BM25_FIELD_ALL && (int32) field_id != c->qfield)
        return;
    if (!bm25_seg_reader_doc_is_live(&c->rdr, local_docid))
        return;
    tid = bm25_seg_reader_docid_to_tid(&c->rdr, local_docid);
    if (c->pending_tids != NULL)
    {
        bool found;

        (void) hash_search(c->pending_tids, &tid, HASH_FIND, &found);
        if (found)
            return;                         /* pending already marked this TID */
    }
    phrase_and_mark(c, &tid);
}

/* Pending presence walk for the AND fallback (read-your-writes): mark every live
 * pending doc that carries the phrase term (identified by term/termlen). */
void
pending_and_stash(PhraseAndCtx *c, BlockNumber pending_head,
                  uint32 epoch_bound, const char *term, int termlen)
{
    BlockNumber     blk = pending_head;
    PGAlignedBlock  copy;
    BM25PendingWalk w;

    bm25_pending_walk_init(&w, c->index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&w, blk); /* SHARE-locked */

        /* Fix (2026-08, crash/replica-safety pass): copy-then-unlock, same shape
         * and reasoning as pending_phrase_stash above -- phrase_and_mark does a
         * hash_search(HASH_ENTER), which can trigger a rehash unbounded by this
         * page's size, and that used to run while the page was still locked. */
        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) copy.data;
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;

            if (!ItemPointerIsValid(&dh->tid))
                continue;
            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                char *tterm = p + sizeof(BM25PendingTermEntry);

                if (te->termlen == termlen && memcmp(tterm, term, termlen) == 0 &&
                    (c->qfield == BM25_FIELD_ALL ||
                     (int32) te->field_id == c->qfield))
                {
                    phrase_and_mark(c, &dh->tid);
                    break;      /* one field's hit is enough for AND presence */
                }
                p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
            }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;   /* read off the unlocked copy */
        blk = next;
    }
}

/* Run the D5 matcher for one stashed TID: return true iff the phrase holds within
 * ANY single field the doc satisfies every SLOT in (cross-field OR, D6). A
 * field-scoped phrase (qfield != BM25_FIELD_ALL) restricts the check to that ONE field
 * so a `title:"a b"` never matches on `body` (the pos stash is field-agnostic; scoping
 * lives here). pos[]/npos[] are per-call scratch of length nslots.
 *
 * SLOTS, NOT TOKENS (issue #184). The stash is still per-TOKEN -- smap is consulted
 * only here, so nothing about how positions are gathered changes. Per field this now:
 *   1. gathers each slot's member lists (the token ordinals smap assigns to it);
 *   2. disqualifies the field when a slot has NO member present. That is the per-slot
 *      ANY-of test that replaces the old per-token all-of test: a source word is
 *      satisfied by any one of the lexemes it produced;
 *   3. borrows the member's pointer outright when a slot has exactly one -- the
 *      common case, since a slot is one token unless a compound-splitting dictionary
 *      co-positions several lexemes (analyzer revision 5), so the hot path costs one
 *      extra indirection and no copy;
 *   4. merges + dedups otherwise into scratch freed before this function returns.
 * The scratch is bounded by positions the stash already charged to the #62.5 match
 * budget (it is a subset of them, copied), and it is transient, so it takes no
 * separate charge.
 */
bool
phrase_recheck_tid(const PhraseStashEnt *e, uint32 nq, uint32 field_count,
                   int32 qfield, bool ordered, int slop,
                   const BM25PhraseSlotMap *smap)
{
    uint32 field;
    uint32 field_lo = (qfield == BM25_FIELD_ALL) ? 0 : (uint32) qfield;
    uint32 field_hi = (qfield == BM25_FIELD_ALL) ? field_count : (uint32) qfield + 1;
    int    nslots = smap->nslots;

    /* A zero-slot phrase matches nothing, and saying so HERE rather than relying on
     * the callers keeps the invariant local: bm25_phrase_match reads npos[0] before
     * it can conclude anything, so an empty map would read uninitialized stack in a
     * production build. Both callers do guard (the text path rejects nq == 0, and a
     * zero-token boolean leaf stashes nothing so this never runs), which is why this
     * is unreachable today rather than a fix -- but the guard is two call sites away
     * from the read it protects. */
    if (nslots == 0)
        return false;

    Assert(nslots >= 0 && nslots <= (int) nq);   /* invariant */

    for (field = field_lo; field < field_hi; field++)
    {
        const uint32 *pos[BM25_PHRASE_MAX_TERMS];
        uint32        npos[BM25_PHRASE_MAX_TERMS];
        uint32       *scratch[BM25_PHRASE_MAX_TERMS];   /* per-slot merge buffers */
        bool          all_present = true;
        bool          matched = false;
        int           k;

        for (k = 0; k < nslots; k++)
            scratch[k] = NULL;

        for (k = 0; k < nslots; k++)
        {
            const uint32 *mpos[BM25_PHRASE_MAX_TERMS];
            uint32        mnpos[BM25_PHRASE_MAX_TERMS];
            int           nmem = 0;
            uint32        total = 0;
            uint32        qi;

            /* Slot members are a CONTIGUOUS range of token ordinals -- see the
             * BM25PhraseSlotMap contract in bm25.h -- which is what keeps this gather
             * linear in nq rather than nq * nslots on a per-candidate-TID path. */
            for (qi = smap->slot_start[k]; qi < smap->slot_start[k + 1]; qi++)
            {
                const PhrasePosList *pl = &e->lists[qi * field_count + field];

                if (pl->n == 0)
                    continue;               /* this member absent; another may serve */
                mpos[nmem] = pl->pos;
                mnpos[nmem] = pl->n;
                total += pl->n;
                nmem++;
            }
            if (nmem == 0)
            {
                all_present = false;        /* no member of this word in this field */
                break;
            }
            if (nmem == 1)
            {
                pos[k] = mpos[0];           /* zero-copy borrow */
                npos[k] = mnpos[0];
            }
            else
            {
                scratch[k] = (uint32 *) palloc(sizeof(uint32) * total);
                npos[k] = bm25_phrase_merge_lists(mpos, mnpos, nmem, scratch[k]);
                pos[k] = scratch[k];
            }
        }
        if (all_present)
            matched = bm25_phrase_match(pos, npos, nslots, ordered, slop);
        for (k = 0; k < nslots; k++)
            if (scratch[k] != NULL)
                pfree(scratch[k]);
        if (matched)
            return true;                    /* matched within this field (D6 OR) */
    }
    return false;
}
