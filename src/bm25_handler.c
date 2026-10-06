/* bm25_handler.c -- registers the bm25 index access method.
 * bm25_handler() below allocates the IndexAmRoutine with makeNode, which ZEROES
 * it, then assigns every callback this AM implements plus an explicit NULL for
 * the ten optional slots listed here -- NOT for every optional slot it declines
 * (see the next paragraph). None of those ten explicit NULLs is a stub awaiting
 * a later milestone: amcanreturn/amgetbitmap -- no index-only scan or bitmap
 * scan support; amproperty/ambuildphasename -- no custom property reporting or
 * build-phase names; amadjustmembers -- no AM-specific vetting of the operator
 * and support-function members proposed by CREATE OPERATOR CLASS or ALTER
 * OPERATOR FAMILY ... ADD, and no adjustment of the dependency types core
 * initializes for them (hard on the opclass for the former, soft on the
 * opfamily for the latter). amapi.h calls it "validate operators and support
 * functions to be added to an opclass/family"; leaving it NULL keeps those core
 * defaults, and this AM's opclass checking lives in amvalidate, which unlike
 * amadjustmembers sees the complete member set. ammarkpos/amrestrpos -- no
 * mark/restore; the three parallel-scan entries -- amcanparallel is false.
 *
 * NOT a fully-populated struct, and stated rather than implied: the remaining
 * optional slots are never assigned at all and are NULL/false only because
 * makeNode zeroed the node -- aminsertcleanup and amgettreeheight among the
 * callbacks, and amtranslatestrategy/amtranslatecmptype plus the newer bool
 * flags (amcanhash, amconsistentequality, amconsistentordering) where the server
 * headers define them; amcanbuildparallel is also unassigned, but it exists on the
 * PG 17 floor. Core reads an unassigned optional callback and an explicit NULL
 * identically, and zero is the
 * correct (or, for the flags, the conservative) answer for each, so this is a
 * documentation distinction, not a behavioural one. It is called out because the
 * set of slots differs across the two majors this AM builds against (PG 17 floor,
 * see bm25.h; PG 18 adds to it), so an exhaustive per-slot claim here would rot. */
#include "postgres.h"

#include "bm25.h"
#include "access/amvalidate.h"
#include "access/htup_details.h"  /* GETSTRUCT (amvalidate catalog tuples) */
#include "access/reloptions.h"
#include "catalog/pg_amop.h"      /* Form_pg_amop, AMOP_SEARCH/AMOP_ORDER */
#include "catalog/pg_amproc.h"    /* Form_pg_amproc */
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"  /* Form_pg_opfamily (opfamily name, PG17-portable) */
#include "catalog/pg_type.h"      /* BOOLOID, FLOAT8OID */
#include "commands/defrem.h"     /* defGetString (per-field knob filter) */
#include "commands/vacuum.h"
#include "miscadmin.h"           /* CHECK_FOR_INTERRUPTS */
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"       /* List/foreach (per-field knob filter) */
#include "storage/indexfsm.h"     /* IndexFreeSpaceMapVacuum (every cleanup, issue #300) */
#include "storage/latch.h"       /* SetLatch (debug_cancel_at) */
#include "storage/lmgr.h"        /* LockPage/UnlockPage (bm25_bulkdelete's whole-pass hold) */
#include "storage/lock.h"        /* LockAcquire/LockRelease (debug_pause); PG19 lmgr.h no longer pulls it in */
#include "utils/guc.h"
#include "utils/jsonb.h"         /* PG_GETARG_JSONB_P (jsonb distance anchor, #138) */
#include "utils/builtins.h"      /* format_type_be (HDL-02 opcintype message) */
#include "utils/lsyscache.h"     /* getBaseType -- a domain over text is indexable */
#include "utils/regproc.h"       /* format_operator/format_procedure */
#include "bm25_query.h"          /* bm25_query_validate (#245 off-index jsonb &@@ check) */
#include "bm25_wand.h"           /* BM25_WAND_TOP_K_MAX (the wand_top_k GUC bound) */
#include "utils/syscache.h"      /* SearchSysCache1, CLAOID, AMOPSTRATEGY */
#include <limits.h>

/* The BM25_VACUUM_DELAY_POINT macro moved to bm25.h during the maintenance-interrupts
 * pass so bm25_reclaim_orphans (bm25_fsm.c) can share the same macro instead
 * of duplicating (and risking drifting) the PG_VERSION_NUM guard. Named here
 * without its parentheses on purpose: CI's interrupt floor counts the call form,
 * and this comment used to be counted as a check. */

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(bm25_handler);

/* M2b: bm25_native.wand_top_k GUC backing variable. Declared extern in bm25.h;
 * defined here (rather than a WAND-specific .c file) because it is a
 * process-wide tunable registered at module load, alongside the AM's other
 * GUC (bm25_native.seal_threshold) -- not owned by any one scan-time translation
 * unit. Default 100 keeps WAND on by default (a design decision since M2b). */
int bm25_wand_top_k = 100;

/* M6: wildcard expansion guardrails (D9 of the M6 design), declared extern in
 * bm25.h. bm25_wildcard_min_prefix is enforced at PARSE time (bm25_query_parse,
 * bm25_query.c) so a short literal prefix ERRORs identically on the @@@ filter
 * and &@@ ranked paths; bm25_wildcard_max_expansions is consumed later, by the
 * wildcard expander -- registering both GUCs now (rather than splitting their
 * registration across tasks) keeps every bm25_native.* GUC defined in one place. */
int bm25_wildcard_min_prefix     = 3;
int bm25_wildcard_max_expansions = 1000;
/* Maintenance-interrupts pass: validate_wildcard_pattern (bm25_query.c) had no
 * bound on pattern length or star count, so bm25_glob_match's O(pattern * term)
 * matcher could be handed unbounded work by the text of the pattern alone, before
 * min_prefix/max_expansions ever come into play. Same PGC_SUSET class as those
 * two -- see the registration comment below. */
int bm25_wildcard_max_pattern_length = 256;
int bm25_wildcard_max_stars          = 8;

/* #62.5: the match-set materialization budget, in KB. 256 MB (~1.7M documents at
 * bm25_scan.c's ~160 bytes/doc) rather than work_mem's 4 MB default, because the
 * two are not measuring comparable things: work_mem sizes ONE of a plan's several
 * spillable operations, while this sizes the entire result of an index scan that
 * cannot spill at all. Tying the ceiling to work_mem's default would have turned
 * an ordinary broad query over a 100k-document corpus into a hard error on
 * upgrade -- suite 66_scan_interrupts, which matches 100,000 documents, failed
 * exactly that way while this was being written (ADR 0047). */
int bm25_max_match_memory_kb         = 256 * 1024;

/* BUILD-04 test lever; 0 = off, i.e. follow maintenance_work_mem. See the
 * registration in _PG_init for why this exists and why it is PGC_SUSET. */
int bm25_debug_budget_kb             = 0;

/* Issue #289 test lever; false = the writer cuts posting blocks at document
 * boundaries. See the registration in _PG_init. */
bool bm25_debug_count_slicing        = false;

/* bm25_native.debug_pause backing variable and its pause-point table. A point's
 * advisory-lock key2 is its 1-based position here, so APPEND ONLY: reordering
 * silently re-keys every test that parks on a later point. */
char *bm25_debug_pause                = NULL;

/* bm25_native.debug_cancel_at backing variable: pause points at which this backend
 * raises a query cancel against itself (issue #305). Same name table and check hook
 * as debug_pause. */
char *bm25_debug_cancel_at            = NULL;

/* bm25_native.debug_cancel_after: the work-unit count (bm25_work_units) this backend
 * must have reached before a debug_cancel_at point fires. 0 = the first hit. */
int bm25_debug_cancel_after          = 0;

static const char *const bm25_debug_pause_points[] = {
    "bulkdelete_start",         /* 1: catalog snapshot taken, no segment tombstoned */
    "bulkdelete_segment",       /* 2: first segment tombstoned, second not started */
    "bulkdelete_pending",       /* 3: every segment done, pending sweep not started */
    "merge_preswap",            /* 4: inputs' LIVEDOCS replayed, swap not started */
    "swap_after_snapshot",      /* 5: publish_swap's Step A catalog copy taken, no flip */
    "insert_keycheck",          /* 6: INSERT key check has catalog entry 0, header unread */
    "scan_pending_page",        /* 7: @@@ term walk read one pending page, next unread */
    "orphan_sweep_start",       /* 8: orphan sweep holds the singleton, nothing marked */
    "merge_start",              /* 9: bm25_merge_maybe entered, no heap or singleton lock */
    "pending_append_alloc",     /* 10: append allocated its new page, nothing written yet */
    "orphan_sweep_marked",      /* 11: share-mode orphan sweep marked, nothing swept */
    "reclaim_retired_compacted",/* 12: a retired descriptor compacted, its ranges not freed */
    "reclaim_retired_between_chunks", /* 13: a reclaim chunk done, singleton released */
    "merge_between_passes",     /* 14: a forced merge pass swapped, singleton released */
    "rank_build_attempt",       /* 15: a ranking build attempt entered, nothing read */
    "keymap_rotation",          /* 16: a KEYMAP page finished, its successor not allocated */
    "scan_post_page",           /* 17: a POST page about to be decoded, no content lock held */
    "drain_pending_page",       /* 18: a pending page about to be drained, no buffer held */
    "debug_dict_entry",         /* 19: a DICT entry about to be replayed, its page copied */
};

/* Is `point` one of the comma-separated names in `list`? Spaces around a name are
 * ignored. A list, not one name, because some interleavings need one backend parked
 * twice -- a VACUUM held after its seal and again inside its orphan sweep (t/031). */
static bool
bm25_debug_pause_listed(const char *list, const char *point, size_t pointlen)
{
    const char *p = list;

    while (*p != '\0')
    {
        const char *end;
        const char *q;

        while (*p == ' ')
            p++;
        end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        q = end;
        while (q > p && q[-1] == ' ')
            q--;
        if ((size_t) (q - p) == pointlen && strncmp(p, point, pointlen) == 0)
            return true;
        p = (*end == ',') ? end + 1 : end;
    }
    return false;
}

/* Only listed names are accepted, for the same reason the namespace is reserved
 * below: a typo'd point would otherwise be a pause that silently never fires, and a
 * TAP test would then pass by NOT exercising the interleaving it names. Every name
 * of a comma-separated list is checked. */
static bool
bm25_debug_pause_check(char **newval, void **extra, GucSource source)
{
    const char *p = *newval;

    while (*p != '\0')
    {
        const char *end = strchr(p, ',');
        const char *a = p;
        const char *z;
        bool        known = false;
        int         i;

        if (end == NULL)
            end = p + strlen(p);
        z = end;
        while (a < z && *a == ' ')
            a++;
        while (z > a && z[-1] == ' ')
            z--;
        for (i = 0; i < (int) lengthof(bm25_debug_pause_points); i++)
            if (strlen(bm25_debug_pause_points[i]) == (size_t) (z - a) &&
                strncmp(a, bm25_debug_pause_points[i], z - a) == 0)
                known = true;
        if (!known)
        {
            GUC_check_errdetail("Unknown bm25 pause point \"%.*s\".", (int) (z - a), a);
            return false;
        }
        p = (*end == ',') ? end + 1 : end;
    }
    return true;
}

/* Park here while someone holds pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N).
 *
 * Acquire-then-release of a ShareLock, never a held lock: the pause must leave no
 * lock behind for the rest of the operation, and ShareLock against the holder's
 * ExclusiveLock is what makes it wait. The wait is an ordinary lmgr sleep, so it is
 * cancellable, visible as an ungranted 'advisory' row in pg_locks (which is how a
 * test knows the backend reached the point), and seen by the deadlock detector.
 * Callers must hold no buffer lock here -- every call site is between page visits --
 * with one deliberate exception: pending_append_alloc parks an INSERT holding three
 * content locks, uncancellable, and its only suite (t/028) ends it with an immediate
 * shutdown. */
