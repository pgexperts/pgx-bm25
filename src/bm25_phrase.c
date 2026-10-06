/*
 * bm25_phrase.c -- the pure phrase / proximity matcher (M4 C-MATCH, D5).
 *
 * ROLE IN THE SYSTEM: the query-time decision, over ONE (doc, field), of whether
 * the phrase's N SLOTS co-occur with the required order/slop. It is deliberately a
 * PURE function of per-slot ascending position lists -- no PostgreSQL state, no
 * buffer/relation access -- so it is unit-testable in isolation (see main() in the
 * BM25_PHRASE_SELFTEST block at the end of this file) and reused verbatim by the
 * scorer's post-accumulation
 * recheck (bm25_scan_match.c). The scorer supplies the position lists (stashed from the
 * sealed-segment POS chain via a lockstep pos_cb, and from decoded pending
 * posblobs); this file only does the arithmetic.
 *
 * SLOTS AND ALTERNATIVE GROUPS (issue #184). A slot is ONE SOURCE WORD of the query,
 * not one query lexeme. A dictionary that splits a compound emits several lexemes for
 * one word, and a document position that carries ANY of them satisfies that word's
 * slot. That is a recall-only superset of core's per-variant chains (ADR 0085): core
 * ANDs the lexemes of one variant and ORs the variants, where a slot ORs every
 * lexeme, so it can match a document core would not (one carrying just `foot` for a
 * `footballklubber` slot) but never drops one core matches. bm25_phrase_slot_map
 * (below) derives the grouping from the analyzer's own positions;
 * bm25_scan_match.c's phrase_recheck_tid folds each
 * slot's member lists into ONE deduplicated ascending list and calls in here with
 * nterms == nslots. Two consequences the matcher must honour:
 *   - the span arithmetic now counts SOURCE WORDS, which is what makes it comparable
 *     with core's word distance;
 *   - two different slots' lists MAY OVERLAP (a doc run whose lexemes satisfy two
 *     different query words), which is what the unordered path's KEY FACT block had
 *     to be rewritten around.
 * Analyzer revision 5 emits per-run positions (ADR 0087), so both statements are live
 * for any analyzer whose dictionary splits compounds. For an analyzer that never emits
 * more than one lexeme per word (the default english configuration) one slot is still
 * exactly one query token and every slot list is a distinct term's list -- see the
 * identity-map note on BM25PhraseSlotMap in bm25.h.
 *
 * SLOP DEFINITION (load-bearing): n is the MAX slop = extra distance BEYOND perfect
 * adjacency, not an absolute span and not a per-gap bound. For N adjacent slots the
 * minimal span is (N-1); slop n permits a span up to (N-1)+n.
 *
 *   EXACT phrase  (ordered, slop 0): consecutive slots at consecutive positions.
 *   ORDERED PRE/n (ordered, slop n): query order, positions strictly increasing,
 *                 some choice with (p_last - p_first) - (N-1) <= n.
 *   UNORDERED W/n (unordered, slop n): some choice of one DISTINCT position per slot
 *                 (any order) whose window span (max - min) <= (N-1) + n.
 *
 * EDGE CASES (all covered by the 38_phrase suite):
 *   - N == 1: match iff that slot's list is non-empty (single-slot phrase == plain
 *     term). The general paths also reduce to this, but it is short-circuited.
 *   - empty list for ANY slot: no match (a word the doc lacks cannot phrase-match).
 *   - repeated word ("the the"): two DISTINCT entries, each with its own (identical)
 *     list. The strict-increase advance (ordered) / distinct-position assignment
 *     (unordered) forces two DISTINCT positions, so "the the" needs two occurrences.
 *   - position 0: positions are 0-based per-(doc,field) ordinals; nothing special.
 *
 * COMPLEXITY, AND THE SIZE OF P (corrected, XCUT-04 / issue #139). P -- the total
 * positions across the slots -- is NOT small. It is the sum of the slots' per-(doc,
 * field) TERM FREQUENCIES, straight from user data: one long document with a term
 * repeated thousands of times produces a list of thousands. Both ingest paths cap a
 * document at PG_UINT16_MAX tokens (#158 / docs/adr/0079 -- ambuild used to have no
 * such cap), so ONE slot's list holds at most 65535 entries and the merged stream at
 * most nslots * 65535. The comment that used to sit here ("m is tiny", "tiny inputs
 * ... not asymptotics") was wrong, and it was the justification for an insertion sort
 * on input that is the worst case for one: a concatenation of per-group ASCENDING
 * runs, so two slots of n occurrences each interleave into ~n^2/4 swaps.
 *
 * The ordered path tries every position of slot 0 and binary-searches each later
 * slot: O(|list_0| * N * log L), L = the longest list. The unordered path sorts the
 * merged tagged position stream with pg_qsort -- O(P log P) -- and then either makes
 * one two-pointer minimum-window pass over it (O(P), the disjoint case, which is
 * every case a one-lexeme-per-word analyzer can produce) or runs the incremental
 * bipartite-matching sweep, bounded in unordered_match's own header.
 *
 * CANCELLABILITY: unlike the POS-chain decode that produces these lists, this file
 * runs entirely UNLOCKED -- the scorer copies positions into its own stash before
 * calling in (bm25_scan_match.c's phrase_stash_add / phrase_recheck_tid) -- so
 * InterruptHoldoffCount is 0 here and a CHECK_FOR_INTERRUPTS in a matcher loop
 * genuinely fires. Each P-sized loop body carries one. pg_qsort itself is not
 * interruptible, which is a second reason the sort had to stop being quadratic.
 */
#include "postgres.h"

#include "bm25.h"
#include "miscadmin.h"          /* CHECK_FOR_INTERRUPTS */

/*
 * bm25_phrase_slot_map -- group a phrase's analyzed query tokens into per-source-word
 * slots (issue #184).
 *
 * A new slot starts wherever the token's position changes. The ONLY property this
 * relies on is that bm25_analyze emits tokens in source order with a NONDECREASING
 * .pos -- it holds because there is a single position counter that is only ever
 * incremented, never reset or rewound. The Assert documents it and catches a
 * violation on a cassert build, which is where this project's UBSan and hardening
 * legs run -- but Assert compiles out of a production build, so on a release binary
 * the property really is trusted, and a future emitter that violated it would
 * silently produce interleaved slots (a phrase whose words are matched out of order)
 * rather than failing. Treat the invariant as the emitter's obligation; the Assert is
 * a tripwire on the builds that have one, not a guarantee on the builds that do not.
 *
 * When every token has its own position (an analyzer that emits one lexeme per word),
 * this returns the identity map (nslots == ntok, slot_start[k] == k) and every caller
 * behaves exactly as it did before slots existed. That was the safety argument for
 * landing the query side ahead of the analyzer change (revision 5, ADR 0087); since
 * then a compound split puts several tokens on one position and yields a multi-token
 * slot.
 */