void
bm25_debug_pause_point(const char *point)
{
    int         i;
    LOCKTAG     tag;

    /* debug_cancel_at: make a cancel arrive at exactly this step, in a single session.
     * These are the assignments StatementCancelHandler makes on SIGINT, so the backend
     * is left in the state a real pg_cancel_backend would leave it in, and the next
     * CHECK_FOR_INTERRUPTS that can act on it does. Whether one can, before the step's
     * work is over, is what a suite using this asserts -- a pause cannot show that
     * without a second session, and a cancel that lands while the backend is parked
     * is serviced by the lock wait, not by the code under test.
     *
     * debug_cancel_after holds the cancel back until the work counter has reached it,
     * so it lands MID-loop. A first-hit cancel only shows that the loop's FIRST check
     * is live, and a lock taken during the first iteration and held across the rest
     * would leave that one check live and every later one dead.
     *
     * Both GUCs default to '', so the common case is a NULL check and a first-byte
     * test on each: this runs per POST page on every scan, and per page drained or
     * pending page scanned. */
    if (bm25_debug_cancel_at != NULL && bm25_debug_cancel_at[0] != '\0' &&
        bm25_work_units >= (uint64) bm25_debug_cancel_after &&
        bm25_debug_pause_listed(bm25_debug_cancel_at, point, strlen(point)))
    {
        InterruptPending = true;
        QueryCancelPending = true;
        SetLatch(MyLatch);
    }

    if (bm25_debug_pause == NULL || bm25_debug_pause[0] == '\0' ||
        !bm25_debug_pause_listed(bm25_debug_pause, point, strlen(point)))
        return;
    for (i = 0; i < (int) lengthof(bm25_debug_pause_points); i++)
        if (strcmp(point, bm25_debug_pause_points[i]) == 0)
            break;
    Assert(i < (int) lengthof(bm25_debug_pause_points));    /* checked: the GUC's check hook */

    /* The same tag pg_advisory_lock(int4, int4) builds (SET_LOCKTAG_INT32). */
    SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32) BM25_DEBUG_PAUSE_LOCKKEY,
                         (uint32) (i + 1), 2);
    (void) LockAcquire(&tag, ShareLock, false, false);
    LockRelease(&tag, ShareLock, false);
}

/* Module init: register the bm25_native.* GUCs. Called once at library load.
 * Not declared here: since PG 15 fmgr.h declares _PG_init centrally, as
 * `extern PGDLLEXPORT void _PG_init(void)`, precisely so extensions need not --
 * and a local declaration WITHOUT PGDLLEXPORT, if it were ever to precede
 * fmgr.h, is what breaks the Windows build. fmgr.h arrives via bm25.h. */
void
_PG_init(void)
{
    /* seal_threshold is PGC_USERSET in BOTH directions (#310 SURFACE-08). Raising it
     * defers seals and is the case the PGC_SUSET comment further down discusses.
     * LOWERING it is the same class: the 64 kB floor is 64 times below the 4 MB
     * default, so a role that can only INSERT can make each insert batch seal a
     * segment far smaller than the default would, multiplying the segment count every
     * scan of the index pays for. That damage is bounded by the role's own write
     * volume, corrupts nothing, and is recoverable: a VACUUM-time merge (or
     * bm25_merge()) consolidates the small segments. It is accepted on the same
     * "bounded and VACUUM-recoverable" premise as the raising direction, and the floor
     * is deliberately not raised -- a 1 MB floor would still leave a 4x lever while
     * breaking every test and bulk-load recipe that uses a smaller value. */
    DefineCustomIntVariable("bm25_native.seal_threshold",
        "Pending-list size (KB) above which an opportunistic seal fires.",
        NULL,
        &bm25_seal_threshold_kb,
        4096, 64, INT_MAX,
        PGC_USERSET, GUC_UNIT_KB,
        NULL, NULL, NULL);

    /* HDL-04: the upper bound is BM25_WAND_TOP_K_MAX, not INT_MAX.
     *
     * This is the one memory consumer bm25_native.max_match_memory does not cover,
     * and the reasoning that makes THAT knob's PGC_USERSET acceptable -- "raising it
     * only lets a query the user could already write consume what it was consuming
     * before the bound existed" -- does not transfer: without the GUC there is no
     * query a user can write that makes a ten-row table allocate a gigabyte. The
     * allocation it drove was a pure function of the GUC, not of the corpus or the
     * query, so it fired before any posting was read.
     *
     * Both halves of the fix are needed and they cover different cases. QRY-10 made
     * the heap's array grow on demand, so a large-but-legal wand_top_k now costs
     * what the query actually produces -- that is what removes the ten-row-table
     * gigabyte. This bound covers the rest: past BM25_WAND_TOP_K_MAX the drained
     * BM25Scored array cannot fit one palloc at all, and without the bound that
     * surfaced from inside a scan as palloc's anonymous XX000 "invalid memory alloc
     * request size" instead of a parameter-named limit. Rejecting in the GUC
     * machinery reports it at SET time, naming bm25_native.wand_top_k.
     *
     * PGC_USERSET is retained: with the eager allocation gone the knob is once again
     * the work_mem-style bargain the block below describes. */
    DefineCustomIntVariable("bm25_native.wand_top_k",
        "Top-N size for block-max WAND acceleration of ranked (&@@) scans; 0 disables.",
        NULL,
        &bm25_wand_top_k,
        100, 0, BM25_WAND_TOP_K_MAX,
        PGC_USERSET, 0,
        NULL, NULL, NULL);

    /* PGC_USERSET, deliberately, even though an over-large value lets one backend
     * consume a lot of memory: that is precisely work_mem's own bargain, and this
     * knob is the same kind of thing. It differs from the wildcard guardrails
     * below, which decide whether a query may RUN at all and so cannot be in the
     * caller's gift -- raising this one only lets a query the user could already
     * write consume what it was consuming before the bound existed (#62.5).
     *
     * There is no separate "unbounded" value: 0 means "follow work_mem", and the
     * closest thing to the old unbounded behaviour is MAX_KILOBYTES. Making 0 mean
     * unbounded would have made the safe-looking reading of a zero the dangerous one.
     *
     * "Closest thing", not "the same thing": MAX_KILOBYTES is INT_MAX on a 64-bit
     * build (2 TB, which no relation's match set can reach) but INT_MAX/1024 on a
     * 32-bit one (2 GB, ~17M documents). Truly unbounded is no longer reachable, and
     * that is intentional -- unbounded is the defect. */
    DefineCustomIntVariable("bm25_native.max_match_memory",
        "Memory one bm25 scan may use to materialize its match set; 0 follows work_mem.",
        "The exhaustive scorer and the non-scoring @@@ union both hold every matching "
        "document in memory and neither can spill to disk, so a query that would exceed "
        "this errors instead of growing without bound. Values above roughly 5GB do not "
        "raise the ceiling: a plain palloc of the scorer's drain array reaches "
        "MaxAllocSize first, at about 33.5 million matched documents, and reports the "
        "generic allocation error this budget exists to replace. The non-scoring @@@ "
        "union stops at the same count of term-document postings, which it reaches at "
        "about 1.5GB of budget, and reports its own program-limit error. See ADR 0047.",
        &bm25_max_match_memory_kb,
        256 * 1024, 0, MAX_KILOBYTES,
        PGC_USERSET, GUC_UNIT_KB,
        NULL, NULL, NULL);

    /* PGC_SUSET, unlike the three knobs above. These are not tuning knobs -- they are
     * the only bound on how much work one wildcard pattern may demand, and a
     * PGC_USERSET guardrail is not a guardrail: any user could
     * `SET bm25_native.wildcard_min_prefix = 0; SET ..._max_expansions = 1000000;`
     * and then make `bm25_wildcard('body','*')` expand to a million dictionary
     * terms. The knobs the user MAY move decide how much work the user's own
     * session does; these two decide whether a query is allowed to exist at all.
     *
     * That distinction is the real basis for the split, and it is NOT "the USERSET
     * knobs affect only the setting session" (HDL-06 -- this comment used to say
     * that, and it was wrong about seal_threshold). wand_top_k and
     * max_match_memory are genuinely session-local, but seal_threshold is read by
     * the WRITER and decides whether shared on-disk state changes:
     * bm25_pending_should_seal compares the INDEX's pending-list size against the
     * inserting session's value, so `SET bm25_native.seal_threshold = '2TB'`
     * followed by a bulk load leaves the index's pending list unsealed
     * indefinitely -- and every OTHER session's scan then walks that whole unsealed
     * list, and the index grows, until a VACUUM or a manual bm25_seal() runs.
     *
     * It stays PGC_USERSET regardless, because the damage is bounded and
     * self-correcting (the next VACUUM seals) and because deferring one's own
     * seals is a legitimate bulk-load tactic. But the justification is "bounded and
     * recoverable", not "session-local" -- and getting that wrong is what would let
     * the next knob be misclassified by analogy with a property seal_threshold does
     * not actually have.
     *
     * PGC_SUSET blocks both directions, so a non-superuser cannot TIGHTEN them
     * either. That is accepted: an administrator who wants to delegate can
     * `GRANT SET ON PARAMETER bm25_native.wildcard_min_prefix TO <role>` (PG 15+),
     * which is the supported way to hand out a SUSET knob without superuser. */
    DefineCustomIntVariable("bm25_native.wildcard_min_prefix",
        "Minimum literal-prefix length, in bytes, a wildcard query pattern must supply; shorter is an ERROR.",
        NULL,
        &bm25_wildcard_min_prefix,
        3, 0, 64,
        PGC_SUSET, 0,
        NULL, NULL, NULL);

    DefineCustomIntVariable("bm25_native.wildcard_max_expansions",
        "Maximum distinct dictionary terms a wildcard query may expand to; more is an ERROR.",
        NULL,
        &bm25_wildcard_max_expansions,
        1000, 1, 1000000,
        PGC_SUSET, 0,
        NULL, NULL, NULL);

    /* Same PGC_SUSET class as the pair above: these bound bm25_glob_match's own
     * O(pattern * term) matching cost, not the dictionary-expansion cost the two
     * GUCs above already guard. A non-superuser could otherwise submit a
     * megabyte-scale pattern via bm25_wildcard() and force full-cost matching
     * against every candidate term.
     *
     * These do NOT reach bm25_debug_glob_match: that SQL function calls
     * bm25_glob_match directly and never goes through validate_wildcard_pattern
     * (the whole point of a debug SRF is to exercise the primitive without the
     * query-tree machinery around it), so neither this pattern-length cap nor
     * the star-count one below apply to it. Its cancellability instead comes
     * from the CHECK_FOR_INTERRUPTS bm25_glob_match's main loop gained in this
     * same pass, and access to it at all is gated by the bm25_debug_ REVOKE
     * loop (ADR 0020), not by anything here. */
    DefineCustomIntVariable("bm25_native.wildcard_max_pattern_length",
        "Maximum byte length of a wildcard query pattern, applied to BOTH the pattern as "
        "written and its case-folded form; longer is an ERROR.",
        NULL,
        &bm25_wildcard_max_pattern_length,
        256, 1, 8192,
        PGC_SUSET, 0,
        NULL, NULL, NULL);

    DefineCustomIntVariable("bm25_native.wildcard_max_stars",
        "Maximum number of '*' wildcards in one query pattern; more is an ERROR.",
        NULL,
        &bm25_wildcard_max_stars,
        8, 1, 64,
        PGC_SUSET, 0,
        NULL, NULL, NULL);

    /* bm25_native.debug_budget -- the maintenance memory budget, overridden.
     *
     * WHY IT EXISTS. The build, merge and drain accumulators chunk when they cross
     * bm25_maintenance_budget_bytes (BUILD-04). Reaching a chunk boundary through
     * the real knob means either a corpus in the tens of megabytes or
     * maintenance_work_mem at its GUC floor of 1 MB -- and the floor is not low
     * enough, because it sits only just above the accumulator's own fixed baseline,
     * so a suite driven that way would need a corpus large enough to make the whole
     * regression run expensive. 64 KB here reaches every chunk path on a hundred-row
     * corpus in milliseconds. This is the same doctrine sql/83 states for
     * max_match_memory: shrink the budget, never grow the corpus.
     *
     * A pure-function debug probe cannot substitute. What has to be exercised is an
     * end-to-end chunked PUBLISH -- N orphan segments and one atomic record -- which
     * only a real CREATE INDEX / bm25_merge() / bm25_seal() produces.
     *
     * WHY PGC_SUSET, and why it must stay that way. Unlike wand_top_k or
     * max_match_memory, this does not bound the setting session's own work: it
     * changes the PHYSICAL LAYOUT of shared on-disk state, since a lowered budget
     * makes the next seal or merge publish more, smaller segments that every other
     * session then scans. It is also the one knob that can make the segment count
     * grow without limit. maintenance_work_mem is the supported contract and this is
     * not a second way to spell it; it is named debug_, documented as debug-only,
     * and left where an administrator can still delegate it with GRANT SET ON
     * PARAMETER if a support case needs it. Do not widen it to PGC_USERSET, and do
     * not present it as a tuning knob. (ADR 0084.)
     *
     * MAX_KILOBYTES rather than INT_MAX so the KB-to-bytes multiply in
     * bm25_maintenance_budget_bytes cannot overflow Size on a 32-bit build -- the
     * same ceiling max_match_memory uses. */
    DefineCustomIntVariable("bm25_native.debug_budget",
        "Overrides maintenance_work_mem as the build/merge/seal accumulator budget; 0 disables.",
        "Debug and regression-test lever only. It exists so the suite can reach the "
        "accumulator's chunk boundaries on a small corpus; maintenance_work_mem (or "
        "autovacuum_work_mem in an autovacuum worker) is the supported control, and a "
        "small value here makes seals and merges publish many small segments.",
        &bm25_debug_budget_kb,
        0, 0, MAX_KILOBYTES,
        PGC_SUSET, GUC_UNIT_KB,
        NULL, NULL, NULL);

    /* bm25_native.debug_count_slicing -- issue #289. The segment writer now ends
     * every posting block at a document boundary, so a document's postings for a
     * term never straddle two blocks. Segments written before that still hold
     * straddles, and the WAND reader has to stay correct on them forever, since
     * nothing on disk tells the two layouts apart. Without this lever the suite
     * could not build such a segment any more, so the reader's straddle handling
     * would go untested the moment the writer fix landed. On: the writer reverts to
     * the old rule, a cut every BM25_POSTINGS_PER_BLOCK postings. Both layouts are
     * valid format; nothing else changes.
     *
     * PGC_SUSET for debug_budget's reason: it changes the physical layout of
     * shared on-disk state that every other session then scans, and the layout it
     * produces is the slower one to scan. Debug-only, not a tuning knob. */
    DefineCustomBoolVariable("bm25_native.debug_count_slicing",
        "Makes the segment writer cut posting blocks by count, the layout of older segments.",
        "Regression-test lever only. It reproduces the older layout, in which one "
        "document's postings for a term can straddle two blocks, so the suite can "
        "test the reader on it.",
        &bm25_debug_count_slicing,
        false,
        PGC_SUSET, GUC_NOT_IN_SAMPLE,
        NULL, NULL, NULL);

    /* bm25_native.debug_pause -- deterministic interleaving for the concurrency
     * tests (t/020, t/022, t/025, t/027). A pause-point hook rather than timing: the races it
     * pins (#239, #241, #270, #291) need one backend stopped at a precise step while another
     * runs, which no sleep-and-hope schedule reproduces reliably, and standard server
     * builds lack injection points. PGC_SUSET because a parked VACUUM or merge holds
     * the seal/merge singleton, i.e. setting it is a way to stall every writer of
     * the index. */
    DefineCustomStringVariable("bm25_native.debug_pause",
        "Parks this backend at the named bm25 steps (comma-separated); empty disables.",
        "Regression-test lever only. At a named point the backend waits while "
        "pg_advisory_lock(1651323445, N) is held, N being the point's position: "
        "1 bulkdelete_start, 2 bulkdelete_segment, 3 bulkdelete_pending, "
        "4 merge_preswap, 5 swap_after_snapshot, 6 insert_keycheck, "
        "7 scan_pending_page, 8 orphan_sweep_start, 9 merge_start, "
        "10 pending_append_alloc, 11 orphan_sweep_marked, "
        "12 reclaim_retired_compacted, 13 reclaim_retired_between_chunks, "
        "14 merge_between_passes, 15 rank_build_attempt, 16 keymap_rotation, "
        "17 scan_post_page, 18 drain_pending_page, 19 debug_dict_entry.",
        &bm25_debug_pause,
        "",
        PGC_SUSET, GUC_NOT_IN_SAMPLE,
        bm25_debug_pause_check, NULL, NULL);

    /* bm25_native.debug_cancel_at -- the single-session counterpart of debug_pause
     * (issue #305): at a named point the backend cancels its own query, as if a
     * pg_cancel_backend had arrived at that instant. It exists for the suites that
     * must show WHERE a cancel takes effect -- that a long write loop is cancellable
     * between pages rather than only after it -- which a regression suite cannot
     * arrange by timing. PGC_SUSET with debug_pause: it shares that GUC's points and
     * check hook, and a lever that cancels maintenance midway belongs to the same
     * role that may park it. */
    DefineCustomStringVariable("bm25_native.debug_cancel_at",
        "Cancels this backend's query at the named bm25 steps (comma-separated); empty disables.",
        "Regression-test lever only. Accepts the point names bm25_native.debug_pause "
        "accepts. At a named point the backend marks its own query cancelled, and the "
        "cancel takes effect at the next point where interrupts are serviced.",
        &bm25_debug_cancel_at,
        "",
        PGC_SUSET, GUC_NOT_IN_SAMPLE,
        bm25_debug_pause_check, NULL, NULL);

    /* bm25_native.debug_cancel_after -- where in a loop debug_cancel_at's cancel lands.
     * A threshold on the backend's own work counter rather than a per-point hit count:
     * the suites that use it already reset and read that counter, so the arrival point
     * and the measurement share one scale, and "cancelled at N, serviced at N + k" is
     * read straight off it. PGC_SUSET with debug_cancel_at, which it only qualifies. */
    DefineCustomIntVariable("bm25_native.debug_cancel_after",
        "Holds bm25_native.debug_cancel_at back until this backend has done this many work units.",
        "Regression-test lever only. The units are bm25_debug_work_units(); 0 lets the "
        "cancel fire at the first named point reached.",
        &bm25_debug_cancel_after,
        0, 0, INT_MAX,
        PGC_SUSET, GUC_NOT_IN_SAMPLE,
        NULL, NULL, NULL);

    /* Reserve the namespace AFTER every bm25_native.* GUC above is registered:
     * without this, `SET bm25_native.wildcard_max_starss = 1` (a typo) is
     * silently accepted as a placeholder custom GUC instead of erroring, which
     * is especially bad for the PGC_SUSET pair above -- a typo'd parameter name
     * is USERSET by default, so a non-superuser could silently no-op the
     * guardrail they meant to (fail to) tighten. Caught in review (NIT). */
    MarkGUCPrefixReserved("bm25_native");
}

/* Forward decls for callbacks implemented in sibling files. */
extern IndexBuildResult *bm25_build(Relation heap, Relation index, struct IndexInfo *info);
extern void  bm25_buildempty(Relation index);
extern IndexBulkDeleteResult *bm25_bulkdelete(IndexVacuumInfo *info,
                         IndexBulkDeleteResult *stats,
                         IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *bm25_vacuumcleanup(IndexVacuumInfo *info,
                         IndexBulkDeleteResult *stats);
extern void  bm25_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                         Cost *startup, Cost *total, Selectivity *sel,
                         double *corr, double *pages);
/* Scan callbacks are implemented in bm25_scan.c; declared in bm25.h. */

/* Strategy numbers, as declared by the opclass DDL in bm25_native--1.0.sql:
 *   1  @@@   boolean match     (text,text) and (text,jsonb)
 *   2  &@@   float8 distance   ORDER BY, sorted by float_ops
 * amstrategies is 0 (operators come from the opclass, like GiST/GIN/pgvector),
 * so these numbers are a CONTRACT WITH THE CATALOG rather than a property the
 * AM declares -- which is exactly why amvalidate has to check them. */
#define BM25_STRAT_MATCH        1
#define BM25_STRAT_DISTANCE     2
#define BM25_NSTRATEGIES        2

/*
 * bm25_validate -- amvalidate: check that an opclass declared for this AM is
 * one this AM can actually execute.
 *
 * The ONLY caller is the amvalidate() SQL function (amapi.c), which opr_sanity
 * drives. CREATE OPERATOR CLASS does NOT invoke it -- which is precisely why
 * sql/86 can construct the malformed opclasses it uses as canaries, and why a
 * user can create one this AM cannot execute without hearing a word about it
 * until the planner or the scan disagrees.
 *
 * This ran as `return true` for the whole of M0-M6, with a comment deferring
 * the real check to a milestone that had already shipped. The amapi contract
 * permits that -- a stub returning true is legal -- but it means a second
 * opclass declared for bm25_native with a missing @@@ member, or with &@@
 * attached as a search operator instead of an ORDER BY one, is accepted at DDL
 * time and only surfaces later as a planner "operator is not a member of
 * opfamily" or as a scan that never receives the qual it expects.
 *
 * Deliberately stricter than blvalidate in one respect: amsupport is 0, so ANY
 * support procedure in the family is wrong, not merely one with a bad
 * signature. Reporting via ereport(INFO) + a false return, rather than ERROR,
 * is the amvalidate convention -- it lets opr_sanity report every problem in
 * one pass instead of stopping at the first.
 */
static bool
bm25_validate(Oid opclassoid)
{
    bool                result = true;
    HeapTuple           classtup;
    HeapTuple           familytup;
    Form_pg_opclass     classform;
    Form_pg_opfamily    familyform;
    Oid                 opfamilyoid;
    Oid                 opcintype;
    char               *opclassname;
    char               *opfamilyname;
    CatCList           *proclist;
    CatCList           *oprlist;
    bool                seen_match = false;
    bool                seen_distance = false;
    int                 i;

    classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
    if (!HeapTupleIsValid(classtup))
        elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
    classform = (Form_pg_opclass) GETSTRUCT(classtup);

    opfamilyoid = classform->opcfamily;
    opcintype   = classform->opcintype;
    opclassname = NameStr(classform->opcname);

    /* HDL-02: opcintype was read and then used ONLY to test member coverage --
     * whether the opclass declares an operator for its own input type -- never to
     * test what that type IS. Detecting exactly this shape is amvalidate's stated
     * purpose, and the canary suite builds six malformed opclasses all FOR TYPE
     * text, so the wrong-input-type case went unexercised and unreported.
     *
     * This is the reporting half only. amvalidate is never invoked by DDL -- only
     * the amvalidate() SQL function calls it, which opr_sanity drives -- so it
     * cannot stop a malformed opclass reaching CREATE INDEX. The check that
     * actually prevents the crash is the per-column type test in
     * bm25_check_indexed_column_types (bm25_build.c), which runs on the build and
     * insert paths where the Datum is about to be dereferenced as a varlena. */
    if (opcintype != TEXTOID && opcintype != VARCHAROID &&
        getBaseType(opcintype) != TEXTOID && getBaseType(opcintype) != VARCHAROID)
    {
        ereport(INFO,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("operator class \"%s\" of access method bm25_native is "
                        "declared for type %s, but the access method can only "
                        "index text", opclassname, format_type_be(opcintype))));
        result = false;
    }

    /* Deliberately NOT get_opfamily_name(): that helper is PG 18+, and this
     * extension's floor is 17. The syscache lookup below is what PG 17's own
     * blvalidate does and compiles on both. */
    familytup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamilyoid));
    if (!HeapTupleIsValid(familytup))
        elog(ERROR, "cache lookup failed for operator family %u", opfamilyoid);
    familyform = (Form_pg_opfamily) GETSTRUCT(familytup);
    opfamilyname = NameStr(familyform->opfname);

    oprlist  = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
    proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

    /* amsupport == 0: the AM calls no support procedure, so any registered one
     * would silently never be invoked. */
    for (i = 0; i < proclist->n_members; i++)
    {
        HeapTuple       proctup  = &proclist->members[i]->tuple;
        Form_pg_amproc  procform = (Form_pg_amproc) GETSTRUCT(proctup);

        ereport(INFO,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("bm25_native opfamily %s contains support procedure %s, but the access method uses none",
                        opfamilyname, format_procedure(procform->amproc))));
        result = false;
    }

    for (i = 0; i < oprlist->n_members; i++)
    {
        HeapTuple       oprtup  = &oprlist->members[i]->tuple;
        Form_pg_amop    oprform = (Form_pg_amop) GETSTRUCT(oprtup);

        if (oprform->amopstrategy < 1 ||
            oprform->amopstrategy > BM25_NSTRATEGIES)
        {
            ereport(INFO,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("bm25_native opfamily %s contains operator %s with invalid strategy number %d",
                            opfamilyname, format_operator(oprform->amopopr),
                            oprform->amopstrategy)));
            result = false;
            continue;
        }

        /* SURFACE-09, the reporting half: a right-hand type the scan cannot
         * decode. The scan itself refuses such a key (bm25_rescan); this only
         * reports, since DDL never calls amvalidate. Not counted toward
         * coverage below: such a member cannot answer a query. */
        if (!bm25_query_rhs_type_ok(oprform->amoprighttype))
        {
            ereport(INFO,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("bm25_native opfamily %s contains operator %s with right-hand type %s, "
                            "but the access method accepts only text, varchar or jsonb queries",
                            opfamilyname, format_operator(oprform->amopopr),
                            format_type_be(oprform->amoprighttype))));
            result = false;
            continue;
        }

        /* The two strategies differ in PURPOSE, not just in signature: getting
         * this backwards is the failure that DDL currently accepts silently. */
        if (oprform->amopstrategy == BM25_STRAT_MATCH)
        {
            if (oprform->amoppurpose != AMOP_SEARCH ||
                OidIsValid(oprform->amopsortfamily))
            {
                ereport(INFO,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("bm25_native opfamily %s contains operator %s registered for ORDER BY, "
                                "but strategy %d is a search operator",
                                opfamilyname, format_operator(oprform->amopopr),
                                BM25_STRAT_MATCH)));
                result = false;
            }
            else if (!check_amop_signature(oprform->amopopr, BOOLOID,
                                           oprform->amoplefttype,
                                           oprform->amoprighttype))
            {
                ereport(INFO,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("bm25_native opfamily %s contains operator %s with wrong signature "
                                "for strategy %d (expected boolean result)",
                                opfamilyname, format_operator(oprform->amopopr),
                                BM25_STRAT_MATCH)));
                result = false;
            }
            else if (oprform->amoplefttype == opcintype)
                seen_match = true;
        }
        else                    /* BM25_STRAT_DISTANCE */
        {
            if (oprform->amoppurpose != AMOP_ORDER ||
                !OidIsValid(oprform->amopsortfamily))
            {
                ereport(INFO,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("bm25_native opfamily %s contains operator %s registered as a search operator, "
                                "but strategy %d is an ORDER BY operator",
                                opfamilyname, format_operator(oprform->amopopr),
                                BM25_STRAT_DISTANCE)));
                result = false;
            }
            else if (!check_amop_signature(oprform->amopopr, FLOAT8OID,
                                           oprform->amoplefttype,
                                           oprform->amoprighttype))
            {
                /* The scan returns the score as a float8 distance; a different
                 * result type would be coerced or rejected in the planner
                 * rather than here, far from the DDL that caused it. */
                ereport(INFO,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("bm25_native opfamily %s contains operator %s with wrong signature "
                                "for strategy %d (expected double precision result)",
                                opfamilyname, format_operator(oprform->amopopr),
                                BM25_STRAT_DISTANCE)));
                result = false;
            }
            else if (!opfamily_can_sort_type(oprform->amopsortfamily, FLOAT8OID))
            {
                ereport(INFO,
                        (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                         errmsg("bm25_native opfamily %s contains operator %s with an ORDER BY sort family "
                                "that cannot sort double precision",
                                opfamilyname, format_operator(oprform->amopopr))));
                result = false;
            }
            else if (oprform->amoplefttype == opcintype)
                seen_distance = true;
        }
    }

    /* Completeness, checked against the opclass's OWN input type: a family may
     * legitimately carry cross-type members, but the named opclass must be able
     * to answer both a match and an ordering query over its own type or it
     * cannot drive a scan at all. */
    if (!seen_match)
    {
        ereport(INFO,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("bm25_native opclass %s is missing operator %d for its own input type",
                        opclassname, BM25_STRAT_MATCH)));
        result = false;
    }
    if (!seen_distance)
    {
        ereport(INFO,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("bm25_native opclass %s is missing ORDER BY operator %d for its own input type",
                        opclassname, BM25_STRAT_DISTANCE)));
        result = false;
    }

    ReleaseCatCacheList(proclist);
    ReleaseCatCacheList(oprlist);
    ReleaseSysCache(familytup);
    ReleaseSysCache(classtup);

    return result;
}