void
bm25_phrase_slot_map(const BM25Token *toks, int ntok, BM25PhraseSlotMap *out)
{
    int     i;

    Assert(ntok >= 0 && ntok <= BM25_PHRASE_MAX_TERMS);   /* checked: bm25_scan_rank.c's nq cap */

    out->nslots = 0;
    for (i = 0; i < ntok; i++)
    {
        Assert(i == 0 || toks[i].pos >= toks[i - 1].pos);   /* invariant */
        if (i == 0 || toks[i].pos != toks[i - 1].pos)
            out->slot_start[out->nslots++] = (uint8) i;
    }
    out->slot_start[out->nslots] = (uint8) ntok;
}

/*
 * Binary min-heap over the merge cursors, keyed by each list's current head. The heap
 * holds LIST INDICES; heap_key() reads the head through cur[]. Only sift-down is
 * needed: the heap is built once and then only ever loses or replaces its root.
 */
static inline uint32
merge_heap_key(const uint32 *const *lists, const uint32 *cur, int li)
{
    return lists[li][cur[li]];
}

static void
merge_sift_down(int *heap, int nheap, int root,
                const uint32 *const *lists, const uint32 *cur)
{
    while (2 * root + 1 < nheap)
    {
        int     child = 2 * root + 1;
        int     tmp;

        if (child + 1 < nheap &&
            merge_heap_key(lists, cur, heap[child + 1]) <
            merge_heap_key(lists, cur, heap[child]))
            child++;
        if (merge_heap_key(lists, cur, heap[root]) <=
            merge_heap_key(lists, cur, heap[child]))
            break;
        tmp = heap[root];
        heap[root] = heap[child];
        heap[child] = tmp;
        root = child;
    }
}

uint32
bm25_phrase_merge_lists(const uint32 *const *lists, const uint32 *lens,
                        int nlists, uint32 *out)
{
    int     heap[BM25_PHRASE_MAX_TERMS];
    uint32  cur[BM25_PHRASE_MAX_TERMS];
    int     nheap = 0;
    uint32  nout = 0;
    int     i;

    Assert(nlists >= 1 && nlists <= BM25_PHRASE_MAX_TERMS);   /* checked: the nq cap */

    for (i = 0; i < nlists; i++)
    {
        cur[i] = 0;
        if (lens[i] > 0)
            heap[nheap++] = i;
    }
    for (i = nheap / 2 - 1; i >= 0; i--)
        merge_sift_down(heap, nheap, i, lists, cur);

    while (nheap > 0)
    {
        int     top = heap[0];
        uint32  v = lists[top][cur[top]];

        /* Per EMITTED position: nlists is bounded by the phrase cap but the lists
         * themselves are term frequencies, so this is the loop that can run long. */
        CHECK_FOR_INTERRUPTS();

        /* Dedup: the same doc position can appear in two member lists once a run's
         * lexemes share a position, and bm25_phrase_match's contract is a STRICTLY
         * ascending list per slot. */
        if (nout == 0 || out[nout - 1] != v)
            out[nout++] = v;

        if (++cur[top] >= lens[top])
            heap[0] = heap[--nheap];
        merge_sift_down(heap, nheap, 0, lists, cur);
    }
    return nout;
}

/*
 * lower_bound_gt -- index of the first element of a[0..n) strictly greater than key,
 * or n if none. a[] is ascending. Used by the ordered path to advance a term to the
 * smallest position strictly after the previously chosen one (enforces both query
 * order AND strictly-increasing, which is what makes a repeated term consume two
 * distinct positions).
 */