Datum
bm25_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amr = makeNode(IndexAmRoutine);

    /* Operators are defined by the opclass (search=1, orderby=2), like
     * GiST/GIN/pgvector; strategy numbers live in the opclass DDL, not here. */
    amr->amstrategies = 0;
    amr->amsupport = 0;             /* permanent: this AM calls no support procedure at
                                     * all, so any registered one IS merely unused --
                                     * silently. bm25_validate reports it, but only at
                                     * INFO with result = false, and nothing in DDL calls
                                     * amvalidate (see the HDL-02 note on bm25_validate:
                                     * the amvalidate() SQL function that opr_sanity
                                     * drives is its only caller). */
    amr->amoptsprocnum = 0;
    amr->amcanorder = false;
    amr->amcanorderbyop = true;     /* &@@ ORDER BY (strategy 2); see bm25_validate */
    amr->amcanbackward = false;
    amr->amcanunique = false;
    amr->amcanmulticol = true;      /* M5/C2: each indexed column is a dense field.
                                     * Flipped here (per section 0 R2) because the flip is ONLY
                                     * safe once every ingest path preserves every field's
                                     * postings -- verified true for build, INSERT, AND merge:
                                     *  - build/INSERT tokenize EACH column into its field
                                     *    (bm25_build_callback / bm25_insert), the accumulator
                                     *    + per-block RLE + per-field NORMS/sumlen carry the
                                     *    (term,field_id) dimension;
                                     *  - the MERGE replay is field-aware: bm25_merge_execute
                                     *    uses bm25_accum_begin_multi(field_count), reads each
                                     *    doc's per-field doclens from the source's per-field
                                     *    NORMS (bm25_seg_doclen_field), and merge_post_cb
                                     *    threads the decoded field_id into add_posting, so the
                                     *    merged segment carries field 0..N-1 exactly.
                                     * Proven by the merge-preservation stanza in
                                     * 30_field_postings (a 3-column index survives a real
                                     * 4-segment merge with per-field postings intact).
                                     * NOTE: the BM25F SCORER is per-field too (C3 shipped):
                                     * it scores each query term against every field's own
                                     * doclen/idf/k1/b/boost (bm25_scan_rank.c's
                                     * fcfg-threaded term-scoring context, bm25_wand.c's
                                     * per-field impacts, bm25_debug.c's BM25_FIELD_ALL SRFs),
                                     * not a single pooled posting list. */
    amr->amoptionalkey = false;
    amr->amsearcharray = false;
    amr->amsearchnulls = false;
    amr->amstorage = false;
    amr->amclusterable = false;
    amr->ampredlocks = false;
    amr->amcanparallel = false;
    amr->amcaninclude = true;       /* M5/C5: key_field is carried as an INCLUDE column
                                     * (e.g. USING bm25_native (title, body) INCLUDE (id)) so its
                                     * value reaches the build/insert callback in values[]
                                     * without the key type needing its own bm25 opclass.
                                     * INCLUDE columns are NOT tokenized (the field loop
                                     * covers only the key attributes); bm25_resolve_fields
                                     * matches key_field against ALL index attributes (key +
                                     * included) and records the key's dense index attno so
                                     * the callback reads values[key_attno]. */
    /* BUILD-04: this AM DOES consume maintenance_work_mem. amvacuumcleanup seals
     * the pending list and then merges, and both feed the same accumulator, which
     * is bounded by bm25_maintenance_budget_bytes -- autovacuum_work_mem in an
     * autovacuum worker when set, else maintenance_work_mem. Declaring false was
     * accurate only while nothing consulted the budget at all.
     *
     * INERT TODAY, and stated rather than implied: vacuumparallel.c consults this
     * flag only for indexes that participate in parallel vacuum, and
     * amparallelvacuumoptions below is VACUUM_OPTION_NO_PARALLEL, so nothing reads
     * it. It is set because it is the true answer to what the field asks -- the same
     * declaration-accuracy standard the rest of this block was brought to -- and so
     * that the day this AM gains a parallel vacuum option, the budget is not
     * silently multiplied across workers. */
    amr->amusemaintenanceworkmem = true;
    amr->amsummarizing = false;
    amr->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
    amr->amkeytype = InvalidOid;

    amr->ambuild = bm25_build;
    amr->ambuildempty = bm25_buildempty;
    amr->aminsert = bm25_insert;
    amr->ambulkdelete = bm25_bulkdelete;
    amr->amvacuumcleanup = bm25_vacuumcleanup;
    amr->amcanreturn = NULL;
    amr->amcostestimate = bm25_costestimate;
    amr->amoptions = bm25_options;
    amr->amproperty = NULL;
    amr->ambuildphasename = NULL;
    amr->amvalidate = bm25_validate;
    amr->amadjustmembers = NULL;
    amr->ambeginscan = bm25_beginscan;
    amr->amrescan = bm25_rescan;
    amr->amgettuple = bm25_gettuple;
    amr->amgetbitmap = NULL;
    amr->amendscan = bm25_endscan;
    amr->ammarkpos = NULL;
    amr->amrestrpos = NULL;
    amr->amestimateparallelscan = NULL;
    amr->aminitparallelscan = NULL;
    amr->amparallelrescan = NULL;

    PG_RETURN_POINTER(amr);
}

/*
 * bm25_bulkdelete -- VACUUM's dead-TID callback (Task 17).
 *
 * Standard ambulkdelete shape: core hands us a callback that TESTS one index TID
 * at a time (not a list of dead TIDs), so we iterate every index entry and test
 * it. For each live segment we read its header and walk local doc-ids 0..ndocs-1,
 * mapping each to its heap TID via the docid->TID DOCMAP; a live doc whose TID the
 * callback marks dead is tombstoned with bm25_livedocs_clear. Then we sweep the
 * pending list for dead entries. stats->tuples_removed accumulates the count.
 *
 * We iterate the DOCMAP directly rather than building a sorted (TID,docid) view:
 * because core gives us a per-TID test callback (never a dead-TID set to search),
 * a sorted view would only be iterated front to back -- costing the same per-doc
 * bm25_seg_docid_to_tid walk as direct iteration while adding a qsort and an
 * unused binary-search path. Cost is therefore O(ndocs * docmap-chain) per segment,
 * acceptable for VACUUM (background, bounded frequency). A persistent per-segment
 * reverse TID->docid index would make a cold lookup O(log ndocs) but is a deferred
 * optimization, out of scope for Phase 3.
 *
 * NOTE: amvacuumcleanup (below) runs after this within the same VACUUM and does
 * the real work -- seal the pending list and reclaim orphan pages (Task 18).
 */