static int
lower_bound_gt(const uint32 *a, int n, uint32 key)
{
    int lo = 0,
        hi = n;

    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;

        if (a[mid] > key)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/*
 * ordered_match -- ORDERED PRE/n (and EXACT as slop 0).
 *
 * For each candidate first position p1 (every position of slot 0), greedily assign
 * each later slot the SMALLEST position strictly greater than the previous chosen
 * position. Greedy-earliest minimizes p_last for that fixed p1, so if any ordered
 * placement starting at p1 satisfies the span bound, this one does. Accept when
 * (p_last - p1) - (N-1) <= slop. Strictly-increasing assignment also handles a
 * repeated word (its identical list is advanced past the prior pick).
 *
 * EXACT is just slop == 0: the bound becomes (p_last - p1) == (N-1), i.e. perfectly
 * consecutive -- no separate code path needed.
 *
 * ALTERNATIVE GROUPS NEEDED NO CHANGE HERE (issue #184, audited). The exchange
 * argument assumes only that each list is ascending; it never assumed the lists were
 * disjoint, and "strictly greater than the previous pick" IS the per-slot invariant
 * the slot design asks for. Two slots sharing a position simply means the shared
 * position can satisfy at most one of them, which strict increase already enforces.
 * The reverted doc-side-only fix broke phrase search precisely because it made N
 * co-positioned LEXEMES fill N slots; folding a source word's lexemes into ONE slot
 * before the matcher sees them is what makes the same greedy chain correct again.
 */
static bool
ordered_match(const uint32 *const *pos, const uint32 *npos, int nterms, int slop)
{
    int i0;

    for (i0 = 0; i0 < (int) npos[0]; i0++)
    {
        uint32 p1 = pos[0][i0];
        uint32 prev = p1;
        int    t;
        bool   ok = true;

        /* npos[0] is a term frequency, i.e. user-data-sized (see the file header),
         * and each trip does N binary searches. Nothing is locked on this path. */
        CHECK_FOR_INTERRUPTS();

        for (t = 1; t < nterms; t++)
        {
            int j = lower_bound_gt(pos[t], (int) npos[t], prev);

            if (j >= (int) npos[t])
            {
                ok = false;         /* no position of term t strictly after prev */
                break;
            }
            prev = pos[t][j];
        }
        if (!ok)
            continue;
        /* prev is p_last for this greedy placement. span = prev - p1; the minimal
         * span for N terms is (N-1), slop allows (N-1)+slop. */
        if ((prev - p1) <= (uint32) ((nterms - 1) + slop))
            return true;
    }
    return false;
}

/* Compare two ascending uint32 lists for exact content equality. Used to group
 * slots with identical occurrence sets (a repeated word, whose slots the recheck
 * builds as separate but identical lists; or two distinct words that happen to occur
 * at exactly the same positions) into ONE group with a required multiplicity. The
 * grouping is correct WHATEVER made the lists equal: G equal slots need G distinct
 * positions out of the one shared set, which is exactly the multiplicity rule. */
static bool
lists_equal(const uint32 *a, uint32 na, const uint32 *b, uint32 nb)
{
    uint32 i;

    if (na != nb)
        return false;
    for (i = 0; i < na; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

/* One merged position, tagged with the group that owns it. File-scope only so
 * tagged_pos_cmp below can name it; nothing outside this file uses it. */
typedef struct Tagged
{
    uint32  pos;
    int     grp;
} Tagged;

/* pg_qsort comparator for the merged stream. Equal keys MEAN SOMETHING here: one
 * group's own run is strictly ascending, so two entries can only share a position if
 * two DIFFERENT groups' sets overlap -- the case unordered_match classifies on and
 * dispatches to its exact path (see its KEY FACT block). This comment used to claim
 * the equal case was unreachable, which was true only while a position could belong
 * to exactly one query term. The comparator itself needs no change: it was already
 * correct for equal keys, and the sweep that consumes the sorted stream does not
 * depend on the order WITHIN an equal-position cluster, so instability is harmless.
 * Subtraction is avoided because uint32 positions can differ by more than INT_MAX. */
static int
tagged_pos_cmp(const void *a, const void *b)
{
    uint32 pa = ((const Tagged *) a)->pos;
    uint32 pb = ((const Tagged *) b)->pos;

    if (pa < pb)
        return -1;
    if (pa > pb)
        return 1;
    return 0;
}

/*
 * counting_sweep -- the classic "minimum window substring with multiplicity"
 * two-pointer pass over the sorted tagged stream. Expand hi until every group has its
 * required count in the window, then contract lo while still satisfied, testing the
 * span at each step. O(m).
 *
 * VALID ONLY when the groups' position sets are pairwise disjoint; unordered_match's
 * KEY FACT block below states why, and step (3) there is what checks it per call.
 * Otherwise unchanged from the sweep this file has always run.
 */
static bool
counting_sweep(const Tagged *merged, uint32 m, int ngroups, const int *required,
               uint32 span_bound)
{
    int      have[BM25_PHRASE_MAX_TERMS];       /* per-group count in the current window */
    int      satisfied = 0;                     /* groups meeting `required` */
    int      lo = 0,
             hi,
             g;

    for (g = 0; g < ngroups; g++)
        have[g] = 0;
    for (hi = 0; hi < (int) m; hi++)
    {
        /* m-sized. The inner contraction loop below needs no check of its own: lo
         * only ever advances, so its total trips across this whole sweep are also
         * bounded by m and each one is preceded by a trip through here. */
        CHECK_FOR_INTERRUPTS();

        g = merged[hi].grp;
        have[g]++;
        if (have[g] == required[g])
            satisfied++;

        while (satisfied == ngroups)
        {
            uint32 span = merged[hi].pos - merged[lo].pos;

            if (span <= span_bound)
                return true;        /* a valid window within slop exists */
            /* contract from lo. */
            {
                int lg = merged[lo].grp;

                if (have[lg] == required[lg])
                    satisfied--;
                have[lg]--;
                lo++;
            }
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * The exact path: windowed incremental bipartite matching (an SDR -- system of
 * distinct representatives). Reached only when two groups' position sets overlap
 * without being equal, which needs the per-run positions the analyzer emits since
 * revision 5 (issue #184, ADR 0087): with one lexeme per word, sets are disjoint or
 * equal.
 *
 * THE REDUCTION. Left vertices are slot INSTANCES: group g contributes required[g] of
 * them. Right vertices are the DISTINCT doc positions inside the current window. An
 * instance of g may take position p iff p is in g's set. A window admits a valid
 * unordered assignment iff that bipartite graph has a matching saturating every
 * instance (Hall's theorem, with the repeated-word multiplicities folded into
 * instances). No counting shortcut is claimed anywhere on this path.
 *
 * THE SWEEP. Slide the same minimum-window two-pointer over the stream's DISTINCT
 * positions, maintaining a MAXIMUM matching incrementally rather than rebuilding it:
 *   - on ADD of position p, one augmenting search rooted at p (sdr_try_pos). Sound
 *     because the matching was maximum before p existed, so every augmenting path in
 *     the enlarged graph must END at p -- p is free, and a free vertex cannot sit in
 *     the interior of an alternating path.
 *   - on CONTRACT, p leaves on the left; if it was matched, free its instance and run
 *     one augmenting search rooted at that instance (sdr_try_group). Sound by the
 *     mirror argument: deleting a vertex costs the maximum at most one, and every
 *     augmenting path in the reduced graph must START at the freed instance -- a path
 *     avoiding it would have been an augmenting path before the deletion too.
 * So "matching size == nslots" is exactly "this window is feasible", and feasibility
 * is monotone in the window, which is what makes the two-pointer contraction legal.
 *
 * COST, STATED AGAINST REACHABLE INPUT. G = groups <= nslots <= nq, and nq is capped at
 * BM25_PHRASE_MAX_TERMS = 64 by BOTH phrase entry points before anything is stashed
 * (bm25_scan_rank.c raises ERRCODE_PROGRAM_LIMIT_EXCEEDED past it), so G, N <= 64 comes from
 * an explicit QUERY cap and not from the analyzer. D = distinct positions in one
 * (doc, field) <= 65535, because a position is a per-(doc,field) token ordinal and a
 * document is capped at PG_UINT16_MAX tokens on BOTH ingest paths (docs/adr/0079); the
 * sweep does <= 2D events, since each cursor crosses each distinct position once.
 *
 * Each augmenting search visits each group at most once (visited[]), and a group's
 * frame costs O(G) bit tests + O(N) assignment scans + O(N) for sdr_first_free -- O(N)
 * and not O(tf) because at most N positions are ever assigned, so at most N+1 of a
 * group's in-window positions can be occupied before an unassigned one turns up. That
 * is O(G*(G+N)) per event and O(D*G*(G+N)) overall: at the simultaneous ceiling of all
 * three, order 1e9 integer ops for ONE candidate document -- roughly an order of
 * magnitude above the ordered path's own worst case (|list_0| * N * log L ~ 7e7) and
 * the counting sweep's (O(m log m) ~ 9e7), which is why it runs only when the counting
 * proof is void and why every event carries a CHECK_FOR_INTERRUPTS. Reaching that
 * ceiling needs a compound dictionary, a 64-word phrase whose words alias each other's
 * lexemes at shared document positions, and documents at the token cap; realistic G is
 * 2-5, where the sweep is a rounding error. Memory is O(1) on top of the merged
 * stream: nothing here is indexed by D.
 *
 * REJECTED: rebuilding a Kuhn matching per candidate window, N augmentations per window
 * over D windows, order 4e10 -- the "no quadratic on user-sized input" rule (#139) all
 * over again.
 * ---------------------------------------------------------------------------
 */
typedef struct SdrAssign
{
    uint32  pos;                /* an in-window doc position, currently matched */
    uint64  mask;               /* bitmap of the groups whose set contains pos */
    int     grp;                /* the group holding it right now */
} SdrAssign;

typedef struct SdrState
{
    const Tagged   *merged;     /* the sorted tagged stream, for mask lookups */
    uint32          m;

    int             ngroups;
    const int      *required;
    const uint32   *glist[BM25_PHRASE_MAX_TERMS];   /* group -> its ascending position set */
    uint32          glen[BM25_PHRASE_MAX_TERMS];

    /* window: [gstart[g], gend[g]) is group g's in-window slice of glist[g]. Both
     * cursors only advance, so maintaining them costs O(P) over the whole sweep. */
    uint32          gstart[BM25_PHRASE_MAX_TERMS];
    uint32          gend[BM25_PHRASE_MAX_TERMS];

    /* the current matching: at most nslots <= 64 records */
    SdrAssign       asg[BM25_PHRASE_MAX_TERMS];
    int             nasg;
    int             used[BM25_PHRASE_MAX_TERMS];    /* records per group */
    uint32          apos[BM25_PHRASE_MAX_TERMS];    /* asg[].pos, kept ASCENDING */

    bool            visited[BM25_PHRASE_MAX_TERMS]; /* one augmenting search's marks */
} SdrState;

static int
sdr_find(const SdrState *st, uint32 p)
{
    int     k;

    for (k = 0; k < st->nasg; k++)
        if (st->asg[k].pos == p)
            return k;
    return -1;
}

/* apos[] mirrors the matched positions in ascending order so sdr_first_free can walk a
 * group's in-window slice and the matched set in lockstep. Both are <= 64 entries, so
 * an insertion shift beats any index that would have to be kept coherent as well. */
static void
sdr_assign(SdrState *st, uint32 p, uint64 mask, int g)
{
    int     i;

    Assert(st->nasg < BM25_PHRASE_MAX_TERMS);   /* invariant */
    Assert(sdr_find(st, p) < 0);   /* invariant */
    st->asg[st->nasg].pos = p;
    st->asg[st->nasg].mask = mask;
    st->asg[st->nasg].grp = g;
    st->nasg++;
    st->used[g]++;
    for (i = st->nasg - 1; i > 0 && st->apos[i - 1] > p; i--)
        st->apos[i] = st->apos[i - 1];
    st->apos[i] = p;
}

static void
sdr_unassign(SdrState *st, int k)
{
    uint32  p = st->asg[k].pos;
    int     i;

    st->used[st->asg[k].grp]--;
    st->asg[k] = st->asg[st->nasg - 1];     /* swap-delete: order is not meaningful */
    st->nasg--;
    for (i = 0; i <= st->nasg; i++)
        if (st->apos[i] == p)
            break;
    Assert(i <= st->nasg);   /* invariant */
    for (; i < st->nasg; i++)
        st->apos[i] = st->apos[i + 1];
}

/* First in-window position of group g that no instance currently holds. Bounded by
 * O(nasg): every outer trip that fails consumes one entry of the matched set. */
static bool
sdr_first_free(const SdrState *st, int g, uint32 *out)
{
    uint32  i;
    int     j = 0;

    Assert(st->gend[g] <= st->glen[g]);   /* invariant */
    for (i = st->gstart[g]; i < st->gend[g]; i++)
    {
        uint32  p = st->glist[g][i];

        while (j < st->nasg && st->apos[j] < p)
            j++;
        if (j >= st->nasg || st->apos[j] != p)
        {
            *out = p;
            return true;
        }
        j++;
    }
    return false;
}

/* The set of groups whose position set contains p, read back off the sorted stream so
 * nothing D-sized has to be materialized. O(log m + G). */
static uint64
sdr_mask_of(const SdrState *st, uint32 p)
{
    uint32  lo = 0,
            hi = st->m;
    uint64  mask = 0;

    while (lo < hi)
    {
        uint32  mid = lo + (hi - lo) / 2;

        if (st->merged[mid].pos < p)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < st->m && st->merged[lo].pos == p; lo++)
        mask |= UINT64CONST(1) << st->merged[lo].grp;
    Assert(mask != 0);   /* invariant */
    return mask;
}

/* Hand record k's position to a DIFFERENT group, recursing when that group is itself
 * full. Only ever rewrites .grp fields -- no record is added or removed -- so record
 * indices stay valid across the recursion and a failure leaves the state untouched. */
static bool
sdr_rehome(SdrState *st, int k)
{
    uint64  mask = st->asg[k].mask;
    int     from = st->asg[k].grp;
    int     h;

    for (h = 0; h < st->ngroups; h++)
    {
        int     k2;

        /* h == from is skipped explicitly as well as by visited[]: "re-homing" a
         * record onto its own group would report progress that did not happen. */
        if (h == from || !(mask & (UINT64CONST(1) << h)) || st->visited[h])
            continue;
        st->visited[h] = true;
        if (st->used[h] < st->required[h])
        {
            st->asg[k].grp = h;
            st->used[from]--;
            st->used[h]++;
            return true;
        }
        for (k2 = 0; k2 < st->nasg; k2++)
        {
            if (st->asg[k2].grp != h)
                continue;
            if (sdr_rehome(st, k2))
            {
                /* the recursion already decremented used[h] for k2 */
                st->asg[k].grp = h;
                st->used[from]--;
                st->used[h]++;
                return true;
            }
        }
    }
    return false;
}

/* ADD event: position p just entered the window unmatched. Grow the matching if an
 * augmenting path ending at p exists. */
static bool
sdr_try_pos(SdrState *st, uint32 p, uint64 mask)
{
    int     g;

    for (g = 0; g < st->ngroups; g++)
    {
        int     k;

        if (!(mask & (UINT64CONST(1) << g)) || st->visited[g])
            continue;
        st->visited[g] = true;
        if (st->used[g] < st->required[g])
        {
            sdr_assign(st, p, mask, g);
            return true;
        }
        for (k = 0; k < st->nasg; k++)
        {
            if (st->asg[k].grp != g)
                continue;
            if (sdr_rehome(st, k))
            {
                sdr_assign(st, p, mask, g);     /* rehome freed one of g's units */
                return true;
            }
        }
    }
    return false;
}

/* CONTRACT event: group g lost a position off the window's left edge. Find its freed
 * instance a replacement if an augmenting path starting there exists. */
static bool
sdr_try_group(SdrState *st, int g)
{
    uint32  p;
    int     k;

    if (st->visited[g])
        return false;
    st->visited[g] = true;

    if (sdr_first_free(st, g, &p))
    {
        sdr_assign(st, p, sdr_mask_of(st, p), g);
        return true;
    }
    /* Every in-window position of g is taken. Claim one that another group holds and
     * make THAT group find itself a replacement. */
    for (k = 0; k < st->nasg; k++)
    {
        uint32  kpos = st->asg[k].pos;
        int     h = st->asg[k].grp;

        if (h == g || !(st->asg[k].mask & (UINT64CONST(1) << g)) || st->visited[h])
            continue;
        if (sdr_try_group(st, h))
        {
            /* sdr_assign only appends, so k would still be valid; re-find by position
             * so this does not silently depend on that. */
            int     k2 = sdr_find(st, kpos);

            Assert(k2 >= 0);   /* invariant */
            st->used[st->asg[k2].grp]--;
            st->asg[k2].grp = g;
            st->used[g]++;
            return true;
        }
    }
    return false;
}

static bool
sdr_sweep(const Tagged *merged, uint32 m, int ngroups, const int *required,
          const int *group_rep, const uint32 *const *pos, const uint32 *npos,
          int nslots, uint32 span_bound)
{
    SdrState   *st = (SdrState *) palloc(sizeof(SdrState));
    uint32      hi = 0,
                lo = 0;
    bool        found = false;
    int         g;

    st->merged = merged;
    st->m = m;
    st->ngroups = ngroups;
    st->required = required;
    st->nasg = 0;
    for (g = 0; g < ngroups; g++)
    {
        st->glist[g] = pos[group_rep[g]];
        st->glen[g] = npos[group_rep[g]];
        st->gstart[g] = 0;
        st->gend[g] = 0;
        st->used[g] = 0;
    }

    while (hi < m && !found)
    {
        uint32  php = merged[hi].pos;
        uint64  hmask = 0;

        CHECK_FOR_INTERRUPTS();

        /* One "event" is one DISTINCT position, i.e. one equal-position cluster of the
         * sorted stream; its mask is the set of groups that own that position. gend is
         * advanced PER STREAM ENTRY rather than once per set mask bit, so the cursor
         * stays aligned with the group's own list however many entries a group
         * contributes to the cluster. */
        while (hi < m && merged[hi].pos == php)
        {
            hmask |= UINT64CONST(1) << merged[hi].grp;
            st->gend[merged[hi].grp]++;
            hi++;
        }

        memset(st->visited, 0, sizeof(bool) * ngroups);
        (void) sdr_try_pos(st, php, hmask);

        while (st->nasg == nslots)
        {
            uint32  plo = merged[lo].pos;
            int     k;

            /* nasg == nslots means every instance is matched inside the window, so the
             * window is feasible; test its span before contracting further. lo < hi
             * holds because nslots >= 2 matched positions are in [plo, php]. */
            CHECK_FOR_INTERRUPTS();

            if (php - plo <= span_bound)
            {
                found = true;
                break;
            }
            while (lo < m && merged[lo].pos == plo)
            {
                st->gstart[merged[lo].grp]++;   /* per entry; see the gend note above */
                lo++;
            }
            k = sdr_find(st, plo);
            if (k >= 0)
            {
                int     og = st->asg[k].grp;

                sdr_unassign(st, k);
                memset(st->visited, 0, sizeof(bool) * ngroups);
                (void) sdr_try_group(st, og);
            }
        }
    }
    pfree(st);
    return found;
}

/*
 * unordered_match -- UNORDERED W/n. Some assignment of one DISTINCT position per slot
 * (any order) has window span (max - min) <= (N-1) + slop.
 *
 * ALGORITHM: minimum window with multiplicity, plus an exact fallback. The naive "one
 * cursor per slot, chase the min" sweep is WRONG for this problem (it is the
 * smallest-range-covering-k-lists sweep, which picks one element per list but cannot
 * enforce the DISTINCT-position constraint a repeated word imposes -- a fuzz check
 * against brute force flagged it).
 *
 * THE KEY FACT, AND WHAT REPLACED IT (issue #184). The counting reduction used to rest
 * on an axiom: a position is a per-(doc,field) token ordinal owned by exactly one
 * token, so two DIFFERENT phrase terms' position sets are DISJOINT and only a repeated
 * term reuses a set. Alternative groups retire the axiom -- a slot's list is the union
 * over one source word's lexemes, and a single doc position can carry lexemes matching
 * two different query words. Concretely S_0 = {5}, S_1 = {5,9}, bound 0: the counting
 * sweep finds the window [5,5] holding one entry of each group and reports a match,
 * but the only DISTINCT assignment is {5,9}, span 4. A FALSE POSITIVE -- the exact dual
 * of the false negatives the reverted doc-side-only fix produced.
 *
 * So disjointness stopped being an axiom and became a RUNTIME-CHECKED PRECONDITION:
 *   - WITHIN a group the fact survives untouched: a group's representative list is
 *     strictly ascending, so counting c entries of one group inside a window still
 *     certifies c distinct positions for that group's instances. This is the whole of
 *     what the repeated-word multiplicity rule needs.
 *   - ACROSS groups, step (3) scans the sorted stream for two ADJACENT entries sharing
 *     a position. That is an exact test for overlap, because equal positions sort
 *     adjacent and can only come from different groups. None found => the sets are
 *     pairwise disjoint FOR THIS CALL, the old proof applies verbatim, and the old
 *     counting sweep runs -- which is every call an index whose analyzer emits one
 *     lexeme per position can produce.
 *   - Overlap found => no counting shortcut is claimed at all; sdr_sweep decides the
 *     window by explicit bipartite matching.
 *
 * ordered_match never used the fact (strict increase does the distinctness work by
 * construction), which is why it needed no change -- see its own header.
 *
 * Steps (1) group by identical list and (2) merge + pg_qsort cost O(m log m); step (3)
 * is O(m); step (4)'s bounds are at sdr_sweep. See the file header for why the sort
 * that preceded pg_qsort here was not a defensible choice.
 */
static bool
unordered_match(const uint32 *const *pos, const uint32 *npos, int nterms, int slop)
{
    uint32   span_bound = (uint32) ((nterms - 1) + slop);
    int      group_rep[BM25_PHRASE_MAX_TERMS];  /* group id -> representative slot */
    int      required[BM25_PHRASE_MAX_TERMS];   /* per-group required count */
    int      ngroups = 0;
    int      t,
             g;
    Size     mcap = 0;                          /* exact merged length: sum over REPS */
    Tagged  *merged;
    uint32   i,
             m;
    bool     overlapped = false;
    bool     result;

    /* (1) Group slots by identical position list; accumulate required multiplicity.
     * group_rep[g] is the first slot assigned to group g (its list == the group's
     * occurrence set). */
    for (t = 0; t < nterms; t++)
    {
        int found = -1;

        /* nterms is capped at BM25_PHRASE_MAX_TERMS, but each trip runs up to
         * ngroups lists_equal calls and each of THOSE is O(tf) -- user-data-sized,
         * so this loop is not the trivially-bounded thing its trip count suggests.
         *
         * The check is per SLOT, not per comparison, and that is a deliberate floor
         * rather than an oversight: at the simultaneous ceiling (64 slots x 64 groups
         * x 65535 positions) it leaves a few million uint32 compares between two
         * cancellation points -- milliseconds, not the unbounded window this file's
         * standard exists to prevent. Pushing it into the inner loop would cost ~2000
         * checks per candidate document per field on the ordinary path, where every
         * list differs and ngroups grows to nterms, to buy cancellation latency
         * nothing can perceive. */
        CHECK_FOR_INTERRUPTS();

        for (g = 0; g < ngroups; g++)
        {
            int rep = group_rep[g];

            if (lists_equal(pos[rep], npos[rep], pos[t], npos[t]))
            {
                found = g;
                break;
            }
        }
        if (found < 0)
        {
            found = ngroups++;
            group_rep[found] = t;
            required[found] = 0;
        }
        required[found]++;
    }

    /* (2) Merge every group's positions (one representative list per group), tagged
     * with the group id, into one sorted stream.
     *
     * Sized by the sum over group REPRESENTATIVES, which is exactly the number of
     * entries the fill below writes. It used to be sized by the sum over ALL slots,
     * which double-counts every repeated word ("the the the" allocated 3x what it
     * filled) -- harmless at the tiny sizes the old comment assumed, but tf is
     * user-data-sized, so the over-count was a real way to push a request past
     * MaxAllocSize on input the exact size would have accepted. */
    for (g = 0; g < ngroups; g++)
        mcap += npos[group_rep[g]];
    merged = (Tagged *) palloc(sizeof(Tagged) * Max(mcap, 1));
    m = 0;
    for (g = 0; g < ngroups; g++)
    {
        int    rep = group_rep[g];

        for (i = 0; i < npos[rep]; i++)
        {
            /* Per POSITION, not per group: ngroups is capped by the phrase length,
             * npos[rep] is not, so a per-group check would leave the actual
             * unbounded work uncancellable. */
            CHECK_FOR_INTERRUPTS();

            merged[m].pos = pos[rep][i];
            merged[m].grp = g;
            m++;
        }
    }
    /* Sort by position. Equal keys are the OVERLAP signal step (3) reads, and the
     * sweeps do not care how an equal-position cluster is ordered internally, so
     * pg_qsort's instability is harmless -- see tagged_pos_cmp. */
    if (m > 1)
        pg_qsort(merged, m, sizeof(Tagged), tagged_pos_cmp);

    /* (3) Classification: are the groups' sets pairwise disjoint for THIS call? */
    for (i = 1; i < m; i++)
    {
        CHECK_FOR_INTERRUPTS();

        if (merged[i].pos == merged[i - 1].pos)
        {
            overlapped = true;
            break;
        }
    }

    /* (4) Dispatch. */
    result = overlapped
        ? sdr_sweep(merged, m, ngroups, required, group_rep, pos, npos,
                    nterms, span_bound)
        : counting_sweep(merged, m, ngroups, required, span_bound);
    pfree(merged);
    return result;
}

bool
bm25_phrase_match(const uint32 *const *pos, const uint32 *npos,
                  int nterms, bool ordered, int slop)
{
    int t;

    Assert(nterms >= 1);   /* checked: the scorer only calls with a live slot set */
    Assert(nterms <= BM25_PHRASE_MAX_TERMS);   /* checked: bm25_scan_rank.c's nq/lnt caps */

    /* An empty list for ANY slot means the doc carries none of that source word's
     * lexemes in this field: it can never phrase-match. (The scorer only calls the
     * matcher for a field in which every slot has at least one member present, so this
     * is a defensive guard, not the common exit.) */
    for (t = 0; t < nterms; t++)
        if (npos[t] == 0)
            return false;

    /* Single-slot phrase == plain term-present (the list is non-empty by the guard
     * above). Short-circuit before the sweeps (both would also return true here).
     * This is also the whole-compound query case: '"footballklubber"' analyzes to one
     * source word, so its several lexemes are ONE slot satisfied by any of them. */
    if (nterms == 1)
        return true;

    return ordered ? ordered_match(pos, npos, nterms, slop)
                   : unordered_match(pos, npos, nterms, slop);
}

/*
 * Standalone self-check. Compile with -DBM25_PHRASE_SELFTEST to exercise the
 * discriminating cases without a running server; the 38_phrase SQL suite is the
 * in-tree regression. The block below supplies the handful of backend symbols this
 * file references, so the command is self-contained apart from the server headers:
 *
 *   cc -DBM25_PHRASE_SELFTEST -I src -I "$(pg_config --includedir-server)" \
 *      -x c src/bm25_phrase.c -o /tmp/bm25_phrase_selftest && /tmp/bm25_phrase_selftest
 *
 * (The command this comment used to give omitted both -I flags and the stubs, and so
 * had not compiled since the file gained its first Assert.)
 *
 * THE OVERLAP CASES ARE THE DIRECT EVIDENCE FOR THE ALTERNATIVE-GROUP WORK. Slot sets
 * overlap only when a compound-splitting dictionary co-positions lexemes (analyzer
 * revision 5), so a default-english query cannot reach the sdr_sweep path. The cases
 * below reach it directly, and the {5} / {5,9} pair is the discriminator: the
 * counting sweep alone answers true.
 */
#ifdef BM25_PHRASE_SELFTEST
#include <assert.h>
#include <stdlib.h>
/* port.h redirects these to the backend's own implementations (pg_printf,
 * pg_fprintf, pg_qsort). The selftest binary has no backend to link, so it takes
 * libc's -- including inside the pg_qsort stub below, which would otherwise recurse. */
#undef printf
#undef fprintf
#undef qsort
#include <stdio.h>

/* Backend symbols this translation unit references. Definitions, not declarations:
 * the selftest binary links nothing else. */
volatile sig_atomic_t InterruptPending = 0;

void
ProcessInterrupts(void)
{
}

void
ExceptionalCondition(const char *conditionName, const char *fileName, int lineNumber)
{
    fprintf(stderr, "Assert(%s) failed at %s:%d\n", conditionName, fileName, lineNumber);
    abort();
}

void *
palloc(Size size)
{
    void   *p = malloc(size ? size : 1);

    assert(p != NULL);
    return p;
}

void
pfree(void *pointer)
{
    free(pointer);
}

void
pg_qsort(void *base, size_t nel, size_t elsize,
         int (*cmp) (const void *, const void *))
{
    qsort(base, nel, elsize, cmp);
}

static bool
run(const uint32 *a, int na, const uint32 *b, int nb, bool ordered, int slop)
{
    const uint32 *pos[2] = {a, b};
    uint32        npos[2] = {(uint32) na, (uint32) nb};
    return bm25_phrase_match(pos, npos, 2, ordered, slop);
}

/* ---- brute force, for the fuzz below -------------------------------------
 * Try every choice of one position per slot; accept iff the choices are pairwise
 * DISTINCT and (ordered ? strictly increasing : true) and the span fits. Exponential,
 * so the fuzz keeps the lists tiny. */
static bool
brute(const uint32 *const *pos, const uint32 *npos, int n, bool ordered, int slop,
      uint32 *pick, int depth)
{
    uint32  i;

    if (depth == n)
    {
        uint32  lo = pick[0],
                hi = pick[0];
        int     a,
                b;

        for (a = 0; a < n; a++)
            for (b = a + 1; b < n; b++)
                if (pick[a] == pick[b])
                    return false;
        if (ordered)
            for (a = 1; a < n; a++)
                if (pick[a] <= pick[a - 1])
                    return false;
        for (a = 1; a < n; a++)
        {
            if (pick[a] < lo)
                lo = pick[a];
            if (pick[a] > hi)
                hi = pick[a];
        }
        return (hi - lo) <= (uint32) ((n - 1) + slop);
    }
    for (i = 0; i < npos[depth]; i++)
    {
        pick[depth] = pos[depth][i];
        if (brute(pos, npos, n, ordered, slop, pick, depth + 1))
            return true;
    }
    return false;
}

/* xorshift32, so the fuzz is reproducible on every platform. */
static uint32 rng_state = 0x2b7e1516u;

static uint32
rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void
fuzz_vs_brute(void)
{
    int     iter;

    for (iter = 0; iter < 200000; iter++)
    {
        uint32          buf[4][8];
        const uint32   *pos[4];
        uint32          npos[4];
        uint32          pick[4];
        int             n = 2 + (int) (rng() % 3);      /* 2..4 slots */
        int             slop = (int) (rng() % 5);       /* 0..4 */
        bool            ordered = (rng() & 1) != 0;
        int             s,
                        v;
        bool            empty = false;

        /* Build each slot as a strictly ascending subset of 0..7 -- small and dense,
         * so overlapping and equal slot sets both come up constantly. */
        for (s = 0; s < n; s++)
        {
            npos[s] = 0;
            for (v = 0; v < 8; v++)
                if (rng() & 1)
                    buf[s][npos[s]++] = (uint32) v;
            pos[s] = buf[s];
            if (npos[s] == 0)
                empty = true;
        }
        if (empty)
            continue;               /* bm25_phrase_match's empty-list guard, not the sweeps */

        assert(bm25_phrase_match(pos, npos, n, ordered, slop) ==
               brute(pos, npos, n, ordered, slop, pick, 0));
    }
}

static void
slot_map_cases(void)
{
    BM25Token           toks[6];
    BM25PhraseSlotMap   map;
    int                 i;

    /* The identity map: what bm25_analyze produces when every run yields one lexeme
     * (one position per token). Every phrase behaviour then reduces to the pre-slot
     * behaviour. */
    for (i = 0; i < 6; i++)
    {
        toks[i].ptr = NULL;
        toks[i].len = 0;
        toks[i].pos = i;
        toks[i].src_off = -1;
        toks[i].src_len = 0;
    }
    bm25_phrase_slot_map(toks, 6, &map);
    assert(map.nslots == 6);
    for (i = 0; i <= 6; i++)
        assert(map.slot_start[i] == i);

    /* Compound shape: "footballklubber yesterday" -- five lexemes at position 0 and
     * one at position 1, i.e. two slots. */
    toks[0].pos = toks[1].pos = toks[2].pos = toks[3].pos = toks[4].pos = 0;
    toks[5].pos = 1;
    bm25_phrase_slot_map(toks, 6, &map);
    assert(map.nslots == 2);
    assert(map.slot_start[0] == 0 && map.slot_start[1] == 5 && map.slot_start[2] == 6);

    /* Degenerate but legal inputs. */
    bm25_phrase_slot_map(toks, 0, &map);
    assert(map.nslots == 0 && map.slot_start[0] == 0);
    bm25_phrase_slot_map(toks, 1, &map);
    assert(map.nslots == 1 && map.slot_start[1] == 1);
}

static void
merge_cases(void)
{
    uint32          a[] = {0, 4, 9};
    uint32          b[] = {0, 4};              /* fully duplicated by a */
    uint32          c[] = {2, 4, 11};
    const uint32   *lists[3] = {a, b, c};
    uint32          lens[3] = {3, 2, 3};
    uint32          out[8];
    uint32          n;

    n = bm25_phrase_merge_lists(lists, lens, 3, out);
    assert(n == 5);
    assert(out[0] == 0 && out[1] == 2 && out[2] == 4 && out[3] == 9 && out[4] == 11);

    /* Singleton: the zero-copy case the recheck takes for every non-compound query. */
    n = bm25_phrase_merge_lists(lists, lens, 1, out);
    assert(n == 3 && out[0] == 0 && out[1] == 4 && out[2] == 9);

    /* An empty member list contributes nothing and must not stall the heap. */
    {
        uint32          e[1];
        const uint32   *l2[2] = {e, a};
        uint32          n2[2] = {0, 3};

        n = bm25_phrase_merge_lists(l2, n2, 2, out);
        assert(n == 3 && out[0] == 0 && out[1] == 4 && out[2] == 9);
    }
}

/* Slot sets that OVERLAP without being equal -- the shape only per-run positions can
 * produce, and the shape the counting sweep answers WRONGLY. */
static void
overlap_cases(void)
{
    /* THE DISCRIMINATOR. S_0={5}, S_1={5,9}. The counting sweep finds the window
     * [5,5] with one entry of each group and says true; the only distinct assignment
     * is (5,9), span 4. Against the unmodified matcher this assert fires. */
    {
        uint32 a[] = {5}, b[] = {5, 9};
        assert(run(a, 1, b, 2, false, 0) == false);
        assert(run(a, 1, b, 2, false, 2) == false);  /* span 4 > 1+2 */
        assert(run(a, 1, b, 2, false, 3) == true);   /* span 4 <= 1+3 */
        assert(run(a, 1, b, 2, true, 3) == true);    /* 5 then 9, ordered */
        assert(run(b, 2, a, 1, true, 3) == false);   /* nothing of S_0 after 5 */
    }
    /* Two slots collapsing to ONE shared position: '"football klubber"'~0 against a
     * doc whose only occurrence is the compound. Equal lists => one group, required 2,
     * one distinct position => no match, ordered or not. */
    {
        uint32 s[] = {0};
        const uint32 *pos[2] = {s, s};
        uint32        npos[2] = {1, 1};

        assert(bm25_phrase_match(pos, npos, 2, true, 0) == false);
        assert(bm25_phrase_match(pos, npos, 2, false, 100) == false);
    }
    /* Compound fills ONE slot: '"footballklubber yesterday"'. S_0 is the union of the
     * compound's five lexemes (all at position 0), S_1 is the next word. Exact match --
     * the query the doc-side-only fix turned into zero rows. */
    {
        uint32 s0[] = {0}, s1[] = {1};
        assert(run(s0, 1, s1, 1, true, 0) == true);
    }
    /* Multiplicity mixed with overlap: S_0 == S_1 == {3,4} (required 2) and
     * S_2 = {4,9}. The only distinct assignment is {3,4} for the pair and 9 for the
     * third, span 6, so it needs slop >= 4 against a bound of (3-1)+slop. */
    {
        uint32 ab[] = {3, 4}, c[] = {4, 9};
        const uint32 *pos[3] = {ab, ab, c};
        uint32        npos[3] = {2, 2, 2};

        assert(bm25_phrase_match(pos, npos, 3, false, 3) == false);
        assert(bm25_phrase_match(pos, npos, 3, false, 4) == true);
    }
    /* Chained displacement: three slots whose sets overlap pairwise but admit exactly
     * one distinct assignment (0,1,2). Exercises sdr_rehome's recursion rather than
     * its first-try branch. */
    {
        uint32 s0[] = {0, 1}, s1[] = {1, 2}, s2[] = {0, 2};
        const uint32 *pos[3] = {s0, s1, s2};
        uint32        npos[3] = {2, 2, 2};

        assert(bm25_phrase_match(pos, npos, 3, false, 0) == true);   /* span 2 <= 2 */
    }
    /* Overlap that is NOT satisfiable at any slop: three slots sharing two positions
     * between them. Hall's condition fails for the whole set, so no window works. */
    {
        uint32 s0[] = {0, 1}, s1[] = {0, 1}, s2[] = {0, 1};
        const uint32 *pos[3] = {s0, s1, s2};
        uint32        npos[3] = {2, 2, 2};

        assert(bm25_phrase_match(pos, npos, 3, false, 100000) == false);
    }
    /* The window has to CONTRACT past a matched position and re-augment: the only
     * feasible triple is (10,11,12) far to the right of a decoy cluster at 0. */
    {
        uint32 s0[] = {0, 10}, s1[] = {0, 11}, s2[] = {0, 12};
        const uint32 *pos[3] = {s0, s1, s2};
        uint32        npos[3] = {2, 2, 2};

        assert(bm25_phrase_match(pos, npos, 3, false, 0) == true);   /* 10,11,12 */
        assert(bm25_phrase_match(pos, npos, 3, false, 100) == true);
    }
}

int
main(void)
{
    /* EXACT "a b": matches only adjacent (a@0, b@1). */
    {
        uint32 a[] = {0}, b[] = {1};
        assert(run(a, 1, b, 1, true, 0) == true);
    }
    {
        uint32 a[] = {0}, b[] = {2};        /* gap 2, not adjacent */
        assert(run(a, 1, b, 1, true, 0) == false);
    }
    /* reversed pair a@2 b@0: unordered ~3 matches, ordered ~>3 does NOT. */
    {
        uint32 a[] = {2}, b[] = {0};
        assert(run(a, 1, b, 1, false, 3) == true);   /* W/3: span 2 <= 1+3 */
        assert(run(a, 1, b, 1, true, 3) == false);   /* PRE/3: b must follow a */
    }
    /* ordered within slop: a@0 b@3 -> span 3, (3-1)=2 <= slop 3 ok; slop 1 fails. */
    {
        uint32 a[] = {0}, b[] = {3};
        assert(run(a, 1, b, 1, true, 3) == true);
        assert(run(a, 1, b, 1, true, 1) == false);
    }
    /* repeated term "the the": needs two DISTINCT adjacent positions. */
    {
        uint32 the[] = {3, 4};              /* two adjacent occurrences */
        const uint32 *pos[2] = {the, the};
        uint32 npos[2] = {2, 2};
        assert(bm25_phrase_match(pos, npos, 2, true, 0) == true);   /* 3,4 adjacent */
    }
    {
        uint32 the[] = {3, 9};              /* two far-apart occurrences, span 6 */
        const uint32 *pos[2] = {the, the};
        uint32 npos[2] = {2, 2};
        assert(bm25_phrase_match(pos, npos, 2, true, 0) == false);  /* not adjacent */
        assert(bm25_phrase_match(pos, npos, 2, false, 4) == false); /* span 6 > 1+4 */
        assert(bm25_phrase_match(pos, npos, 2, false, 5) == true);  /* span 6 <= 1+5 */
    }
    {
        uint32 the[] = {5};                 /* only ONE occurrence: "the the" fails */
        const uint32 *pos[2] = {the, the};
        uint32 npos[2] = {1, 1};
        assert(bm25_phrase_match(pos, npos, 2, true, 0) == false);
        assert(bm25_phrase_match(pos, npos, 2, false, 100) == false);
    }
    /* single-term phrase == plain term. */
    {
        uint32 a[] = {7};
        const uint32 *pos[1] = {a};
        uint32 npos[1] = {1};
        assert(bm25_phrase_match(pos, npos, 1, true, 0) == true);
    }
    /* empty list => no match. */
    {
        uint32 a[] = {0};
        const uint32 *pos[2] = {a, a};
        uint32 npos[2] = {1, 0};
        assert(bm25_phrase_match(pos, npos, 2, true, 0) == false);
    }
    /* 3-term exact "a b c" at 0,1,2 matches; at 0,1,3 fails exact but ordered ~1 ok. */
    {
        uint32 a[] = {0}, b[] = {1}, c[] = {2};
        const uint32 *pos[3] = {a, b, c};
        uint32 npos[3] = {1, 1, 1};
        assert(bm25_phrase_match(pos, npos, 3, true, 0) == true);
    }
    {
        uint32 a[] = {0}, b[] = {1}, c[] = {3};
        const uint32 *pos[3] = {a, b, c};
        uint32 npos[3] = {1, 1, 1};
        assert(bm25_phrase_match(pos, npos, 3, true, 0) == false); /* span 3 != 2 */
        assert(bm25_phrase_match(pos, npos, 3, true, 1) == true);  /* (3-0)-2=1<=1 */
    }
    slot_map_cases();
    merge_cases();
    overlap_cases();
    fuzz_vs_brute();
    printf("bm25_phrase self-check OK\n");
    return 0;
}
#endif