IndexBulkDeleteResult *
bm25_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                IndexBulkDeleteCallback callback, void *callback_state)
{
    Relation            index = info->index;
    BM25SegCatEntry    *segs;
    uint32              nsegs,
                        s;
    uint64              tombstoned = 0;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* The seal/merge singleton is held in ShareLock mode for the WHOLE pass --
     * catalog snapshot, every segment's tombstones, and the pending sweep -- and
     * that hold is what makes the pass correct (#239, #241). Every operation that
     * changes which documents live where takes it ExclusiveLock: the seal (pending
     * docs move into a new segment), the merge and bm25_upgrade's rewrite (inputs'
     * live docs are replayed into new segments from ONE read of their LIVEDOCS,
     * then the inputs are retired and every survivor's catalog entry is republished
     * from a copy taken before the flip), and both reclaims (retired and orphan
     * pages are freed for reuse). Any of them running inside this pass loses
     * tombstones:
     *
     *   - a seal drains a dead pending doc into a segment this snapshot never saw,
     *     before the pending sweep reaches it, so nothing tombstones it;
     *   - a merge that replayed an input before we cleared a bit republishes that
     *     doc live in its output. VACUUM then frees the heap line pointer, and once
     *     it is reused the index returns the new, unrelated row with
     *     xs_recheck = false. A merge that swaps mid-loop instead leaves us
     *     tombstoning a retired segment: "segment header N not found in catalog";
     *   - the swap's pre-flip catalog copy overwrites a survivor's live_ndocs /
     *     total_len and the metapage ndocs / total_len decrements we make between
     *     its copy and its flip, so corpus statistics drift high.
     *
     * It also means no segment in the snapshot can be retired, let alone have its
     * pages reclaimed and reused, while we walk it. That is why bm25_livedocs_clear
     * may re-read the header with gen validation off, and why the snapshot's own
     * two-step read (meta, then chain) is atomic against a root swap.
     *
     * ShareLock, not ExclusiveLock: it does not conflict with pending appenders
     * (ShareLock, ADR 0022), so inserts keep flowing, and bm25_pending_mark_dead's
     * own ShareLock below is a re-acquisition by this backend, granted from the
     * local lock table without queueing. The opportunistic writers -- aminsert's
     * seal and amvacuumcleanup's merge_maybe(false) -- use ConditionalLockPage and
     * skip rather than wait, and amvacuumcleanup's seal and merge run only after
     * this function has returned and released the lock.
     *
     * The cost is ADR 0066's trade, stretched from the pending sweep to the whole
     * pass: a blocking bm25_seal() / bm25_merge() / bm25_upgrade() waits for the
     * pass to finish, and while one waits the lock manager queues new ShareLock
     * requests behind it, so inserters stall with it. Under autovacuum that is
     * bounded by deadlock_timeout, after which the waiter's deadlock check cancels
     * the autovacuum (never an anti-wraparound one); a manual VACUUM is not
     * cancelled. Both loops stay cancellable (BM25_VACUUM_DELAY_POINT).
     *
     * Released at the normal exit below. An ERROR releases it at transaction abort:
     * it is a transaction-level lmgr lock, VACUUM processes each table in its own
     * transaction, and no caller catches an ambulkdelete error and carries on, so a
     * PG_TRY here would only duplicate what abort already does. */
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
    bm25_segcat_read_locked(index, &segs, &nsegs);
    bm25_debug_pause_point("bulkdelete_start");

    for (s = 0; s < nsegs; s++)
    {
        BM25SegmentHeader   h;
        /* Forward cursors over THIS segment's LIVEDOCS/DOCMAP chains for the doc loop
         * below, the same shape bm25_scan_rank.c's TermScoreCtx uses on the query path. The
         * loop is the textbook case for it: it visits every local docid in ASCENDING
         * order, so each lookup resumes on the page the previous one landed on instead
         * of re-walking from the chain root -- the "O(ndocs * docmap-chain)" sweep the
         * delay-point comment below names becomes O(ndocs + chain). It also removes the
         * two SEGREAD-11 RelationGetNumberOfBlocks calls the one-shot accessors took per
         * document, which lseek on every call outside recovery.
         *
         * DECLARED INSIDE THE SEGMENT LOOP, with h, so a reader can never be carried to
         * the next segment: it is opened on h, and h is refilled per iteration.
         *
         * Safe against this loop's own writes. bm25_livedocs_clear mutates a bit on a
         * LIVE page already in the chain; it neither relinks the chain nor extends the
         * relation, and the cursor caches a page NUMBER and a byte origin, never page
         * CONTENT -- so the next read re-fetches the page and sees the cleared bit. The
         * loop is also strictly forward, so it never revisits a docid it tombstoned. */
        BM25SegReader       rdr;
        uint32              d;

        CHECK_FOR_INTERRUPTS();
        if (s == 1)
            bm25_debug_pause_point("bulkdelete_segment");

        bm25_seg_header_read(index, segs[s].header_blkno, segs[s].gen, &h);
        bm25_seg_reader_init(&rdr, index, &h);

        /* Note: bm25_livedocs_clear below re-reads this header with expected_gen=0
         * (gen-validation off) -- intentional, not an inconsistency: the option-(d)
         * check guards a read that can race a retire-and-reclaim, and this pass
         * cannot, because the ShareLock held above excludes every merge and reclaim
         * until it ends. We validate here only because we have the gen in hand. */

        /* Test each local doc's heap TID against the callback; only LIVE docs can
         * still be tombstoned. */
        /* The cast is exact: bm25_segheader_validate bounds ndocs to PG_UINT32_MAX. */
        for (d = 0; d < (uint32) h.ndocs; d++)
        {
            ItemPointerData tid;

            /* Not just an interrupt point: vacuum_delay_point is also where
             * cost-based throttling engages, so without it autovacuum's delay
             * budget was silently bypassed for this whole O(ndocs * docmap-chain)
             * sweep. It calls CHECK_FOR_INTERRUPTS internally, matching
             * btvacuumpage / ginVacuumPostingTreeLeaves. */
            BM25_VACUUM_DELAY_POINT();

            if (!bm25_seg_reader_doc_is_live(&rdr, d))
                continue;       /* already tombstoned */
            tid = bm25_seg_reader_docid_to_tid(&rdr, d);
            if (callback(&tid, callback_state))
            {
                bm25_livedocs_clear(index, segs[s].header_blkno, d);
                tombstoned++;
            }
        }
    }

    /* The catalog copy lives in the vacuum's context, which outlives this call; nsegs
     * * sizeof(BM25SegCatEntry) per pass is not worth keeping (issue #313 XCUT-15).
     * NULL when the catalog is empty (bm25_segcat_read_locked allocates nothing). */
    if (segs != NULL)
        pfree(segs);

    bm25_debug_pause_point("bulkdelete_pending");
    tombstoned += bm25_pending_mark_dead(index, callback, callback_state);
    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);

    stats->tuples_removed += (double) tombstoned;
    return stats;
}

/*
 * bm25_vacuumcleanup -- runs after bm25_bulkdelete (or standalone for an
 * analyze-free VACUUM with no dead tuples). Order matters and is the transitive
 * tail of the two-phase commit: SEAL first (publishes any pending docs as a
 * segment and advances the pending anchor in ONE record), and reclaim orphans only
 * after it. Sealing before reclaiming is what makes the reclaim safe -- the
 * just-truncated pending pages are now unreachable (pending_head was reset by the
 * publish record), so reclaim re-frees them; live segment pages are reachable and
 * never freed.
 *
 * analyze_only VACUUMs do no cleanup. There is no should-seal guard here because
 * bm25_seal_index handles an empty CHAIN itself: pending_head Invalid means
 * drained_head is Invalid too, so bm25_segcat_publish_append writes no record and
 * bm25_pending_truncate never runs. That is narrower than "the drain produced no
 * docs" -- a chain whose every doc VACUUM already tombstoned also drains to zero
 * entries, and THAT case still emits the anchor-detach publish record and still
 * recycles the drained pages. Both are safe to call unconditionally; only the
 * first is free.
 *
 * Order: seal -> reclaim_retired -> merge_maybe -> orphan sweep (issue #300).
 * The orphan sweep is LAST because it is the one pass whose cost scales with the
 * whole index (mark every live chain, then visit every block, throttled), so it is
 * the pass most likely to be cancelled. It runs only on evidence that an orphan can
 * exist (bm25_reclaim_orphans, "GATED, AND SHARE-MODE"), and running it after the
 * merge is what lets the same cleanup free the catalog chain a merge's swap orphans:
 * the merge leaves its orphan bracket open on purpose, and the sweep that follows
 * retires it. It used to run second, and a
 * cancelled sweep then also skipped the merge and the retired reclaim: merge_maybe
 * here is the ONLY automatic merge and this is the only automatic retired reclaim,
 * so an insert-busy table whose autovacuum kept being cancelled in the sweep grew
 * segments and retired-but-never-freed ranges without bound. Now a cancelled sweep
 * loses only itself; whatever the passes before it did is already on disk (index
 * page writes are not undone by the abort).
 *
 * Why the reorder is safe. Each pass takes and releases the seal/merge singleton on
 * its own, so the sweep still runs with nothing building or swapping (ADR 0019),
 * and every interleaving this order produces was already reachable across two
 * VACUUMs, or a bm25_merge() followed by a VACUUM. Pages reclaim_retired frees are
 * stamped BM25_PAGE_DELETED before they reach the FSM, so a later sweep finds them
 * unreachable and only re-records them free (it never re-stamps a DELETED page);
 * one an appender re-allocated in between is either linked into the pending chain
 * before the sweep marks, or -- the sweep holds the singleton only in ShareLock,
 * which appenders share -- a PENDING page carrying the chain's epoch, which the
 * sweep's per-page rule never stamps. A merge's freshly retired segments are on the retired list when the
 * sweep marks, so their RANGE marking keeps them out of the free set. The cost runs
 * the other way: an ERROR in reclaim_retired or the merge now also skips this
 * VACUUM's sweep, where it used to skip only what followed the sweep. The sweep is
 * idempotent and its orphans wait for the next VACUUM, which is the better side of
 * that trade: the two earlier passes are bounded by the retired list and the merge
 * inputs, the sweep by the whole index.
 *
 * reclaim_retired runs before the merge so that a merge that is itself cancelled --
 * its one opportunistic pass also holds the singleton for O(inputs) -- cannot starve
 * horizon reclamation either, and so the merge can reuse what it frees.
 * bm25_merge_maybe still calls bm25_reclaim_retired after its own pass (that is the
 * bm25_merge() path's reclaim); here that second call finds only entries whose
 * horizon cleared during the merge, since the merge's own retirements carry a
 * future retire_xid. info->heaprel is the open heap relation supplying the
 * cluster-wide visibility horizon.
 *
 * Only merge_maybe(false) is conditional. The seal and both reclaims take the
 * seal/merge singleton with a BLOCKING LockPage(ExclusiveLock), so a cleanup that
 * arrives while an explicit bm25_seal() / bm25_merge() / bm25_upgrade() holds the
 * singleton waits for that hold to end -- the reverse of bm25_bulkdelete's stall,
 * where the explicit call is the one waiting. Inserts are already blocked by the
 * holder (ADR 0022); once it releases, a queued cleanup's own hold is the same seal
 * and reclaims every VACUUM makes.
 *
 * Autovacuum's lock-conflict cancel cuts one way only. While an autovacuum cleanup
 * WAITS it is the waiter, so nothing cancels it. Once it HOLDS the ExclusiveLock for
 * its seal, a reclaim or its merge, an insert that queues on it is hard-blocked by an
 * autovacuum holder, and after deadlock_timeout the insert's deadlock check cancels a
 * non-wraparound autovacuum (the trade ADR 0066 records). The orphan sweep is no
 * longer such a hold: it takes the singleton in ShareLock, which inserts share, and
 * runs only on evidence. The ExclusiveLock holds that remain scale with the pending
 * list (the seal), the horizon-cleared retired list (reclaim_retired), and the
 * merge's inputs. One share-mode residual: a BLOCKING bm25_seal() / bm25_merge() /
 * bm25_upgrade() arriving during the sweep queues for ExclusiveLock, inserts queue
 * behind that waiter, and the waiter cancels the autovacuum (bm25_reclaim_orphans's
 * header). */
IndexBulkDeleteResult *
bm25_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (info->analyze_only)
        return stats;

    bm25_seal_index(info->index);
    bm25_reclaim_retired(info->index, info->heaprel);
    bm25_merge_maybe(info->index, false);          /* opportunistic merge at vacuum cadence */
    bm25_reclaim_orphans(info->index);             /* last: see "Order" above; gated */

    /* The whole-map FSM vacuum, on EVERY cleanup, swept or not (issue #300). Pages
     * become free through RecordFreeIndexPage, which writes only an FSM leaf; the
     * upper levels GetFreeIndexPage searches from learn of them only from a vacuum,
     * and fsm_search corrects a stale parent only downward. The truncate vacuums
     * just the range it freed (bm25_pending_truncate, PEND-07), and
     * bm25_page_alloc's requeue of a rejected candidate vacuums nothing, so this is
     * where those leaves become findable. It used to happen as a side effect of the
     * orphan sweep, which ran every time; the sweep is gated now and usually does
     * not run. Outside every heavyweight lock: the FSM locks its own pages. About
     * nblocks / 4000 FSM pages. */
    IndexFreeSpaceMapVacuum(info->index);

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    stats->num_pages = RelationGetNumberOfBlocks(info->index);

    /* num_index_tuples is never computed on this path, so it stays at the
     * palloc0 zero above. Left alone, that zero reaches core as a REAL count:
     * update_relstats_all_indexes (vacuumlazy.c) writes istat->num_index_tuples
     * into pg_class.reltuples for this index whenever istat->estimated_count is
     * false -- which is exactly the palloc0 default -- so every VACUUM would
     * permanently stomp the index's reltuples to 0. estimated_count = true
     * tells core not to trust that zero, so it leaves pg_class.reltuples as it
     * was (set correctly at CREATE INDEX / ANALYZE time) instead of overwriting
     * it.
     *
     * The honest alternative -- summing live document counts from the segment
     * catalog (segs[s].live_ndocs) plus the metapage's pending_ndocs -- was
     * rejected: every maintenance call above (bm25_seal_index,
     * bm25_reclaim_orphans, bm25_merge_maybe, bm25_reclaim_retired) returns
     * void, so none of that data is already in hand here. Producing a real
     * count would mean this function running its OWN fresh bm25_meta_read +
     * bm25_segcat_read pass -- genuinely new I/O, paid on every VACUUM, for a
     * statistic nothing in this AM actually consumes. */
    /* Note this also suppresses the relpages update: core skips vac_update_relstats
     * entirely when estimated_count is set, so num_pages above now only feeds
     * VACUUM VERBOSE. That is the accepted trade -- ANALYZE refreshes both fields,
     * and a stale relpages is a far smaller lie than a reltuples pinned at 0. */
    stats->estimated_count = true;
    return stats;
}

/* Parameter names match the declaration above and PostgreSQL's amcostestimate
 * contract: loop_count is the number of times the inner scan is re-executed,
 * not a row count. It is unused here -- nothing this function produces varies
 * with re-execution. (That is NOT the same as "the estimate is a constant":
 * *total below scales with path->indexinfo->tuples. See the placeholder note
 * inside.) */
void
bm25_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                  Cost *startup, Cost *total, Selectivity *sel, double *corr,
                  double *pages)
{
    /* M1: cheap, finite, ordering-aware estimate so the ordered index path is
     * chosen for ORDER BY ... LIMIT without being literally zero-cost (a zero
     * total can confuse the planner and makes the no-Sort EXPLAIN test brittle).
     * Still exactly that placeholder today: M6 shipped (REGRESS carries
     * 46_m6_boolean..49_m6_acceptance) without touching this function.
     *
     * A PLACEHOLDER DEFERRED BY DECISION, NOT A FINISHED DESIGN. ADR 0010's two
     * honest numbers are bm25_matchsel's BM25_MATCH_SEL and bm25_match's
     * COST 5000 -- neither of them lives here. That record lists "Reworking
     * bm25_costestimate" under Alternatives considered as "out of scope by user
     * decision", and calls *sel and *startup below known stubs awaiting a future
     * M6 cost pass. (ADR 0010 writes the two Cost outputs as `*su` and `*tot`;
     * the parameters are named *startup and *total here, and are referred to by
     * those names below.) *sel = 0.05 equalling BM25_MATCH_SEL is a coherence
     * bonus, not a shared constant: a change to one does not propagate to the
     * other, and a reader touching either should check both.
     *
     * WHAT THE FUTURE COST PASS MUST CARRY FORWARD (ADR 0010, "The startup-cost
     * coupling (spec section 7), verified"): *startup = 1.0 understates a scan
     * that builds its WAND-capped ranking before emitting a tuple, and it stopped
     * being dormant once honest selectivity turned Limit proration on. Measured
     * at shipped values it is not decisive on its own -- a deliberately extreme
     * probe setting startup to 0.9 * total (the record's "*su = *tot x 0.9")
     * kept the Index Scan in all three GUC cells. The hazard is the COMBINATION
     * of raising *startup toward honesty AND a much higher effective selectivity
     * shrinking the procost-driven gap between the index and seqscan paths;
     * re-check the margin if a corpus or query shape ever drives @@@ selectivity
     * dramatically higher at the same time.
     *
     * Of the five values assigned below, four are literal constants; *total is
     * not -- it scales with path->indexinfo->tuples. */
    *startup = 1.0;
    *total = 10.0 + 0.01 * path->indexinfo->tuples;
    *sel = 0.05;
    *corr = 0.0;
    *pages = 1.0;
}
/* Reject any tokenizer but "standard" -- the only M3 tokenizer (BM25_TOKENIZER_*).
 *
 * NOT static, and that is the point (#148 HDL-07). Two surfaces take a tokenizer
 * name from the user -- this reloption, and the explicit-config debug overloads
 * bm25_debug_tokenize(text,text,text,text) / bm25_debug_analyze_positions -- and they
 * must share the PREDICATE, not restate it. Restating is how the two drift into
 * accepting different sets, which is a defect this project has already paid for once
 * (#157): the identical knob answering differently depending on which door you came
 * in is worse than either answer. src/bm25_tokenize.c calls this one. */
void
bm25_validate_tokenizer(const char *value)
{
    if (value != NULL && strcmp(value, "standard") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: invalid tokenizer \"%s\"; only \"standard\" is supported",
                        value)));
}

/* Reject any analyzer but "english" (#148 HDL-07).
 *
 * `analyzer` is RESERVED SURFACE, not a selector: there is exactly one analyzer
 * pipeline (standard tokenizer -> Snowball stemmer), and analyzer_offset is accepted
 * but never consumed anywhere in the tree -- see the long note in
 * bm25_analyzer_config_from_opts (bm25_analyzer.c). Until this validator existed,
 * `WITH (analyzer = 'german')` was accepted, produced english stemming, did not even
 * perturb the analyzer fingerprint (so require_analyzer_match never fired), and told
 * the user nothing. Rejecting is the honest reading of a reloption that selects
 * nothing: a value that changes no behavior should not be quietly accepted as if it
 * did.
 *
 * "english" is the registered default, so the only accepted value is the one that was
 * already in effect. It is a language-shaped name for historical reasons; do not read
 * it as evidence that some other language is selectable here -- `language` is the knob
 * that does that. When a real named-analyzer registry is built, this is the function
 * that grows a list. */
static void
bm25_validate_analyzer(const char *value)
{
    if (value != NULL && strcmp(value, "english") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: invalid analyzer \"%s\"; only \"english\" is supported",
                        value),
                 errdetail("The analyzer reloption is reserved: "
                           "this build has one analyzer pipeline and selects nothing from it."),
                 errhint("Use the language reloption to choose a Snowball stemmer. "
                         "An index CREATED before this value was validated still carries it, "
                         "and every ALTER INDEX ... SET on that index now fails: "
                         "clear it with ALTER INDEX ... RESET (analyzer), "
                         "which changes no behavior because the value was never consumed. "
                         "If this statement came from pg_dump or pg_upgrade there is no index here yet to RESET -- "
                         "run that RESET in the SOURCE database and dump again, "
                         "or delete the analyzer= clause from the dump.")));
}

/* Reject any stopwords value but "default" | "none" (BM25_STOPWORDS_*). */
static void
bm25_validate_stopwords(const char *value)
{
    if (value != NULL &&
        strcmp(value, "default") != 0 && strcmp(value, "none") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: invalid stopwords \"%s\"; use \"default\" or \"none\"",
                        value)));
}

/* M4: reject any phrase_fallback value but "error" | "and". Controls the D7
 * degradation of a phrase query against a position-less segment / off field:
 * "error" (default) raises; "and" downgrades to a WARNING + plain AND-of-terms. */
static void
bm25_validate_phrase_fallback(const char *value)
{
    if (value != NULL &&
        strcmp(value, "error") != 0 && strcmp(value, "and") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: invalid phrase_fallback \"%s\"; use \"error\" or \"and\"",
                        value)));
}

/* Is `name` a dynamically-named per-field knob (k1_<attname> / b_<attname> /
 * boost_<attname>)? These cannot be pre-registered on the process-global relopt
 * kind (the attnames are only known at CREATE INDEX), and build_reloptions with
 * validate=true would reject them as "unrecognized parameter". bm25_options strips
 * them before validation; bm25_resolve_fields reads their values from the raw
 * catalog reloption array (which core stores in full regardless). The suffix must
 * be non-empty so the fixed keys are never matched (none of them fit the shape:
 * "key_field"/"require_analyzer_match"/etc. do not start with "k1_"/"b_"/"boost_").
 * The shape itself is bm25_field_knob_suffix's (bm25_build.c), shared with the
 * build-time unmatched-suffix WARNING so the two can never disagree. */
static bool
bm25_is_per_field_knob(const char *name)
{
    return bm25_field_knob_suffix(name) != NULL;
}

/* Drop the per-field knobs from a reloptions Datum so build_reloptions validates
 * only the registered keys. Returns the original Datum unchanged when there are no
 * knobs to strip (the common single-field / no-override case) so we pay nothing. */
static Datum
bm25_strip_field_knobs(Datum reloptions)
{
    List     *opts;
    List     *kept = NIL;
    ListCell *lc;
    bool      stripped = false;

    /* PointerIsValid was removed from c.h in PG19; DatumGetPointer(x) != NULL
     * is the portable equivalent and reads the same on 17/18. */
    if (DatumGetPointer(reloptions) == NULL)
        return reloptions;

    opts = untransformRelOptions(reloptions);
    foreach(lc, opts)
    {
        DefElem *de = (DefElem *) lfirst(lc);
        if (bm25_is_per_field_knob(de->defname))
            stripped = true;
        else
            kept = lappend(kept, de);
    }
    if (!stripped)
        return reloptions;      /* nothing dynamic; keep the caller's Datum */

    /* Rebuild a fresh Datum from the retained keys (namespace = the reloptions
     * default; no OIDs, not a RESET). NIL kept -> a NULL Datum, which
     * build_reloptions treats as "no options". */
    return transformRelOptions(PointerGetDatum(NULL), kept, NULL, NULL,
                               false, false);
}

/* bm25_options -- reloptions parser/validator (amoptions). Registers the M3
 * analyzer keywords + the M5 key_field keyword on a private relopt_kind on first
 * use; fills a BM25Options bytea. Validation (validate = true at CREATE INDEX /
 * ALTER) rejects unknown tokenizer/stopwords values. key_field's column-existence
 * check and the per-field k1_/b_/boost_ knobs (dynamically named by attname)
 * are resolved in bm25_build/bm25_resolve_fields, which have the INDEX descriptor
 * and column list in scope (key_field is matched against RelationGetDescr(index),
 * key + INCLUDE attributes); BM25_MAX_FIELDS is enforced there too. Their VALUES,
 * though, are also range-validated right here at CREATE/ALTER time (validate =
 * true), before the strip below hides them from build_reloptions -- see the
 * bm25_check_field_knob_value loop further down -- so garbage fails at DDL time
 * instead of surviving to the next REINDEX. amoptsprocnum stays 0; this is the
 * C amoptions hook. */
bytea *
bm25_options(Datum reloptions, bool validate)
{
    static relopt_kind  bm25_relopt_kind = 0;
    static const relopt_parse_elt tab[] = {
        {"analyzer",  RELOPT_TYPE_STRING, offsetof(BM25Options, analyzer_offset)},
        {"language",  RELOPT_TYPE_STRING, offsetof(BM25Options, language_offset)},
        {"stopwords", RELOPT_TYPE_STRING, offsetof(BM25Options, stopwords_offset)},
        {"tokenizer", RELOPT_TYPE_STRING, offsetof(BM25Options, tokenizer_offset)},
        {"require_analyzer_match", RELOPT_TYPE_BOOL,
            offsetof(BM25Options, require_analyzer_match)},
        {"key_field", RELOPT_TYPE_STRING, offsetof(BM25Options, key_field_offset)},
        {"store_positions", RELOPT_TYPE_BOOL,
            offsetof(BM25Options, store_positions)},
        {"phrase_fallback", RELOPT_TYPE_STRING,
            offsetof(BM25Options, phrase_fallback_offset)},
        {"k1", RELOPT_TYPE_REAL, offsetof(BM25Options, k1)},
        {"b",  RELOPT_TYPE_REAL, offsetof(BM25Options, b)},
    };
    BM25Options *opts;

    if (bm25_relopt_kind == 0)
    {
        relopt_kind local_kind = add_reloption_kind();
        /* validate_string callbacks reject illegal values at parse time so a bad
         * CREATE INDEX / ALTER INDEX fails before any pages are written. Every
         * string reloption here now carries one except key_field, whose check needs
         * a Relation and so cannot live in a validate_string (see its note below);
         * `analyzer` and `language` were the two that did not (#148 HDL-07), which
         * is why `analyzer` was accepted-and-ignored and a bad `language` survived
         * DDL and only exploded at the next scan.
         *
         * A NON-NULL default_val ALSO MEANS THE VALIDATOR RUNS ON IT, HERE, at
         * registration: init_string_reloption does `if (validator) validator(default_val)`
         * before it allocates anything. For bm25_validate_analyzer that is a strcmp.
         * For bm25_validate_language it is a syscache lookup under a GUC nest
         * push/pop, executed once per backend at the FIRST bm25_options call --
         * including a call with validate = false, i.e. an ordinary relcache load.
         *
         * Measured consequence, because it is larger than it looks: with
         * pg_catalog.english_stem dropped, `SELECT count(*) FROM t` on a table that
         * merely HAS a bm25 index now ERRORs, since loading the index's relcache
         * entry reaches this registration. Before the validator it did not. The
         * precondition is a superuser DROP that also cascades away the `english`
         * text search configuration -- so the database is comprehensively broken
         * already, and every bm25 SCAN in it was failing before this change too --
         * but the blast radius genuinely widened from "the index" to "any query
         * touching the table". The fix, if that is ever judged to matter, is to
         * register `language` with a NULL default_val (this validator returns
         * immediately on NULL) and let bm25_opt_str supply "english" through the
         * fallback described below; that was deliberately NOT done here because it
         * moves a default the analyzer FINGERPRINT is computed from, and getting
         * that wrong is a cluster-wide REINDEX.
         *
         * Otherwise a non-NULL default_val means core's fillRelOptions copies it
         * into the struct even when the user did not set the option -- so the
         * offset is never 0 for any of the five string reloptions that have one
         * (analyzer/language/stopwords/tokenizer below, plus phrase_fallback).
         * Only key_field passes NULL. bm25_opt_str's
         * "offset 0 == absent, substitute the documented default" fallback
         * (bm25_analyzer.c) therefore only fires for an index with NO reloptions at
         * all, where index_reloptions short-circuits to NULL and never calls this
         * function. The two default sets are the same strings and must be kept that
         * way by hand; nothing checks it. */
        add_string_reloption(local_kind, "analyzer", "analyzer name",
                             "english", bm25_validate_analyzer, AccessExclusiveLock);
        add_string_reloption(local_kind, "language", "Snowball language",
                             "english", bm25_validate_language, AccessExclusiveLock);
        add_string_reloption(local_kind, "stopwords", "stopword set",
                             "default", bm25_validate_stopwords, AccessExclusiveLock);
        add_string_reloption(local_kind, "tokenizer", "tokenizer type",
                             "standard", bm25_validate_tokenizer, AccessExclusiveLock);
        add_bool_reloption(local_kind, "require_analyzer_match",
                           "ERROR (true) or WARN (false) on analyzer fingerprint mismatch",
                           true, AccessExclusiveLock);
        /* key_field: no validate_string callback -- the column-existence check lives
         * in bm25_resolve_fields, which matches the name against the INDEX descriptor
         * (key + INCLUDE attributes); bm25_options gets a bare reloptions Datum and no
         * Relation at all. NULL default = absent = key_field_offset 0 = no key.
         * Per-field k1_<attname>/b_<attname>/boost_<attname> knobs are dynamically
         * named and read from the raw reloption array in bm25_resolve_fields, not
         * registered here.
         *
         * The description string below is NOT user-visible: allocate_reloption
         * pstrdup's it into relopt_gen.desc, and core never reads that field back
         * -- no catalog, view, error message or SQL function exposes it (checked
         * against PG 18.3). Treat it as a comment that happens to be a string
         * literal, and keep it true for the same reason the comments around it
         * must be true. It said "heap column name" until 2026-08, contradicting
         * the paragraph directly above it. */
        add_string_reloption(local_kind, "key_field",
                             "index column (key or INCLUDE'd) for the docid->key map (return/score by key)",
                             NULL, NULL, AccessExclusiveLock);
        /* M4: index-wide default for per-field position storage (default true). A
         * per-field store_positions_<attname> knob overrides it (dynamically named,
         * stripped before validation like k1_/b_/boost_, read from the raw reloption
         * array in bm25_resolve_fields). */
        add_bool_reloption(local_kind, "store_positions",
                           "store token positions for phrase/proximity search (default true)",
                           true, AccessExclusiveLock);
        /* M4: how a phrase query degrades against a position-less segment / off field
         * (D7). default_val here is the non-NULL literal "error", so -- per the
         * fillRelOptions behavior explained above -- core copies it into the option
         * struct even when the user never sets phrase_fallback; bm25_phrase_fallback_is_and
         * therefore always reads a real "error"/"and" string, never an absent/offset-0
         * case. "error" is the safe default: never a silent false-positive AND-match. */
        add_string_reloption(local_kind, "phrase_fallback",
                             "phrase query fallback on a position-less segment: "
                             "'error' (default) or 'and'",
                             "error", bm25_validate_phrase_fallback, AccessExclusiveLock);
        /* Index-wide BM25 parameter defaults; per-field k1_<col>/b_<col>
         * override them. Ranges match the per-field knobs' (the finite / >= 0
         * checks and b's <= 1 in match_field_reloption; the k1_<col>/boost_<col>
         * caps in bm25_check_field_knob_value) -- they are preconditions of the WAND
         * bound's monotonicity, not taste. Core checks these ranges only when
         * validate=true (CREATE/ALTER); a stored value is parsed unchecked --
         * the same DDL-only rule the per-field upper caps follow. */
        add_real_reloption(local_kind, "k1",
                           "BM25 k1 (term-frequency saturation), index-wide default",
                           1.2, 0.0, BM25_K1_MAX, AccessExclusiveLock);
        add_real_reloption(local_kind, "b",
                           "BM25 b (length normalization), index-wide default",
                           0.75, 0.0, 1.0, AccessExclusiveLock);
        /* All registrations succeeded; publish to the static. Each failed attempt
         * burns one kind bit, and the backend falls back to re-registering from
         * scratch. After roughly 19 failures add_reloption_kind will raise, which is
         * loud and acceptable. */
        bm25_relopt_kind = local_kind;
    }

    /* Per-field knob VALUES are validated here, at CREATE/ALTER time, because
     * the strip below hides them from build_reloptions and the only other
     * check (bm25_resolve_fields) runs at build -- without this, ALTER INDEX
     * accepted garbage that exploded at the next REINDEX, and out-of-range
     * values would undermine the WAND bound's monotonicity preconditions.
     * validate=false (relcache parse of stored options) stays lenient so an
     * index carrying pre-fix garbage can still be opened and repaired. */
    /* See bm25_strip_field_knobs above: PointerIsValid is gone in PG19. */
    if (validate && DatumGetPointer(reloptions) != NULL)
    {
        List     *opts_raw = untransformRelOptions(reloptions);
        ListCell *lc;

        foreach(lc, opts_raw)
        {
            DefElem *de = (DefElem *) lfirst(lc);

            if (bm25_is_per_field_knob(de->defname))
                bm25_check_field_knob_value(de->defname, defGetString(de));
        }
    }

    /* Strip the dynamically-named per-field knobs (k1_<attname> etc.) before
     * validation -- they are read from the raw catalog reloptions in
     * bm25_resolve_fields, not from this parsed bytea. */
    opts = (BM25Options *) build_reloptions(bm25_strip_field_knobs(reloptions),
                                            validate, bm25_relopt_kind,
                                            sizeof(BM25Options), tab, lengthof(tab));
    return (bytea *) opts;
}

/* Total order over tokens for the qsort/bsearch in bm25_match (and bm25_snippet's hit
 * set, hence extern): length first, then bytes. Ordering by length first keeps the
 * memcmp well-defined (it only ever compares equal-length runs) without needing a
 * shortest-common-prefix rule, and equality under this order is exactly the
 * (len, bytes) equality the match test wants. */
int
bm25_token_cmp(const void *a, const void *b)
{
    const BM25Token *x = (const BM25Token *) a;
    const BM25Token *y = (const BM25Token *) b;

    if (x->len != y->len)
        return (x->len < y->len) ? -1 : 1;
    return memcmp(x->ptr, y->ptr, (size_t) x->len);
}

/*
 * bm25_match -- the @@@ operator's standalone evaluator (bare-filter / seqscan path).
 *
 * OR semantics: tokenize both the document and the query with bm25_analyze, then
 * return true if ANY query token equals any document token. Empty query -> false.
 *
 * bm25_match has NO index Relation in scope (it is a 2-arg text function), so it
 * cannot read the index's analyzer reloptions and resolves the M3 DEFAULT analyzer
 * (english/default/standard) via bm25_analyzer_default_config. The bm25 index scan
 * therefore does NOT route its recheck through this function -- bm25_gettuple sets
 * xs_recheck=false because the index match (made with the index's own analyzer) is
 * authoritative. If the index path DID recheck here, a non-english index would have
 * its correct matches silently dropped by english re-tokenization (the bug this
 * separation avoids).
 *
 * Consequence: a bare `col @@@ 'q'` evaluated as a SEQSCAN filter (no bm25 index
 * answering the query) always uses the default english analyzer -- it has no index to
 * borrow an analyzer from. For a non-english index this differs from the index-scan
 * result; the index scan (the intended path) is the source of truth. M5/positions do
 * not change this: the recheck operator fundamentally lacks index context.
 *
 * The same lack of context bounds what this function will answer at all. It refuses
 * (feature_not_supported) the two query shapes it structurally cannot decide -- a
 * quoted phrase (#132) and a `field:` scope (#298) -- rather than answering a
 * different question. It still answers a bare RHS, against the LHS value only; on a
 * multi-column index the index path matches every field instead (ADR 0004), a
 * divergence this function cannot detect and which is documented, not refused.
 */
PG_FUNCTION_INFO_V1(bm25_match);
Datum
bm25_match(PG_FUNCTION_ARGS)
{
    text               *doc = PG_GETARG_TEXT_PP(0);
    text               *qry = PG_GETARG_TEXT_PP(1);
    BM25Token          *dt, *qt;
    BM25Token          *small, *large;
    BM25AnalyzerConfig  cfg;
    int                 nd, nq, nsmall, nlarge, i;

    /* Seed the M3 keyword defaults, then derive dict OID + hashes. This is the same
     * config bm25_analyzer_config builds when rd_options is NULL.
     *
     * HARD-CODED, AND NOT FIXABLE HERE (SQL-12, #151). These defaults are used whatever
     * the index's reloptions say, so on an index built with language='german' or
     * stopwords='none' this off-index evaluation tokenizes differently from the index --
     * and the recheck path can therefore disagree with the index path about the same row.
     *
     * The reason it stays is structural, not effort. This function receives two bare
     * text Datums. There is no Relation, no Oid, and no PostgreSQL mechanism by which an
     * operator procedure can discover an index over its left argument's source column --
     * the same expression is legal with no index at all. Changing the signature would
     * break the @@@ operator, and a planner-support rewrite cannot help either: at plan
     * time it is not yet known whether the qual lands as an Index Cond (where the index's
     * own analyzer runs and this code never executes) or as a Filter (where it does).
     *
     * Erroring is worse than the status quo, and specifically worse than the precedent
     * it would be imitating. bm25_match_jsonb refuses off-index because a jsonb query
     * tree CANNOT be evaluated without the index -- refusing is the only correct answer
     * it has. This function can evaluate, and on an english index (the default) its
     * answer is right. Turning that into a hard error for every user would trade a
     * narrow, documented divergence for a broad, certain breakage.
     *
     * So it is documented rather than fixed, and pinned by sql/99_query_semantics so the
     * divergence stays a known, tested property instead of a latent surprise. */
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.language, "english", BM25_STEMMER_NAME_LEN);
    cfg.stopwords      = BM25_STOPWORDS_DEFAULT;
    cfg.tokenizer_type = BM25_TOKENIZER_STANDARD;
    bm25_analyzer_default_config(&cfg);

    /*
     * #132: refuse a PHRASE here rather than answering it wrongly.
     *
     * This function is a pure OR-of-tokens intersection: it does not strip quotes,
     * does not parse ~n / ~>n, and has no positions -- it is handed two text values
     * and an analyzer, with no index, no segments and no position chains. So a
     * quoted phrase is a predicate it structurally cannot decide, and answering
     * "do these token sets intersect?" silently returns the OR-union of the
     * phrase's tokens: precisely the bug #132 fixed on the index path.
     *
     * Fixing only the index path would have RELOCATED that bug here rather than
     * eliminating it, and left it silent. Failing loud is the established pattern
     * for exactly this situation: bm25_match_jsonb below refuses for the same
     * reason (docs/adr/0004 records why -- it used to return false and drop every
     * row, "a 0-row lie"), and a bare wildcard @@@ already errors on this path.
     *
     * A bare term is unaffected by this check; only a quoted phrase is refused
     * here (a field-scoped term is refused by the #298 check below, an unscoped
     * one evaluates normally). The predicate is shared with the scan's micro-parser
     * (bm25_query_phrase_offset) so the two paths cannot drift about what counts
     * as a phrase.
     */
    if (bm25_query_phrase_offset(VARDATA_ANY(qry), VARSIZE_ANY_EXHDR(qry), true, NULL))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: the \"@@@\" match operator with a phrase query "
                        "can only be evaluated by a bm25 index scan"),
                 errdetail("The query was planned to apply \"col @@@ query\" as a filter or recheck qual "
                           "rather than as a bm25 index condition. "
                           "A phrase is a positional predicate, "
                           "and this evaluation path has no access to the index's position chains, "
                           "so it would otherwise silently return the union of the phrase's tokens."),
                 errhint("Ensure the predicate is answered by the bm25 index: "
                         "anchor \"@@@\" on the index's first indexed column, set enable_seqscan = off, "
                         "and avoid an ORDER BY or competing index that displaces the bm25 index scan.")));

    /*
     * #298: refuse a `field:` SCOPE here too, on the same reasoning as the phrase.
     *
     * A scope names a field of the bm25 index; this function has no index to resolve
     * it against, so the analyzer would read `body:cat` as the two terms `body` and
     * `cat` and test them against the LHS column -- rows that mention the word "body"
     * match, rows whose body field holds "cat" do not. Unlike the analyzer divergence
     * above, that answer is wrong on every configuration, english included, and the
     * filter path is not exotic: a non-owner on an RLS table (bm25_match is not
     * LEAKPROOF, so core keeps it above the policy qual), any `@@@ ... OR ...` (no
     * amgetbitmap), enable_indexscan = off. Stripping the prefix instead would still
     * answer an unscoped question silently.
     *
     * The predicate is bm25_query_field_prefix, shared with the phrase test above and
     * with the index path's split, so `meeting 10:30` (whitespace before the colon)
     * stays a bare term here and there. `http://x` and `10:30` are refused here even
     * though the index path, which can see that `http` and `10` name no field, reads
     * them as literal text (#306): this function cannot tell them from `title:2024`,
     * which the index scopes. Refuse or agree, never silently differ.
     * Tested AFTER the phrase check so a scoped phrase keeps its #132 error.
     */
    if (bm25_query_field_prefix(VARDATA_ANY(qry), VARSIZE_ANY_EXHDR(qry), NULL))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: the \"@@@\" match operator with a field-scoped query "
                        "can only be evaluated by a bm25 index scan"),
                 errdetail("The query was planned to apply \"col @@@ query\" as a filter or recheck qual "
                           "rather than as a bm25 index condition. "
                           "A query with a colon before any whitespace or quote (\"title:x\", but also \"http://x\") "
                           "may be scoped to a field of the bm25 index, "
                           "and this evaluation path has no index to resolve the name against, "
                           "so it could otherwise search a field name as a term in the left-hand column."),
                 errhint("Ensure the predicate is answered by the bm25 index: "
                         "anchor \"@@@\" on the index's first indexed column, set enable_seqscan = off, "
                         "and do not combine it with OR or with an ORDER BY or competing index that displaces "
                         "the bm25 index scan. A row-level security policy on the table also forces this "
                         "filter path for roles it applies to.")));

    nd = bm25_analyze(&cfg, VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc), &dt);
    nq = bm25_analyze(&cfg, VARDATA_ANY(qry), VARSIZE_ANY_EXHDR(qry), &qt);

    /* OR: true if the two token sets intersect. Both operands come straight from SQL
     * with no length cap beyond text's own and the tokenizer's per-document token
     * ceiling (bm25_tokenize.c's BM25_MAX_DOC_TOKENS, tens of millions per side), and the
     * all-miss case is both the worst case and the common one for a nonsense query. The
     * obvious doubly-nested scan was therefore O(nd x nq) -- days of CPU at that
     * ceiling -- with no interrupt point anywhere in
     * it or in memcmp, so the backend ignored pg_cancel_backend, pg_terminate_backend and
     * statement_timeout for the whole run while holding its snapshot open and pinning the
     * vacuum horizon cluster-wide. COST 5000 discourages the planner from choosing this
     * path but does nothing about an explicitly written call (H15).
     *
     * Sort the SMALLER set and probe it with the larger: O(s log s + l log s). Sorting
     * the smaller side matters for the realistic shape (a short query against a long
     * document), where it is the query that gets sorted and the probe is a handful of
     * comparisons per document token. sort+bsearch rather than a hash because it is exact
     * by construction and builds nothing per call. When this was written bm25_accum's
     * HTAB had a fixed-size key and needed a linear collision fallback, which turned out
     * to be aimable from the input (#58, then #305); it now keys on (pointer, length)
     * over the full term with no fallback, so a hash here would be safe too -- the
     * choice stands on cost, not on that caveat.
     *
     * The residual uninterruptible stretch is the qsort itself, which at the tokenizer's
     * ceiling is ~3e8 comparisons (seconds), not days. The two bm25_analyze calls above
     * are interruptible per word run. */
    if (nd == 0 || nq == 0)
        PG_RETURN_BOOL(false);      /* empty query (or empty doc) matches nothing */

    if (nq <= nd)
    {
        small = qt;
        nsmall = nq;
        large = dt;
        nlarge = nd;
    }
    else
    {
        small = dt;
        nsmall = nd;
        large = qt;
        nlarge = nq;
    }

    qsort(small, (size_t) nsmall, sizeof(BM25Token), bm25_token_cmp);

    for (i = 0; i < nlarge; i++)
    {
        CHECK_FOR_INTERRUPTS();
        if (bsearch(&large[i], small, (size_t) nsmall, sizeof(BM25Token),
                    bm25_token_cmp) != NULL)
            PG_RETURN_BOOL(true);
    }

    PG_RETURN_BOOL(false);
}

/*
 * bm25_match_jsonb / bm25_distance_jsonb -- M6 anchors for the (text,jsonb)
 * overloads of @@@ / &@@ (jsonb query trees built by bm25_match_terms/term/
 * phrase/wildcard/boolean/boost, see bm25_native--1.0.sql). They exist so
 * the operators can be created and added to text_bm25_ops (CREATE OPERATOR
 * requires a resolvable procedure). bm25_match_jsonb is NEVER reached on the
 * index path (bm25_gettuple sets xs_recheck=false for its own matches; the AM
 * exposes no amgetbitmap / amcanreturn) -- it is called ONLY when the planner
 * applies `col @@@ jsonb` as a filter / recheck / seqscan qual, which cannot
 * match a jsonb query tree from a single heap column value. It used to
 * PG_RETURN_BOOL(false) there, silently dropping every row (a 0-row lie); it now
 * ERRORs (fail-loud), so the misuse is surfaced rather than returning wrong
 * results. Real jsonb @@@ evaluation happens in bm25_rescan_parse_jsonb /
 * bm25_query_eval on the index path, never here.
 *
 * bm25_distance_jsonb is NOT inert: like its (text,text) sibling bm25_distance
 * (see bm25_score.c), &@@ is projected per-row on the Index Scan node's own
 * target list (a resjunk column) independent of xs_recheckorderby, so this
 * function runs for real on the index path too. It returns the stashed distance
 * of the row most recently emitted BY THE SCAN THAT RANKED THIS QUERY, resolved
 * through the shared bm25_distance_for_query (#138); off the index, or when no
 * live scan ranked the projected query, it degrades to +infinity since there is
 * no per-row score to report. On that fall-through it first validates the query
 * tree (#245), because no scan parsed it: see bm25_distance_jsonb_validate.
 */
PG_FUNCTION_INFO_V1(bm25_match_jsonb);
Datum
bm25_match_jsonb(PG_FUNCTION_ARGS)
{
    /*
     * Only reachable when the planner applies `col @@@ jsonb` as a filter /
     * recheck / seqscan qual (a competing ORDER BY that steals another index, a
     * cheaper alternative index, a cost/stats flip) instead of a bm25 Index Cond.
     * A jsonb query tree cannot be matched here -- this fn gets one heap column
     * value and a query that may scope to a DIFFERENT field, with no index handle,
     * segments, or analyzer. The old PG_RETURN_BOOL(false) silently dropped every
     * row; erroring surfaces the misuse (the correct index path never calls this).
     */
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bm25: the \"@@@\" match operator with a jsonb query "
                    "can only be evaluated by a bm25 index scan"),
             errdetail("The query was planned to apply \"col @@@ jsonb\" as a filter or recheck qual "
                       "rather than as a bm25 index condition; "
                       "a jsonb query tree cannot be matched without the index, "
                       "so it would otherwise silently return no rows."),
             errhint("Ensure the predicate is answered by the bm25 index: "
                     "anchor \"@@@\" on the index's first indexed column, set enable_seqscan = off, "
                     "and avoid an ORDER BY or competing index that displaces the bm25 index scan; "
                     "for ranked retrieval use \"col @@@ query ORDER BY col &@@ query\".")));
    PG_RETURN_BOOL(false);      /* unreachable; silences -Wreturn-type */
}

/* The last query tree bm25_distance_jsonb_validate accepted at this call site,
 * held in fn_extra (fn_mcxt, so it lives as long as the expression). Only a
 * VALID tree is ever stored: an invalid one raises, so there is nothing to
 * remember.
 *
 * #272: validity is a function of the bytes AND of the three PGC_SUSET wildcard
 * guardrails validate_wildcard_pattern reads (bm25_query.c), so the verdict is
 * keyed on all four. fn_extra outlives a set_config() between two evaluations of
 * the same expression (at least one statement over several rows, sql/115), and
 * keying on the bytes alone kept accepting a tree a tightened GUC now rejects.
 * Keying beats an assign-hook invalidation: no global state, and a GUC that
 * changes and changes back still hits. A new GUC read on the validate path must
 * be added here. */
typedef struct BM25DistJsonbSeen
{
    int  min_prefix;            /* bm25_wildcard_min_prefix at validation */
    int  max_pattern_length;    /* bm25_wildcard_max_pattern_length */
    int  max_stars;             /* bm25_wildcard_max_stars */
    int  len;
    char bytes[FLEXIBLE_ARRAY_MEMBER];
} BM25DistJsonbSeen;

/*
 * bm25_distance_jsonb_validate -- #245. When no scored scan owns the projected
 * query (the resolver's +inf fall-through: no scan at all, as in ORDER BY &@@
 * with no @@@ predicate, or scans that rank a different query), nothing has
 * parsed the tree, so an invalid one was silently accepted here while the index
 * path rejects it. Parse it structurally (bm25_query_validate: no field
 * resolution, since there is no index to resolve against) and let its error
 * propagate. The +inf result for a VALID tree is unchanged (TEXT-01 / #151).
 *
 * Structural, and also not the whole of the index path's check (documented
 * residual, #273): bm25_query_validate skips the checks bm25_rescan_parse_jsonb
 * applies after flatten, so a tree with a PHRASE leaf inside must_not, which the
 * index path rejects, is accepted here and projects +inf like any other valid tree.
 *
 * Cached by value per call site, so a constant RHS costs one parse per query
 * rather than one per row; an RHS that varies row to row re-parses only when it
 * changes, as does any RHS after a wildcard guardrail GUC changes (#272).
 * jsonb's binary form is canonical per value, so a byte compare is a sound
 * equality test (the resolver relies on the same property).
 */
static void
bm25_distance_jsonb_validate(FunctionCallInfo fcinfo, Jsonb *jb)
{
    const char        *rhs = (const char *) VARDATA_ANY(jb);
    int                len = (int) VARSIZE_ANY_EXHDR(jb);
    FmgrInfo          *flinfo = fcinfo->flinfo;
    BM25DistJsonbSeen *seen = flinfo ? (BM25DistJsonbSeen *) flinfo->fn_extra : NULL;

    /* GUCs first: three int compares are cheaper than the memcmp they can skip. */
    if (seen != NULL &&
        seen->min_prefix == bm25_wildcard_min_prefix &&
        seen->max_pattern_length == bm25_wildcard_max_pattern_length &&
        seen->max_stars == bm25_wildcard_max_stars &&
        seen->len == len && memcmp(seen->bytes, rhs, len) == 0)
        return;

    bm25_query_validate(jb);    /* ERRORs on an invalid tree */

    if (flinfo == NULL)
        return;                 /* direct call: no per-call-site cache to fill */
    if (seen != NULL)
        pfree(seen);
    seen = MemoryContextAlloc(flinfo->fn_mcxt, offsetof(BM25DistJsonbSeen, bytes) + len);
    seen->min_prefix = bm25_wildcard_min_prefix;
    seen->max_pattern_length = bm25_wildcard_max_pattern_length;
    seen->max_stars = bm25_wildcard_max_stars;
    seen->len = len;
    memcpy(seen->bytes, rhs, len);
    flinfo->fn_extra = seen;
}

PG_FUNCTION_INFO_V1(bm25_distance_jsonb);
Datum
bm25_distance_jsonb(PG_FUNCTION_ARGS)
{
    Jsonb *jb = PG_GETARG_JSONB_P(1);   /* the query the caller wrote after &@@ */
    bool   owned;
    float8 dist;

    /* #138: the SAME resolver its (text,text) sibling uses -- see
     * bm25_distance_for_query in bm25_score.c for why the registry head is the
     * wrong answer. This body is why the resolver is shared rather than
     * duplicated: it is the copy that would be forgotten. jsonb's binary form is
     * canonical per value, so a byte compare is a sound equality test. */
    dist = bm25_distance_for_query(true, (const char *) VARDATA_ANY(jb),
                                   VARSIZE_ANY_EXHDR(jb), &owned);

    /* An owning scan parsed this exact tree (byte-identical RHS) at rescan, with
     * the index's field config, so only the fall-through needs checking. */
    if (!owned)
        bm25_distance_jsonb_validate(fcinfo, jb);

    PG_RETURN_FLOAT8(dist);
}
