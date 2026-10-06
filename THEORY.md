# THEORY OF OPERATION

Why `bm25_native` is built the way it is — for an engineer about to change it.
This document is narrative and is read once to build a model; the enumerated
module map and binding invariants live in [ARCHITECTURE.md](ARCHITECTURE.md) and
are not repeated here. Where this document refers to "the invariants," it means
that enumeration; here we explain *why each holds and what it costs*.

ARCHITECTURE.md's invariants and this document describe the current code. The
per-decision record — context, alternatives considered, consequences — is the
ADR log in [docs/adr/](docs/adr/), where a record's addenda carry its later
amendments.

## 1. The problem, and why an LSM

The goal is Okapi BM25 ranked full-text search delivered as an ordinary
PostgreSQL index access method — `CREATE EXTENSION`, `CREATE INDEX … USING bm25_native`,
no external engine, no bespoke runtime. The mental model is *GIN's storage model
extended with term frequencies, document-length norms, a BM25 scorer, and
score-ordered retrieval.* Because the whole index lives in the index relation's
own pages, it inherits WAL, physical replication, crash recovery, and
`pg_basebackup` from core for free.

M0/M1 (on `main`) proved the AM contract end-to-end with a single, mutable,
in-place segment (format v2). That layout hit walls that motivated the M2a
rewrite to a segmented LSM:

- **Scale ceilings** — a single page per term (~680 docs/term) and a single
  dictionary page (~few hundred terms).
- **Expensive, awkward deletes** — no cheap way to retract a doc from a packed
  in-place posting list.
- **Single-writer concurrency races** — TOCTOU on new-term creation, lost
  updates on `df`/stats.
- **Stats that only self-healed at `REINDEX`.**

The segmented-LSM answer (the same shape Lucene and ParadeDB use): an
append-only **pending list** absorbs writes; it is **sealed** into **immutable**
block-compressed **segments**; segments are combined by **tiered merge**; deletes
are a **tombstone-bitmap bit flip**; space is reclaimed only at merge. The
decisive property is **immutability**: nothing mutates a shared segment page in
place, which *structurally* dissolves the M0/M1 write races rather than locking
around them. Reads take no *coordination* lock on segment contents: a reader
still takes each page's buffer content lock while it reads that page's bytes,
but it holds nothing across pages and nothing that a writer must wait behind.
Keeping that true is an ongoing discipline rather than a free consequence of the
design — a reader that decodes, allocates, or nests a second page read while
still holding a page's content lock has quietly reintroduced the coordination
this shape exists to avoid (`docs/adr/0063`, `docs/adr/0083`).

This is a hard break — on-disk **format v3** — and a v2 index must be `REINDEX`ed.
The break was taken once, deliberately, to carry every v3 feature at once (dense
local doc-ids, block postings with a reserved per-block max-impact field, the
multi-page dictionary, the metapage LSM directory, per-page `seg_gen`). The
reserved max-impact field was meant to let block-max WAND layer on with no second
break. In the event (§4, §6) that prediction only half held: WAND *did* arrive on
the reserved slot, but the single reserved `float4` turned out to be the wrong
shape for a *safe* bound, so M2b broke the format once more (v4→**v5**) to replace
it with a per-field impact table. The durable lesson is that reserving *space* is
easy; reserving the *right shape* for a bound whose inputs (idf/avgdl) are only
known at scan time is what the v3 guess missed.

## 2. The three constraints, and what each buys and costs

Three hard constraints are the load-bearing theses. Every structural decision
downstream is a consequence of one of them.

**Generic WAL only — no custom resource manager.** All durability rides on
`GenericXLogStart/Finish`, replayed by core's `generic_redo`. *Buys:* correct
crash recovery and free physical-standby reads on stock Community PostgreSQL —
the thing ParadeDB gates behind an Enterprise custom rmgr that refuses to replay
on a Community standby. "Free correct replication is worth more than the WAL
bytes." *Costs:* heavier WAL volume than a hand-tuned rmgr (accepted, flagged for
the perf pass), and — critically — it makes the nbtree/GiST page-reuse-conflict
WAL record **unbuildable here**, because that record is replayed by a per-AM rmgr
calling `ResolveRecoveryConflictWithSnapshotFullXid`, and `generic_redo` has no
such hook. That single fact forces the reuse-safety design in §5.

**No background workers.** All maintenance — seal, merge, reclaim — runs inline
in user backends, in VACUUM/autovacuum AM callbacks, or via an explicit
maintenance function. *Buys:* no daemon to ship or supervise. *Costs:* a large
merge that lands inline blocks the triggering statement (mitigated by keeping the
inline tier small and pushing big merges to autovacuum). ParadeDB's dynamic
background worker is re-expressed as: small merges inline, large merges in
`amvacuumcleanup` at autovacuum cadence (autovacuum workers *are* core background
processes), plus a manual `bm25_merge()` backstop.

**Preload-free.** `shared_preload_libraries` is never required for a v1 feature.
*Buys:* removes the operational barrier that gates ParadeDB-style planner-hook
retrieval on PG<17. *Costs / consequence:* ranked top-N cannot go through a
planner-hook `CustomScan` (which needs the library's hooks active). Instead it
goes through **`amcanorderbyop`** — the KNN/pgvector ordering-operator mechanism.
`WHERE col @@@ 'q' ORDER BY col &@@ 'q' LIMIT k` emits an ordered index scan with no Sort
node (the `@@@` qual is required: `amoptionalkey` is false, so an ORDER BY alone is not an
index path and plans as Seq Scan + Sort with every distance `+inf`),
consulted by the planner whenever the index is considered, needing no hook and no
preload. Because KNN orders *ascending by distance*, the operator returns a
quantity that is monotone-decreasing in score (e.g. `-score`). Aggregate/facet
pushdown — which only a `CustomScan` can do — is deferred to an optional,
gracefully-degrading, preload-gated add-on.

## 3. The write path: pending list → seal → segment

Inserts append to a **WAL-logged pending list** (block 0 anchors
`pending_head`/`tail`, exactly as `GinMetaPageData` does). This was chosen over a
per-backend RAM buffer (ParadeDB's `RamDirectory`) because a RAM buffer is not
WAL-logged and violates the durability thesis, and over seal-per-row because that
produces 1-doc segments. Batching in the pending list is the compromise.

A **seal** drains the pending chain into a `BM25Accum` (which assigns dense local
doc-ids 0..N−1 in TID order and sorts terms lexicographically), builds an
immutable segment, and publishes it. Sealing happens at a size threshold
(`bm25_native.seal_threshold`, default 4096 KB), during VACUUM, or on demand via
`bm25_seal()`. Opportunistic inline seal runs under a **conditional** singleton
`LockPage` on the metapage held across the whole drain+build+commit: an inserter
that loses the race must never block (it just appends and moves on), and two
concurrent sealers must never publish duplicate segments.

Why a segment install is **two-phase** (orphan pages first, then one final
metapage-flip record): a sealed segment spans far more than the Generic WAL
4-buffer cap (`MAX_GENERIC_XLOG_PAGES`), so it can never be committed in one
record. So all the segment's pages are built as orphans first (many small
records, metapage untouched), and a single final record is the atomic
linearization point — the canonical bloom/BRIN-revmap/nbtree-build pattern.

The subtle, dangerous part is **why the pending-head advance must ride inside
that same publish record** (the *single-record seal* invariant). GIN gets to
publish-then-truncate as separate, idempotent steps because its pending scan is a
bitmap OR — revisiting a doc is harmless. **BM25 scores are additive.** If a
crash landed in a publish-before-truncate window, the redrained docs would be
double-scored and global stats double-counted. So the seal advances
`pending_head` in the *same* record that publishes the segment, and the separate
`bm25_pending_truncate` is demoted to a post-commit page-recycler that frees
drained pages to the FSM and never touches `pending_head`. Re-seal is explicitly
*not* relied on for idempotency. A crash lands entirely before (pending live,
segment absent) or entirely after (segment live, head advanced) — never in
between.

### 3a. The memory budget, and why "one operation, one record" survived it

The accumulator is a pure in-memory inverted index with no spill and no
`tuplesort`, so for a long time its residency was bounded only by the corpus. That
was tolerable for a manual `bm25_seal()` and intolerable everywhere else: the merge
and the pre-merge seal both run from `amvacuumcleanup`, and an OOM in an autovacuum
worker has no user-visible failure path — the index just quietly stops being
maintained. So all three feeders (build, merge, drain) now measure the accumulator
against `maintenance_work_mem` — `autovacuum_work_mem` when the process is an
autovacuum worker and one is configured, GIN's `ginInsertCleanup` precedence — and,
when it is over, seal what they hold as a segment and start a fresh one. Budget
exhaustion **seals; it never errors**, because an error on either of those paths is
the disease rather than the cure.

The interesting question is what that does to the seal invariant above, and the
answer is: nothing, but only because the generalization is chosen carefully. It is
tempting to say "one merge used to be one output segment, now it is N, and since
each publish is atomic the crash story is unchanged". **That is wrong**, and it is
wrong in the same way the GIN publish-then-truncate comparison is wrong. Atomicity
was never the property doing the work — consistency of the *intermediate states*
was. Between two publish records the catalog is a fully committed, WAL-durable
state that other backends read; and because a scan captures `pending_head` and the
whole live catalog under one metapage lock and then *sums* per-TID contributions
with no cross-source dedup, any such state that holds a partial output alongside
its still-live source double-scores every document in it and returns it twice on
the membership path. A crash there makes that permanent until some later merge
happens to repair it. Retiring the inputs first instead deletes live documents.
There is no interleaving of per-chunk publishes over an arbitrary cut that avoids
both.

So the invariant generalizes from **"one operation, one segment"** to **"one
operation, one atomic publish record"** — which is what it always meant. Every
chunk is built as orphan pages and they all become visible together: the seal/build
path appends the whole entry array and resets the pending anchor in one record, and
the merge path publishes every new entry and retires every input in one record. This
costs nothing, which is why it is available at all: the fresh catalog chain is laid
down as its own orphan records beforehand, so both records stay at two buffers
regardless of how many segments they publish.

Where the cut may fall is decided by the same "one TID must not land in two
segments" reasoning:

- **Merge: input-segment granular.** The replay finishes one source segment before
  starting the next, and within a segment it streams postings *per term across
  docs*. A mid-segment cut would therefore leave every document of that segment
  holding terms A..K in one output and L..Z in another — the same TID in two
  segments, permanently double-scored. A doc-range window is expressible (the
  id-remap already skips tombstoned docs) but costs a full dictionary and postings
  replay per window, and it exists only to serve a shrinking legacy case, so it is
  refused.
- **Drain: page granular, and never under a content lock.** A document may span
  pending pages as same-TID continuation records, but its parts are buffered in the
  drain's own memory context and nothing reaches the accumulator until the document
  is complete — so a document mid-assembly at a page boundary has contributed
  nothing to the chunk being sealed and lands whole in the next one. The check sits
  before the page's content lock is taken, because sealing allocates pages and
  writes WAL and interrupts are deferred while an LWLock is held.
- **Build: per document.** Nothing can observe the intermediate states at all —
  under `CONCURRENTLY` the index is `indisready = false` until `index_build`
  returns, and otherwise the relation is uncommitted or exclusively locked — so this
  is the one path that needs no atomicity argument across chunks.

The honest cost is a **floor under the segment count**. A rung of budget-sized
segments merges into the same number of budget-sized segments, so
`BM25_TARGET_SEGMENT_COUNT` is unreachable for a corpus whose accumulator footprint
exceeds eight times the budget, and scan cost grows with the resulting segment
count. Two things follow. First, the merge needs a byte estimate per candidate and
a trim that only accepts sets whose merge can actually *reduce* the count —
otherwise the forced-merge loop re-selects the same rung forever — plus a progress
check in the executor as the backstop that makes termination independent of the
estimate's quality. Second, the remedy is operator-visible and documented: raise
`maintenance_work_mem` and run `bm25_merge()`. The real cure is a streaming k-way
segment merge that needs O(1) memory, which this format permits in principle — the
dictionaries are sorted, the postings are docid-ascending, and the id remap is
order-preserving — but that is a new segment builder rather than a bound on the
existing one.

## 4. The read path: snapshot, score once, dedupe

A scan must produce a consistent, exactly-once-scored ranking over a moving
target (concurrent seals append segments and advance the pending head). The
**Model-A snapshot** (`bm25_scan_snapshot`) handles this: under **one** metapage
SHARE lock it captures `pending_head`, `segcat_root`, `nsegs`, the global stats
cache, **and a full copy of every live `BM25SegCatEntry`**. After the lock
releases, the scan walks the captured catalog and reads segment data pages
without re-locking the metapage or re-reading a catalog page.

Reading the pending head and the catalog under *two separate* lock acquisitions
would be wrong: a seal wedging between them would either double-score a doc
(counted in both the still-current pending head and the freshly-published
segment) or silently drop one. Copying the catalog is cheap (a handful of pages,
infrequent contention), and it pays a second dividend: because no scan ever
re-reads a catalog page after its snapshot, **old catalog pages freed by a future
merge swap need no XID-horizon gating** — an ordinary orphan sweep reclaims them. (The
sweep is gated on evidence since issue #300, `docs/adr/0116`, so a swap leaves its orphan bracket open
precisely so that the next sweep runs and does this.)
Only the large segment *data* pages, read lock-free after the snapshot, need
horizon-gating (§5).

The metapage lock held across that catalog walk also fixes a buffer lock order for
the whole index: metapage before catalog page. The reader's direction is the one that
cannot move, since the held lock is what stops a publish relocating the chain under
the copy, so every writer conforms (`docs/adr/0018`; the binding invariant is
enumerated in ARCHITECTURE.md). The subtlety, found only later (issue #240), is that
a standby does not replay a Generic WAL record in the primary's acquisition order: it
locks each registered block in *registration* order and holds them all until the
record is applied. On the primary registration takes no locks, so a record that
registered a catalog page before the metapage looked correct there and was an
inversion on a standby. The rule is therefore stated on registration order, and the
guard is a WAL-level check that the metapage is block 0 (`t/021`), not an in-code
assertion. The deadlock was derived from the server source and never reproduced:
reproducing it needs a pause point under a buffer lock, which the pause hook
(`bm25_debug_pause_point`) is documented not to be called from.

The catalog copy is also the one place on the read path where a bad pointer can hurt
more than the query that read it. A corrupt `nextblk` that stays inside the relation's
extent used to have three outcomes: a cycle through entry-bearing pages filled the
copy's entry count with duplicates, so the aggregate check passed and a scan answered
from the same few segments with no error; a link to a page of another kind had that
page's bytes read as catalog entries; and a cycle through pages with no entries never
advanced the count, and because the loop runs under the metapage lock, whose acquisition
holds interrupts, it could not be cancelled while every writer needing the metapage
queued behind it. So each walker now checks the page kind, refuses an empty page that
links onward, caps the walk at `nsegs + 1` pages before reading the next one, and, for
the two that copy, refuses a generation listed twice. The four checks are not
redundant: a packed root linked to itself passes the first three and fills the count
within the cap, and only the duplicate check sees it. The threat model is on-disk
corruption and nothing else, which is why the checks assume the catalog's packing
(every linked page holds an entry, except a lone empty root or the chain's last page) and
trust `meta.nsegs`. A corrupt `nsegs` is a documented residual (`docs/adr/0095`'s addendum).
A link to a segment header page, once a residual too, is now refused: catalog and header pages
share a page-kind bit, and `seg_gen` tells them apart for free (0 on a catalog page, the
segment's gen on a header), so the catalog walkers and the appender check that role (#302/#303,
`docs/adr/0062`'s 2026-10-05 addendum). Which corrupt shapes must raise and which may stay
silently wrong is now written down as one rule, `docs/adr/0119`: a corrupt page must never
crash, read or write out of bounds, hang, write into a page of another kind or free a
reachable page, while a wrong answer from in-range values is caught only where a cheap
structural invariant exists. Without that sentence every review re-argued the question. The same record explains why the
extent bound for these walkers re-samples once before failing, and why the pending sweep
needed the same treatment against concurrent appenders.

The scorer (`bm25_scan_build_ranking`) is, in its reference form, the M1 exhaustive
OR-sum scorer extended to multiple sources: it scores each matching TID across all
live segments plus the pending list. (As of M2b the *default* ranked path is
block-max WAND, below; this exhaustive union remains the bit-exact reference and the
fallback for phrase/AND/`@@@`, `wand_top_k=0`, and the over-pull tail.) It **dedupes
by heap TID, pending-wins.** The
dedupe is *not* there to cover a seal-crash window — the single-record seal makes
that window nonexistent. It exists for **UPDATEs**: an UPDATE is delete+insert, so
a live pending entry for the new tuple coexists with a stale tombstoned segment
posting for the old tuple version until VACUUM tombstones the old one and merge
removes it. Pending-wins scores such a TID exactly once, from its current version.
The `n_rows == n_ctids` assertion is the proof that no live doc is emitted twice.

Postings store dense local doc-ids, not raw TIDs, because dense ascending ids are
delta-compressible and block-skippable (and WAND-ready); a per-segment
docid→heap-TID `DOCMAP` is dereferenced only for matched docs. All of a segment's
terms share **one** POST chain (D-POST) so that future retirement collects a
single chain rather than re-walking it per dictionary term — which is exactly why
the decoder must be `df`-bounded and must reset its delta base per block (the
*block independence* invariant; a real decoder bug here, carrying the delta base
across blocks, corrupted every multi-block term and was caught only by a
2500-doc CI suite).

### 4a. The default read path: block-max WAND (M2b)

Scoring *every* matching doc to return ten is wasteful, so M2b makes the default
ranked path **block-max WAND**: skip whole posting blocks whose best-possible score
cannot enter the current top-k. Three problems had to be solved for this to be both
correct and to fit the LSM.

**A safe bound, evaluated late.** Each v5 block carries a per-field impact table —
`(max_tf, min_doclen)` per field — filling the v3 `max_impact` slot (§1). The
per-block upper bound is `Σ_field boost·termscore(idf, max_tf, min_doclen, avgdl,
k1, b)`. Two properties make it *safe*: `termscore` is monotone (rises with tf,
falls with doclen) and `idf ≥ 0` (the Lucene "+1" form), so `(max_tf, min_doclen)`
dominates every real posting in the block. Crucially the bound stores **raw
ingredients** and is evaluated at **scan time**, never as a seal-time scalar —
because `idf` and `avgdl` are global and drift as tombstones and merges move the
corpus; a value frozen at seal could later fall *below* a live score and prune a
doc that belonged in the answer. Using the exact `termscore` (not an inlined copy)
also makes the bound equal a real score bit-for-bit at the boundary where a block's
extremes coincide with one doc — an inlined copy could round a ULP low and become
unsafe. "Monotone" is an exact-arithmetic statement: tf sits in both numerator and
denominator, so the floating-point evaluation can invert adjacent tf values by a few
`DBL_EPSILON` (at `k1 = 0` for any tf). The bound survives because `wand_widen_ub` widens it by
more than that (#312, the `bm25_wand.c` header). The bound also trusts the impact table's
values; a load-time cross-check was measured in the pruning regime, cost about 2.7% on a
two-field index, and was left out under the rule that a hot-path validator lands only if it
is free (`docs/adr/0072`'s 2026-10-05 addendum).

**Fitting WAND to a multi-segment LSM.** WAND wants per-term cursors over one ordered
docid space with skip. That exists *within* a segment (ascending local doc-ids,
independently decodable blocks) but not *across* segments (local ids, dedup-by-TID).
So block-max WAND runs **per segment** under **one shared global top-k heap**: the
heap's k-th score is the threshold θ, and processing segments sequentially lets θ
rise so later segments prune harder. A term's global upper bound (the max block
bound over its blocks) drives the WAND pivot and gives *safe termination*; the
block-max deep-check then shallow-skips, but the skip is **capped at the next
cursor's docid** — skipping further would leap over a docid where a currently-lagging
term also matches, dropping a qualifying doc (a real bug caught in review). Pending
has no blocks, so it is a **non-prunable arm**: scored exhaustively first (priming θ
and registering its TIDs for the same pending-wins dedup), never skipped. Phrase,
proximity, AND-fallback, and boolean `@@@` stay on the exhaustive path, because their
post-accumulation filter (§8) can drop a doc *after* scoring — score-pruning it early
would be unsound.

**Identical answers, not merely close.** WAND's whole value is that it changes
latency, not results, so it must return exactly what the exhaustive scorer would —
the same tuples, the same order, and the same score *bits*. IEEE addition is not
associative, so this is a real constraint: WAND accumulates a candidate's score one
posting at a time in query-term then field order (the exhaustive accumulator's
sequence), routes both the bound and the score through a named `contrib` intermediate
so no fused multiply-add collapses two roundings into one, and drains its heap in the
exact score-desc/tid-asc order. The Makefile pins `-ffp-contract=off` so the compiler
cannot re-fuse across that barrier or diverge between the two paths across CI's
compilers — and that flag, not the named intermediate, is what actually holds under
GCC, whose `-ffp-contract=fast` default will forward-substitute a single-use temporary
and fuse across it anyway. The per-term idf and the pending arm are not merely computed
the same way on both paths: they are the same functions (`bm25_term_idf`,
`bm25_pending_score_term`). Those functions live in `bm25_stats.c`, below both builders,
so WAND reaches them without calling back into the scanner that dispatched it
(`docs/adr/0093`).

**But that constraint belongs to the score, not to the bound.** The file originally
asked the same bit-exactness of every *bound* accumulation, and could not deliver it:
the block-max deep check sums over cursors re-sorted by docid each iteration while the
score is built in fixed query-term order, and `bm25_block_ub` returns a per-term
subtotal while the scorer folds every `(term, field)` posting flat — so the two
regroup relative to each other as soon as a term after the first matches more than one
field. The tempting repair (walk query order, add an accumulate-into-caller bound
variant) works at the deep check and is *structurally impossible* at the other prune
site: the pivot sum folds `global_ub`, a per-term bound precomputed from every block
(since issue #289, `docs/adr/0113`, per-field maxima over blocks summed over fields) — inherently a sum of
precomputed subtotals — and must walk docid order because that prefix scan **is** the
WAND pivot rule. Fixing one site would have left the file asserting an invariant it
still violated at the other. So the contract was split instead: the bound owes only
**domination**, and each prune comparison is widened by a relative slack sized to the
summand count, always in the direction that scores more candidates rather than fewer.
Widening a prune cannot change the answer — the survivors still go through the identical
exact scoring path — which is why this buys correctness at no cost to the guarantee
above. The ascending-`field_id` fold survives as a slack minimiser rather than a
correctness dependency. The one thing it now rests on is that every summand is
non-negative, which the idf clamp and the boost/reloption validation enforce; see
`docs/adr/0043`. Because a PostgreSQL index AM never sees
`LIMIT`, WAND takes k from the `bm25_native.wand_top_k` GUC (default 100); if the executor
pulls *past* k, the scan falls back to building the full exhaustive ranking for the
tail (through the seg_gen retry wrapper, §5) and resumes — the first k rows are
provably identical and already returned.

The trap this whole scheme walks around: bit-exact parity **cannot** prove pruning
happens, because a WAND that decodes every block still returns the exact answer. So
the suite separately witnesses pruning with `blocks_skipped`/`deep_check_skips > 0`
gates — the same "a green test that passes with the feature neutered is a bug"
discipline that the M2a hollow-reclaim finding taught.

Once pruning works, what a ranked build costs is buffer accesses, and profiling them
chain by chain (`docs/adr/0100`, with its 2026-09-29 addendum for the follow-up) found
three costs that had nothing to do with scoring. Every NORMS, LIVEDOCS and DOCMAP lookup
is one `ReadBuffer`, and the LIVEDOCS reads against segments with no tombstone at all
were the largest share, so the ranked readers now check a segment's bitmap once at
reader init and answer liveness from that. This is safe for a reason worth holding onto:
bits only go from set to clear after seal, so a document tombstoned after the check is
scored as live, which is exactly what already happens to any dead tuple VACUUM has not
reached, and the executor's visibility check drops it the same way; and the line
pointer can be reused only after the tombstone, which is after the scan's snapshot. It
follows that a reader which must see a later tombstone (VACUUM's bulkdelete, the merge
replay) cannot use the check, and that the catalog's `live_ndocs` is not consulted at
all, because it can overstate the live set. Second, key projection: rows arrive in score
order, so docids jump backward inside a segment and a forward-only KEYMAP cursor
re-walked its chain from the root each time; the finalizers now sort a multi-page
segment's rows by docid and resolve them in one pass. Third, the df pass looked each
term up in every segment's dictionary and the scorers then did the same lookups again,
so the df pass now hands its results on. All three keep the answers byte-identical; the
measured savings are in the ADR.

What was left after those was one NORMS access per field posting and one DOCMAP access per
candidate, each a `ReadBuffer` plus a content lock even when the cursor was already on the
target page, which for a frequent term it nearly always is (#267). The fix is a cost
trade, and its terms are worth having in mind. A cursor that opts in keeps one `BLCKSZ`
copy of the page its last lookup landed on; a lookup on that block is served from the copy
with no buffer access, and a lookup on another page reads and validates it and replaces the
copy. So the price is an 8 KB allocation per opted-in cursor plus a page copy each time the
cursor changes page, and the saving is one buffer access per same-page lookup. The saving
is therefore largest where lookups cluster on a page, the frequent-term and exhaustive
cases, and smallest where they do not: in the interleaved A/B (192 cells) buffers fell in
every cell, by 27.3% to 99.9%, and the smallest fall was the rare term `w4000`. It is a copy
and not the pin ADR 0100 first sketched because a copy needs no resource-owner cleanup (the
buffer is still released before every lookup returns) and is ordinary memory freed with its
context on error; the pin had been deferred for exactly that cleanup, at about twenty call
sites. The copy is the page only because NORMS, DOCMAP and KEYMAP never change after the
segment is published, which is why LIVEDOCS, whose bits `bm25_livedocs_clear` flips in
place, is never imaged. The copies are bounded per owner and not charged to
`max_match_memory`. What is deliberately left: a key is still resolved for every ranked row
(#267 item 5), because the score-by-key hash, the query-qualified score accessors and the
over-pull tail rebuild each want every key up front; and the merge replay does not use
images, since nothing was measured. After this the
open-time sweep is no longer a rounding error (15-36% of the buffer accesses of an
ordinary query's build at `LIMIT` 10), which ADR 0096's addendum records without reopening its decision.
ARCHITECTURE.md has the enumerated rules.

## 5. Reuse safety and reclamation: the heart of the design

This is where the Generic-WAL-only constraint bites hardest, and where Phase 4's
remaining work lives.

**Reuse safety — option (d), not a reuse-conflict record.** When a page is
reclaimed and handed to a new segment, a concurrent scan (or a Hot Standby query)
might still hold a stale pointer to it. nbtree/GiST solve this with a custom-rmgr
reuse-conflict WAL record that actively cancels conflicting standby queries — but
that record is **unbuildable** under Generic-WAL-only (§2). The chosen
alternative (option d): every segment page carries a monotonic **`seg_gen`**
(drawn from `meta->next_gen`), and the reader validates `page.seg_gen ==
expected_gen` after locking each followed page. A reclaimed-then-reused page
always carries a strictly greater gen (gens never repeat → no ABA), so a stale
pointer is *always* detected and the scan aborts cleanly with a `40001` instead
of reading recycled bytes. On a primary the ranked path retries that abort internally
(bounded, with a fresh snapshot, inside an internal subtransaction); on a hot standby, and on
the flat `@@@` membership path everywhere, there is no retry and the client gets the `40001`. The
standby gets no retry because the subtransaction itself was the hazard: core resolves a
recovery conflict with an ERROR only outside a subtransaction and terminates the session
inside one, and the build's interrupt checks made a conflict landing there likely. A
catch-based retry without the subtransaction is not possible either, because core's own
conflict cancel is also `40001` and catching it is exactly what core's FATAL prevents. So a
standby client sees the reuse abort as the same `40001` it must already retry for core's
conflicts, and keeps its session (`docs/adr/0121`). A gen mismatch is not always the race: a
corrupt link into another segment's page looks the same, so the reader checks whether its own
segment is still in the catalog (read under the metapage lock) and raises `XX002` if it is,
since a live segment's pages cannot have been reclaimed (`docs/adr/0120`).
`next_gen` starts at 1 and `seg_gen == 0` is a reserved skip-validation sentinel —
were `next_gen` zero-initialized, the first segment would silently disable its own
validation.

The pending chain needed the same treatment, and until #291 (`docs/adr/0110`) did not have it. Its
pages carried `seg_gen = 0`, so the only thing protecting a scan's walk of the
chain it captured was the drained pages' `retire_xid` horizon — and that horizon is
the primary's. A `hot_standby_feedback=off` standby holds nothing back, so the
primary could seal, recycle and re-init a pending page under a standby scan, which
then followed the new chain and silently returned too few rows. Now every pending
page carries its chain's **epoch** in `seg_gen`, drawn from the same `next_gen`
when the chain starts (in the metapage record that publishes the new head) and
copied onto each page appended later. A scan captures `next_gen` with
`pending_head` and rejects any page whose epoch is at or above it, i.e. any page
re-initialized after its snapshot, as a pending page or a segment page. It is a
bound rather than an exact match because a scan legitimately walks pages appended
to its own chain after the capture. An epoch of 0 (a chain an older binary
started) is passed, so mixed binaries lose the protection rather than raise an
error no retry clears.

So the residual edge — a `hot_standby_feedback=off` standby racing an aggressive
merge, seal or pending recycle — is a rare abort with SQLSTATE `40001`, never
wrong results, reported to the client (the internal retry exists only on the primary, where
the horizon already keeps this race away). One shape
is left: a pending page reused as a catalog or retired-list page, both of which
carry `seg_gen = 0`, still fails the page-kind check as `XX002`. Option (b), the custom rmgr, is kept
only as a documented future fallback.

**Reclamation — XID horizon, not refcount/epoch.** A retired page is freed only
once `GlobalVisCheckRemovableFullXid(heaprel, retire_xid)` passes, where
`retire_xid = ReadNextFullTransactionId()` is captured at swap-commit time. This
is the battle-tested nbtree/GiST/GIN "decouple deletion from recycling" protocol.
The project spec originally proposed an epoch/refcount scheme; the M2a design
rejected it because a refcount makes every scan register/deregister against every
segment it touches on a hot shared structure (contention) and leaks pins forever
on a crash, whereas the XID horizon lets the existing snapshot/xmin machinery hold
the bound for free and re-derive identically on a replica. The horizon check must
be preceded by a forced `GetOldestNonRemovableTransactionId` refresh — the
per-backend horizon is lazily advanced and otherwise reads stale, deferring
reclamation forever.

**The retire mechanism, and the orphan-sweep subtlety.** On a merge the catalog
is swapped by building a fresh orphan catalog chain (survivors + new segment,
minus dropped gens) and flipping the **single `segcat_root` indirection pointer**
in one record — never an in-place rewrite, because a merge drops entries and an
in-place multi-page rewrite would blow the 4-buffer cap. In that same record, each
dropped segment is recorded as **one compact RANGE descriptor** (`page-range` +
the four chain roots + `retire_xid`) on the persistent retired-free list.

Retire does **no per-page work**: it does not stamp `BM25_PAGE_DELETED` on the
range's pages and does not touch their `nextblk`. Two reasons: the N-page range
cannot fit the 4-buffer record (so per-page stamping is impossible), and a
pre-swap scan captured under the old `segcat_root` may still be walking the chain,
so mutating `nextblk` would silently truncate it — and because `seg_gen` is
unchanged, option (d) would *not* catch that.

This is the source of a Phase-4 hazard already guarded in `bm25_fsm.c`. The
orphan sweep skips `BM25_PAGE_DELETED` pages — but since retired pages carry **no
such flag at swap time**, that guard is **necessary but not sufficient**: Phase 4
must additionally teach `bm25_reclaim_orphans` to treat every retired-RANGE page
as reachable, or the sweep will hand a still-referenced page back to the FSM,
bypassing the XID gate. The current sweep also guards the flag read behind
`PageIsNew`, because a never-initialized zero page (crash after relation-extend,
before the page-init WAL commits) would otherwise trip `PageGetSpecialPointer`'s
assertion inside `BM25PageGetOpaque` on a cassert build — a class of bug CI's
non-cassert build cannot catch.

## 6. The milestone arc

- **M0 / M1** (`main`): AM skeleton + Generic WAL + tokenizer + single-segment
  in-place scoring and ranked top-N. Format reached v2. The scoring math
  (`bm25_idf`, `bm25_termscore`) and tokenizer carry into M2a unchanged.
- **M2a** (merged to `main`): the segmented LSM core — pending list, two-phase seal,
  block-compressed segments, Model-A catalog snapshot, multi-segment dedupe scored
  union, tombstone deletes + VACUUM, the tiered merge engine, XID-horizon reclamation,
  and the stamp-and-gate allocator. All complete and CI-green.
- **Format v4 + M3** (merged): the one v3→v4 break to bake a per-index `ts_lexize`
  Snowball analyzer (fingerprinted + gated at scan start). v4 reserved the M4/M5 surface.
- **M5** (merged): multi-field / BM25F / `key_field`, **filling the reserved v4
  field/keymap surface with no further break** — see §7 below.
- **M4** (`impl/m4`): positions → phrase / proximity / snippets, **filling the LAST
  reserved v4 surface (`pos_root`/`pos_post_root`/`BM25_PAGE_POS`) with no break, no
  version bump, and no forced REINDEX for existing bag-of-words use** — see §8 below.
  After M4 the reserved v4 surface is fully consumed; only v3's per-block `max_impact`
  remained reserved, waiting for M2b.
- **M2b** (`impl/m2b`): block-max WAND, the default ranked path, returning results
  **bit-identical** to the exhaustive scorer — see §4a. It fills the last reserved
  slot but, unlike M5/M4, could not do so in place: a safe scan-time bound needs
  per-field raw ingredients, not the reserved `float4`, so M2b broke the format once
  more (v4→**v5**, REINDEX). **The reserve-then-fill arc v3→v4→v5 is now complete —
  the format has no reserved-but-unused field left.**
- **M6** (`impl/m6`): boolean + wildcard **jsonb** query trees — a second RHS type for
  `@@@`/`&@@`, **query-layer only, no format change** (still v5) — see §9 below. Boolean
  *membership* is exact; the score stays the single-pass bag-of-words BM25F OR-sum.
- **`fix/orderby-rank-collapse` (post-M6):** the `ORDER BY x &@@ q, <secondary key>`
  **rank-collapse is FIXED** — a pre-existing whole-index bug (since M1, not M6) where the
  distance operators' constant `+inf` let an Incremental Sort on a secondary key collapse rank
  onto that key. No format change (still v5); §10 below narrates why the bug happened and why
  the fix is correct, ARCHITECTURE Landmines has the mechanism in full.
- **Next:** the production/ops gates (a cassert/valgrind/ASan CI job). Re-sequenced around
  `pg_search` parity in `ROADMAP.md`.

## 7. BM25F and multi-field (M5)

Multi-field ranking is **BM25F**, not a fresh scorer: the M1 pure functions
(`bm25_idf`, `bm25_termscore`) are unchanged — the "field" lives entirely in *which*
stats the caller feeds them. Each indexed column is a dense field; a doc's score is
`Σ_field boost_f · idf(N_field_f, df_field_f) · termscore(tf, doclen_f, avgdl_f, k1_f, b_f)`,
summed inside the ONE existing scorer pass (still dedupe-by-TID, still score-each-doc-once —
the sum-across-fields is *within* one doc's scoring, not a second pass).

Three design forces shaped the on-disk choices:
- **Per-field `df` from the block RLE (not a second dict).** The dictionary `df` is the
  term's total over all fields; a per-field dict would double the dictionary. Instead the
  posting block carries a `(field_id, run)` RLE, and the scorer partitions `dict.df` into
  `df_field` by decoding it — preserving `Σ_field df_field == dict.df` and keeping the dict
  format M3-compatible. A field-scoped query still decodes the whole `dict.df`-bounded run,
  then filters — the RLE interleaves fields, so it cannot shorten the decode.
- **Per-field `N` in the header, gated on `field_count > 1`.** `avgdl_field` needs the count
  of docs that *have* the field (a field can be empty in some docs), which the v4 header did
  not reserve. M5 appends `ndocs_by_field[]` after the existing `total_len_by_field[]` — but
  ONLY for `field_count > 1`, so a single-field segment is byte-for-byte the M3 layout and
  BM25F collapses to the exact M3 formula (`N_field = ndocs`, `boost = 1`). "Fill the reserved
  surface without a break" therefore extends to a spec gap the reservation missed, without
  breaking existing single-field indexes.
- **`key_field` is an INCLUDE column, and scoping is index-scan-only.** A non-text key
  (int/uuid) has no bm25 opclass, so it can't be an indexed attribute; INCLUDE is the
  idiomatic way to carry its value to the build callback. The docid→key map is an *output*
  layer (the ranking dynahash still keys on TID — a non-unique key must not collide the
  accumulator). And `field:term` scoping, like the non-english analyzer before it, is a
  property of the index SCAN: a bare `@@@` filter the planner answers via `bm25_match`
  (no index Relation) can't honor it, so it refuses a scoped query rather than searching
  the field name as a term (#298, `docs/adr/0004`'s addendum) — and a bare query there sees only the LHS column, not
  every field. Since #306 both paths recognize a scope by the same rule, and the index path,
  which knows the field names, reads an unknown name before `/` or a digit as literal text
  (URLs, times), while `bm25_match`, which cannot tell a known name from an unknown one, keeps
  refusing every scope-shaped RHS. The contract between them is "refuse or agree, never
  silently differ" (`docs/adr/0122`). The ranked form, a `@@@` predicate plus
  `ORDER BY … &@@`, is the reliable entry. `ORDER BY … &@@` alone does not force the index:
  with no `@@@` predicate there is no index path, and every row's distance is `+inf`. For a
  jsonb query that fall-through still parses the tree without resolving fields, so a
  malformed tree errors there just as it does on the index (#245).

## 8. Positions, phrase, and snippets (M4)

M4 fills the last reserved v4 surface. The position math is unremarkable — a token's
position is its 0-based ordinal within a field, deltas are varbyte-encoded, and a
phrase/proximity test is arithmetic over those ordinals (exact = consecutive positions;
ordered PRE/n = strictly increasing with `(p_last − p₁) − (N−1) ≤ n`; unordered W/n = some
assignment of one distinct position per phrase *slot* — see Force 2b — whose window span is
`≤ (N−1) + n`, where `n` is
extra slop beyond perfect adjacency, not an absolute span). What is worth explaining is the
*shape* of the feature, driven by three design forces.

**Force 1 — positions are a SEPARATE chain, so bag-of-words never faults them.** The central
invariant is that a ranked scan that does not ask about phrases must never pay for positions.
So positions do NOT live inline in the POST block; they live in their own per-segment
`BM25_PAGE_POS` chain (the Lucene `.doc`/`.pos` split), rooted at `pos_root`, per-term at
`pos_post_root`/`pos_post_off` — the exact structural twin of the docid/tf `post_root`/`post_off`.
The reader reaches positions only through an OPTIONAL lockstep `pos_cb` cursor; a bag-of-words
caller passes `pos_cb = NULL` and the POS pages are never touched. Had positions ridden in the
POST block, every ranked scan would fault them into the buffer cache — the split is what keeps
the common path free. The chain is orphan-built like KEYMAP/DOCMAP, so it costs ZERO buffers in
the 4-buffer publish window (the cap governs only the single publish record, which touches the
metapage + catalog, not the orphan chains). The lockstep read is self-verifying: one
`[tf-count][Δpos × tf]` frame per position-bearing posting, and the frame's decoded `tf-count`
must equal the posting's `tf` from the POST stream — a mismatch means the two cursors have
desynced, and that is a hard ERROR, never a silent misparse. This equality is the entire safety
argument for the parallel walk; it inherits its outer bound from the `df`-bounded POST walk and
needs no second one.

**Force 2 — phrase matching is a recheck-and-filter, not a re-score.** A single posting is one
term; a multi-term phrase cannot be decided at one posting, so a per-posting gate is
structurally impossible. Instead the scorer runs unchanged over the phrase's constituent terms
(each contributes its ordinary BM25F score), stashing each term's per-`(TID, field)` position
list as it goes. AFTER the full scan, the positional test runs over the produced ranked TID set
and drops any TID that fails. This composes cleanly with the single-pass BM25F scorer and — the
load-bearing property — **does not move the M2b-WAND seam or the `bm25_scan_build_ranking`
contract**: the recheck is a pure post-filter on the TID set the scorer already produced.
Scoring is filter-only (survivors keep the score they already accumulated); phrase-frequency
as a synthesized tf is deferred, because it would need a second pass and a synthesized
phrase-idf and would break the single-pass dedupe-by-TID invariant. A bare phrase is inherently
within one field (positions reset per field), so it ORs across fields; `field:"a b"` scopes to
one. And because the recheck reads positions, a phrase against a position-less segment cannot be
answered — rather than silently returning AND-semantics or nothing, it ERRORs by default
(opt-out to a WARNING + AND-of-terms via `phrase_fallback = 'and'`, mirroring the analyzer-gate
precedent), because for legal search a silent false positive or a hidden index-state problem is
worse than a loud failure.

**Force 2b — the unit of a phrase is a source WORD, not a lexeme (#184).** A dictionary that
splits compounds or expands a thesaurus turns one query word into several lexemes. Core FTS
handles that on the *query* side: `phraseto_tsquery` emits alternatives
(`'footballklubber' | 'foot' <-> 'ball' <-> 'klubber' | …`) so a document that co-positions the
compound's lexemes still matches. This engine had no equivalent, which is why co-positioning
*documents* alone — a change that looks obviously right, matches `ts_parse.c`, and passed every
suite — turned phrase queries over compound words into silent zero-row answers, and was reverted
(§ `docs/adr/0077`).

The counterpart is a **slot** model. A slot is one query position: the set of lexemes the
analyzer emitted for one source word, satisfied by ANY of them. `bm25_phrase_slot_map` reads the
grouping straight off `bm25_analyze`'s own token positions, and the recheck merges a slot's
member position lists into the single ascending list the matcher consumes, so the matcher's
interface never learned about groups at all — the merge happens one call earlier. That choice is
what left `ordered_match` untouched: its greedy strictly-increasing chain was *already* the
per-slot invariant, and the span bound it tests now counts words, which is exactly core's notion
of word distance.

The cost landed entirely on `unordered_match`, whose counting reduction rested on an axiom that
slots make false: different phrase terms' position sets are disjoint, because a position ordinal
belongs to one token. Merged slot lists can overlap without being equal, and then counting is
unsound in the *permissive* direction — `S₀={5}, S₁={5,9}` with slop 0 has a window `[5,5]`
holding one entry of each group but no assignment of two DISTINCT positions inside it. So
disjointness became a runtime-checked precondition (an O(m) adjacent-equal scan of the
already-sorted stream) guarding the old sweep, with an exact windowed bipartite-matching sweep —
Hall's theorem, multiplicities folded into slot instances, maintained incrementally as the
window slides — for the overlapping case. One deliberate consequence: OR-per-slot is strictly
*more* permissive than core (which ANDs the lexemes within one variant and ORs the variants),
because `bm25_analyze` discards `TSLexeme.nvariant` and cannot reconstruct the chains — this
never *drops* a match core would find, only adds ones core reaches by a different route (its
variant-chain alternatives).

The mechanism went live with analyzer revision 5 (`docs/adr/0087`, issue #184), the change that
makes `bm25_analyze` finally stamp one position per run instead of per lexeme — until then every
token had its own position, the slot map was the identity, and this whole section was reachable
code with no reachable input. Landing it was staged in the order the 0077 lesson demanded: the
slot model and the doclen half below shipped first and sat inert, each verified
behavior-preserving by construction, so that the matcher could already consume co-positioned
data the moment the third PR started producing it — ARCHITECTURE Landmines has the staging
history in full. A second divergence from core, orthogonal to the
slot model and not fixed by it, ships alongside: under `stopwords = default` a dropped stopword
consumes no position here, where core lets it consume one. Both divergences are named in
`docs/adr/0087` so neither is later rediscovered as a defect of this work.

**Force 2c — a document's LENGTH is its word count, not its lexeme count (#184).** The same
one-word-becomes-several-lexemes divergence has a second, non-positional half: BM25's length
normalization. If a compound word contributes six to the length denominator, every document
containing an ambiguous word is systematically penalised against documents that happen to use
unambiguous ones — a ranking artifact of the dictionary, not of the text. So `doclen` is defined
as **`max(token position) + 1`**: the number of source word runs that emitted at least one token,
zero for an empty field. Computed in exactly one place (`bm25_accum_add_field_tokens`), which
both ingest paths reach, and as a max-scan rather than "the last token's position" because the
drain rebuilds its token array entry-by-entry and hands it over in dictionary order.

Expressing it as a function of the positions rather than as a token count is what makes it a
*definition* instead of a second thing to keep in sync: while the analyzer emitted one token per
run the two were the same number — so nothing moved when this definition landed on its own — and
once the analyzer stopped doing that for multi-lexeme runs (analyzer revision 5, `docs/adr/0087`,
issue #184), doclen followed automatically, on every path at once, with no further edit.

Every path except one. The pending list scores unsealed documents from its own records, and
those records carried per-(field, term) `tf` but no length, so the readers RECONSTRUCTED
per-field doclen as the sum of `tf` over a field's entries — the token count. That identity is
the thing the flip breaks, and it breaks it asymmetrically: sealed documents would be scored on
runs while pending ones were scored on lexemes, so a `bm25_seal()` would silently re-rank the
corpus, and only for documents containing compound words. Format **v8** therefore stores the
per-field doclen in the pending record itself. The array is announced by a flag in the record's
own header rather than inferred from the index's stamped `format_version`, so a pending list may
hold both shapes at once and every walker still strides it correctly; the floor a v8 record
demands is raised lazily, in the same WAL record as the first such write, exactly as v7's
spanning records do. `sql/16_pending_ryw` is the guard: capture every score and every per-field
length statistic, seal, capture again, and require that nothing moved.

The same redefinition has a third consequence, downstream of scoring rather than of it.
`bm25_accum_estimate_bytes` predicts how much accumulator residency replaying a candidate
segment will cost, so the merge selection trim (§3a) can refuse a set that will not fit
`maintenance_work_mem`. Its dominant term charges two token-scaled quantities — a 32-byte
`AccumPosting` per posting, 4 bytes per stored position, one position per token — from
`total_len`. Runs and tokens were the same number until this section redefined `total_len`
as a run count, so the estimator quietly started under-charging by a compound dictionary's
lexemes-per-run ratio (about 5x for `ispell_sample`; a Snowball-only index is unaffected,
since English emits one lexeme per run). Nothing in the estimator's own code changed — the
input feeding it did, which is exactly the failure mode this whole section exists to avoid
for scoring, now showing up one layer over. The existing layering keeps it from being
urgent: `BM25_ACCUM_SLACK_FACTOR` absorbs the first 2x, `bm25_accum_over_budget` measures
actual residency rather than trusting the estimate, and the merge executor's progress check
bounds the force loop regardless of estimate quality. The visible cost was one no-op index
rewrite per autovacuum for a compound-dictionary index sitting at the merge budget floor —
bounded enough that `docs/adr/0087` recorded it as follow-up rather than a blocker, needing
a per-segment token count the catalog did not yet carry.

`docs/adr/0088` supplies that count, in two stages that were deliberately kept apart. The segment
catalog entry and the segment header each already carried a 4-byte padding hole (the #144
hazard `sql/96_wal_page_determinism` guards against); naming it `total_tokens` costs no
format bump — an old reader keeps reading the same 40/64 bytes and ignoring four of them.
Trust is the harder half: an entry decoded off an index whose catalog was ever written by
a pre-ADR-0074 binary can carry stack residue in exactly those bytes, so the field is
honored only when `BM25_FEAT_SEGCAT_TOKENS` is set, and that bit is a promise about the
*writer* — every catalog entry this index will ever hold came from a counter-aware binary
— which only a fresh build can honestly make. `bm25_upgrade` cannot confer it: its
transform registry is empty, so an accepted version gap is an in-place restamp with no
segment rewrite, and stamping the bit there would bless residue as data. This is also why
the fix arrives in two units rather than one — the unit that lands the field leaves
`bm25_accum_estimate_bytes` UNCHANGED, still reading only `total_len`, so nothing about
the estimate moves until a second change makes it consult `Max(stored_tokens,
total_len)`. That `Max`, not an
unconditional switch to the new field, is what the error's direction demands: an
over-estimate merely refuses a merge that would have fit, which is recoverable at the next
vacuum, while an under-estimate reproduces the very rewrite loop this work exists to
remove. Charging *tokens* rather than *postings* follows the same logic one level down —
tokens over-estimate the postings term (a repeated term within one document folds into one
posting at a higher `tf`, so tokens ≥ postings) while landing exactly on the positions term,
whereas postings would be exact on the larger term and under-count the smaller one, trading
the cheap error for the expensive one. An index gains the corrected estimate only at
REINDEX, which is not a new cost this change introduces: analyzer revision 5 already forces
REINDEX on exactly the compound dictionaries the estimate was wrong for.

**Force 3 — snippets re-analyze the passed text, they do not consume the stored ordinals.** The
stored positions are post-stemming ordinals with no retained mapping back to original character
spans, so they cannot drive a highlighter that must wrap the ORIGINAL surface text. `bm25_snippet`
therefore takes the column *value* and re-runs the analyzer over it at projection time, marking
tokens whose stem is in the query stem set (since #308 built from every non-negated leaf of any
query tree, with wildcard patterns globbed against the field's own tokens: the field is analyzed
by the index's analyzer, so a match there is membership in the scan's expansion, and the ranking
builder did not have to change to export it, `docs/adr/0123`; each token now carries its `src_off`/`src_len` byte
span in the source, so a matched run is wrapped with original casing and on UTF-8 char
boundaries). The position chain and the snippet path are thus fully independent consumers of the
same query — one reads on-disk ordinals to test adjacency, the other re-derives spans from live
text to highlight. Like every other scan-resident accessor (`bm25_score`, `field:term`), the
snippet is bound to the active index scan; a bare `@@@` filter answered by `bm25_match` cannot
produce one.

## 9. Boolean and wildcard query trees (M6)

M6 adds a *second RHS type* for the `@@@`/`&@@` operators — a **jsonb query object** built by
SQL functions (`bm25_term`, `bm25_match_terms`, `bm25_phrase`, `bm25_wildcard`, `bm25_boolean`,
`bm25_boost`) — with no on-disk change: the whole feature lives in the query layer. Four design
forces shaped it.

**Why jsonb objects, not a query string.** Applications assemble queries from user input, so a
text query *language* would put untrusted text on a parse path — the exact shape of an
injection bug. A jsonb object sidesteps it structurally: the builder functions carry user terms
as bound *values* inside a jsonb document, and the C side reads those values as data — it never
parses a grammar out of them, so there is no DSL to inject into. (A `bm25_parse(text) → jsonb`
front-end is a deferred stretch goal; the builders are the v1 surface.) The `(text,text)`
mini-parser (`field:term`, `"phrase"~n`) is untouched and byte-identical; jsonb is purely
additive — a second operator per opclass strategy — and a single MATCH/TERM jsonb leaf is
copied into `so->qterm` and runs the text path verbatim, so the common case pays nothing.

**Why a presence bitmask that reuses `and_presence`.** Boolean matching (must/should/must_not)
is a per-doc predicate, but the scorer is a single OR-sum pass that visits one `(term, doc)`
posting at a time and cannot see a whole doc's clause structure mid-pass. The resolution is to
score exactly as before while tagging, per doc, *which leaf clauses were present* — one bit per
leaf — and evaluate the boolean formula once at drain over that bitmask. The machinery already
existed: M4's phrase-AND path keeps a `PhraseAndEnt.mask` in an `and_presence` HTAB. M6 reuses
it verbatim (`phrase_and_mark(cur_qi = leaf_bit)`) rather than adding a `BM25AccEnt.mask` field, so
there is one bitmask mechanism, not two that could desync. Flatten assigns every leaf a bit from
a single counter and caps the total at 64 so the mask is a plain `uint64`; the cap lives in
flatten because flatten is the one place that both assigns the bit and is guaranteed to run
before any `1<<leaf_bit`. Since issue #68 (`docs/adr/0099`) the same count is ALSO taken
during parse, in `parse_leaf`, before a wide-but-shallow tree's leaves are even fully
allocated — "the cap exists" and "the cap runs before the work it bounds" are different
claims, and a tree with thousands of top-level `should` leaves used to pay for every one of
them before flatten's check ever ran. Parse now rejects at the 65th leaf, which makes
flatten's own check currently unreachable through any caller; it stays as a guard for a
future one that flattens a tree this module didn't parse.

**Why the `@@@` filter reuses the scorer.** The non-scoring boolean filter (`WHERE col @@@
<jsonb>`) and the ranked `ORDER BY col &@@ <jsonb>` must agree on *which* rows match — a
`must_not` query especially, where a naive flat-OR filter would return the excluded docs. The
cheap way to guarantee agreement is to not write a second evaluator at all: the filter path
drives the *same* `bm25_scan_build_ranking` builder, runs the identical per-leaf presence +
`bm25_query_eval` drain, and simply ignores the scores. Filter set == ranked set *by
construction* (D12), for boolean, must_not, wildcard, and phrase alike. The cost — the filter
computes and discards scores — is the accepted price of one code path.

**Why wildcards bypass the stemmer, and why scoring stays bag-of-words.** A `bm25_wildcard`
leaf is *expanded* against the dictionary, not analyzed: the dict stores *stemmed* terms, and
running `judg*` through the stemmer would produce a nonsense stem, so the pattern is matched
raw-lowercased against the sorted dict bytes (prefix range-scan + `*`-glob), per segment and
pending, deduped into one distinct set — a term in several segments must score once under the
leaf's single bit, or it double-counts. Guardrails (`wildcard_min_prefix`,
`wildcard_max_expansions`) bound the expansion; the dict iterator has no seek, so the prefix
scan is a linear skip per segment — acceptable at M6 scale, and the natural upgrade point if
wildcards ever get hot. The deeper honesty is that M6 keeps the single-pass scorer: boolean
*membership* is exact (the drain-time formula is), but the *score* is still the OR-sum of every
positive leaf's constituent terms. A should-phrase or should-wildcard whose own predicate fails
still leaves its terms' contribution in a kept doc's score — the accumulator cannot retract a
leaf after the fact without a second pass. This reconciles the design's D11 "Σ over matched
leaf contributions" wording with what the code does: membership is matched-leaf-exact, ranking
is bag-of-words. Phrase-frequency-as-tf and score-accurate boolean are the same deferred second
pass M4 already declined (§8) — a continuation of the single-pass invariant, not a regression.

The enumerated M6 invariants — leaf-cap → uint64 mask, presence via `and_presence`, filter ==
ranked by construction, jsonb-multi-leaf-bypasses-WAND, wildcard single-count dedup, wildcards
bypass the stemmer — live in ARCHITECTURE.md; the reasons each holds are the four forces above.

## 10. Known approximations, gaps, and unrecovered rationale

Honest boundaries of the current design and of this document. Items marked
*"Rationale not recovered"* are flagged rather than invented.

- **Σdoclen on delete is a per-segment-average approximation.** `bm25_livedocs_clear`
  decrements `total_len` by the segment-average doclen, not the deleted doc's
  actual length; it is exact only at merge. The drift in IDF/avgdl between merges
  is small, bounded, and self-heals at merge — acceptable for ranking.
  > Rationale not recovered: a `NORMS` chain storing per-doc length exists
  > (`bm25_seg_doclen`), so *why* the delete path uses the segment average instead
  > of a `NORMS` lookup is not explained in the sources — possibly a latent
  > inconsistency worth checking before Phase 4 makes stats exact at merge.
- **Cross-partition global IDF is unsolved by design.** Each partition has
  partition-local IDF (ParadeDB issue #3284). A shared cross-index corpus-stats
  mechanism is the one place a preloaded shared-memory structure would clearly
  pay; explicitly deferred as a post-v1 differentiator.
- **Ranked ordering across partitions is per-child correct, not globally comparable.**
  A ranked `ORDER BY &@@` on a partitioned or inheritance parent is a Merge Append over
  one index scan per child, all ranking the same query, so the operator's RHS cannot
  say which child a projected row belongs to. Distance resolution breaks the tie by the
  scan that emitted most recently, which is right whenever the projection runs
  right after its own scan's emit (`docs/adr/0104`; the residual shapes are enumerated
  in ARCHITECTURE.md's score-accessor section). Before that, rows came out of score
  order and `LIMIT k` returned the wrong top-k. Even with the order right, scores are
  only as comparable across children as their partition-local IDF allows, per the
  previous item.
  Emit recency was chosen as the minimal change inside the existing bounded contract;
  a call-site binding (as `bm25_score` uses) is a larger change that would inherit
  ADR 0103's attribution edge cases.
- **Inline-seal starvation / pending growth.** A non-blocking conditional-lock
  inline seal can chronically lose the race and let the pending list grow; the
  threshold sizing plus VACUUM/manual seal are the guaranteed drain. The
  persistent retired-free list must likewise be bounded/compacted (the
  catalog-walk reclamation is that backstop).
- **`bm25_native.seal_threshold` default is 4096 KB.**
  > Rationale not recovered: the tradeoff reasoning behind 4096 KB (pending size
  > vs seal frequency) is not recorded in the sources.
- **`BM25_POSTINGS_PER_BLOCK = 128`.**
  > Rationale not recovered: no justification for 128 over 64/256 appears in the
  > plan or specs.
- **Single-threaded build.** Parallel `ambuild` is deferred; large initial builds
  are single-threaded in v1.
- **Benchmarks are report-only.** M2b added `bench/wand_vs_exhaustive.sh` (ranked top-N
  latency, WAND vs exhaustive, recorded to `bench/baselines/` from local runs); CI runs the
  bench scripts scaled down in a `bench` job that never gates (`docs/adr/0098`) — only the
  correctness legs (build, regression, TAP, hardening, folding-collation, ASan) gate.
- **`amcostestimate` is an M1 placeholder.** Whether/how it is refined for
  multi-segment cost in M2a is not addressed in the harvested sources.
- **The over-pull tail reads a second snapshot but keeps the first build's statistics
  (M2b; #268).** A ranked scan that pulls *past* `bm25_native.wand_top_k` (an un-`LIMIT`ed
  or `LIMIT > wand_top_k` scan, or a `LIMIT` under a filter qual that rejects most of the
  top k — so the default `wand_top_k` reaches it too) rebuilds the full exhaustive ranking
  under a *fresh* snapshot. The corpus statistics that snapshot yields count every valid
  pending entry with no visibility check, so any write between the builds — committed or
  not, aborted, or the scanning transaction's own — and any VACUUM or merge used to move
  them, and the tail was re-scored on a different scale: distances out of order, rows
  repeated or skipped (reproduced). The rebuild now scores under the capped build's
  statistics (avgdl, k1/b/boost and each query term's idf), so a document both builds saw
  keeps its score exactly, and resumes at the first entry after the last emitted (score,
  TID). The rule is that a rebuild may only extend the emitted prefix. Reusing the first
  snapshot outright was the alternative; it would need the segment pages behind that
  snapshot held against reuse for the life of the cursor, while pinning the statistics
  needs nothing held.
- **Maintenance holds block inserts, and the direction of the cancel matters
  (`docs/adr/0022`, `0066`, `0102`, `0116`, `0117`, `0118`; 0022 and 0102 have 2026-09-29 addenda).** Every seal, merge,
  upgrade rewrite and reclaim takes the seal/merge singleton in ExclusiveLock mode, and an
  insert that adds a document takes it in ShareLock mode, which is what makes a seal and an
  append genuinely exclude each other (before that, an append landing during a seal was
  silently lost). The price is that an insert waits for each hold of an explicit
  `bm25_seal`, `bm25_merge` or `bm25_upgrade` -- a seal, one merge pass (a forced merge
  releases between passes), or one retired descriptor page of `bm25_reclaim_retired` --
  and for VACUUM's opportunistic merge pass, which cannot be split. It does not wait for
  VACUUM's orphan sweep, which holds the lock in ShareLock and runs only on evidence that
  orphans can exist (issue #300): the sweep tolerates concurrent appenders because every
  page an appender can init while it runs carries a chain epoch it can recognise. VACUUM's index-vacuuming pass holds the same lock in
  ShareLock mode for its whole length, so an explicit maintenance call waits for it and
  inserts queue behind that waiter; that pass runs only when the heap scan found dead items
  and can be skipped or bypassed, so it is not every VACUUM. VACUUM's end-of-run cleanup
  waits for the lock too, except for its opportunistic merge. The two opportunistic paths
  that skip rather than wait are the insert-triggered seal and that merge. An autovacuum is
  cancelled after `deadlock_timeout` only when it holds the lock and something waits on it,
  never while it is the waiter, and never when it is an anti-wraparound run. The README's
  maintenance section is the operator's version of this.
- **Score accessors are bounded, and the exact form is an overload (#242, #253;
  `docs/adr/0103`, `0105`).** `bm25_score(tid)` and `bm25_score_key(key)` receive only row
  identity and must infer which concurrent scan owns the row. Every tiebreak tried against
  that (NULL on ambiguity, alone and with an exhausted-scan flag, and fresh-bound-only) had a counterexample
  that made some other shape worse, so ADR 0103 states a bounded contract with two named
  residuals instead. The overloads `bm25_score(tid, query [, regclass])` and
  `bm25_score_key(key, query)` do not infer: the caller names the query and, optionally,
  the heap, and each scan ranking that query is asked whether its whole ranking holds the
  row. Two scans that hold it with different scores give NULL. That rule was rejected for
  the one-argument accessors because it turned correct answers into NULLs; the overloads
  have no earlier answers to degrade. Emit recency, which resolves the `&@@` distance
  among same-query siblings (ADR 0104), is deliberately not used, since the overloads exist
  for the decoupled shapes where the newest emitter is not the owner. A planner-support
  binding was found unreachable (a support function sees the expression, not the chosen
  plan) and an executor hook was not pursued. The advice that goes with them is to compute
  the score next to the scan, or use `-(col &@@ q)`, and reserve the overloads for scores
  that must be computed away from it. Limits: a rescan re-ranks, so an answer can differ
  from the value the scan emitted; the over-pull tail rebuild above reads a fresh snapshot
  but scores under the first build's statistics, so a row both builds saw keeps its score.
- **`&@@` with no `@@@` returns `+inf`, on purpose, and validates a jsonb tree (#151,
  #245; `docs/adr/0061`'s addendum).** `&@@` is projected as a resjunk column on every
  ranked query, so it cannot raise the way the `@@@` filter does off-index; both
  fall-through cases (no scan registered, and scans that rank a different query) are
  pinned by suites as decisions. Issue #245 added one narrowing: on that fall-through the
  jsonb form runs the same parse and flatten the index path runs, without resolving field
  names, so a malformed tree errors there as it does on the index. A NOTICE was rejected
  because it would fire on the deliberate cases, and a planner check was not done. Still
  open: a `must_not` phrase leaf is accepted off-index (#273, closed as a residual). The
  validation cache keys on the tree's bytes and on the three SUSET wildcard GUCs that also
  decide validity, since a cached valid verdict would otherwise survive a `set_config` that
  tightens one between rows of a statement (#272, fixed). The text form and a valid jsonb
  tree still return `+inf` in arbitrary order with no signal; that was decided as
  documented and closed (#271).
- **Boolean scoring is bag-of-words (M6).** Boolean *membership* is exact, but a doc's *score*
  is the single-pass BM25F OR-sum of every positive leaf's constituent terms — a should-phrase
  or should-wildcard whose own adjacency/expansion predicate fails still contributes its terms'
  score to a doc a sibling clause keeps. Only the ranking of already-matched docs is
  approximate; membership is exact. By design (the M4 filter-only phrase-scoring contract, §8),
  reconciling the design's D11 "Σ over matched leaf contributions" wording with the
  implementation. Score-accurate boolean (and phrase-frequency-as-tf) needs a second scorer
  pass and is deferred.
- **`must_not`-PHRASE leaves are rejected (M6).** A phrase leaf inside `must_not` raises a
  clean ERROR rather than mis-scoring: the negated presence-only decode can't reuse the
  pending-wins dedup (the position stash is append/non-idempotent). A closable limitation that
  needs its own dedup wiring; deferred, never silent-wrong.
- **`ORDER BY x &@@ q, <secondary key>` collapsed BM25 rank onto the secondary key — FIXED
  (post-M6).** A pre-existing whole-index bug (since M1), identical for text and jsonb, NOT
  M6-specific. Kept here, marked resolved, because the *shape* of the bug — and of the fix — is
  worth understanding even though the symptom is gone; the mechanism in full is in ARCHITECTURE
  Landmines.

  *Why the naive `+inf` collapsed the rank.* The intuition when `bm25_distance`/
  `bm25_distance_jsonb` were written was that `&@@` only matters when it drives the ORDER BY
  itself — surely the executor asks for the ordering value once, off the index, and never
  bothers evaluating the SQL function again once the index has already supplied the order? That
  intuition is wrong, and the planner's actual behavior explains why the bug was invisible for
  so long. `&@@` is not just consulted internally by the ordered index scan; it is *also* placed
  as an ordinary resjunk expression on the Index Scan node's own target list whenever the plan
  needs the value materialized as data — which happens the moment there is a second thing to
  sort by, or the column is selected directly. That per-row projection runs on *every* ranked
  query, unconditionally, independent of `xs_recheckorderby` (a different flag, consulted only
  internally by `IndexNextWithReorder` to decide whether to re-verify an ordering value the
  executor already trusts — it says nothing about whether the SQL-level operator function itself
  gets called as a plain expression). With a single ORDER BY key the index alone supplies the
  true order and the projected `+inf` is simply dead data, discarded downstream — the bug was
  silent because the common case never surfaces it. Add a *second* sort key, though, and the
  planner must interpose an Incremental Sort above the Index Scan to break ties within each
  group the index already ordered; that sort compares exactly the tuple **the resjunk column
  projected** — `(+inf, secondary)` for every row — so every row ties on `+inf` and the sort
  falls through entirely to the secondary key. No error, no warning: the query returns a
  plausible-looking result set, just in the wrong order, which is why this class of bug survived
  from M1 through M6 undetected.

  *Why the stash is correct.* The fix does not try to make the operator function compute BM25
  from scratch (it has no access to the scan's live corpus stats, and re-deriving the score from
  its bare arguments would mean re-plumbing per-field idf/avgdl/boost through a function that
  only ever sees `(text, jsonb)` or `(text, text)`). Instead it exploits a value that already
  exists: `bm25_gettuple` computes the real distance (`-score`) for every tuple it returns, purely
  so it can hand that value to the executor as the genuine ORDER BY key via
  `xs_orderbyvals[0]`. The fix stashes that *same* value on the scan opaque
  (`so->cur_orderby_dist`) at the moment it is computed, and has the operator function return the
  stash instead of `+inf`. This is correct exactly when the resjunk projection that reads the
  stash is projecting the *same* tuple `bm25_gettuple` most recently produced — a strict, in-order
  1:1 `gettuple → project` pairing with no tuple buffered and re-projected later against a since-
  moved-on stash. That pairing holds for a plain Index Scan feeding Limit/Sort/IncrementalSort:
  each child node pulls one tuple at a time and projects it immediately, so the stash is always
  fresh for the tuple being read. It keeps holding across the AM's less-obvious scan shapes for
  the same underlying reason — every one of them still funnels through exactly one
  `gettuple`-then-immediate-project step per returned tuple: multi-segment-plus-pending union
  (each returned tuple, sealed or pending, stashes its own score as it is drained), block-max
  WAND top-k (each top-k entry stashes the score it was ranked under), the over-pull tail's second
  exhaustive rebuild (a distinct scan-catalog snapshot per the separate over-pull-tail limitation
  above, but still one stash per one `gettuple` call), and a rescanned/bound-parameter plan (a
  fresh ranking, fresh stashes, no leftover state from the prior run). The one shape that *would*
  break the pairing — a mark, then more pulling, then a restore that re-projects an
  already-returned tuple against a now-stale stash — cannot happen here at all: the AM declares no
  `ammarkpos`/`amrestrpos` (both are NULL), so the planner can never place a mark/restore consumer
  (a mergejoin or nestloop inner) directly against this scan. It is forced to interpose a
  buffering node (Sort or Materialize) instead, and that node's own mark/restore replays tuples it
  already projected and froze at buffer time — it never re-invokes the bm25 scan or the operator
  function. The invariant holds by construction of how the planner is compelled to use this AM,
  not by any check inside the AM itself.
- **A scan returns the SQL answer; `ORDER BY &@@` only orders (#290; `docs/adr/0109`).**
  The index used to build its whole query from the `&@@` key and never read the `@@@` key,
  so `WHERE body @@@ 'cat' ORDER BY body &@@ 'dog'` returned the 'dog' rows through the
  index and the 'cat' rows through a seqscan: the same statement changed its answer with the
  plan. `xs_recheck = false` and the planner's removal of the clause from the qual list mean a
  key the AM does not apply is applied by nothing, so the AM now applies every `@@@` key and
  intersects them. The `&@@` query then ranks, and the WHERE rows it does not match come last
  at distance `+Infinity`, because an ORDER BY never removes a row. "Not matched" is defined
  by the ORDER BY query's *membership* set computed by the exhaustive scorer, not by "absent
  from the ranking", because a WAND ranking is capped; so a scan with a differing WHERE key
  never uses WAND, and the cost is a full scoring pass per distinct query. Rejected: keeping
  ORDER-BY-wins as documented (ADR 0021's position), `xs_recheck = true` (it defers to
  `bm25_match`'s default analyzer), and plain intersection (drops the unmatched rows). The
  user decided to take the semantics and the cost; the field-scope idiom therefore moved into
  the WHERE.
- **A chain walk ends where its header says, or it is corruption (#293, #294;
  `docs/adr/0111`).** Every fallback that answered for a chain shorter than its header said
  (live, an invalid TID, `doclen` 0, a shorter posting list) only ever served a corrupt
  index, and a merge then wrote the damage into a healthy-looking segment. The contract is
  an expected length or count, the page kind, the generation or epoch, the extent and the
  full span of each non-final page, with an error and no repair, because a repair has to guess
  which bytes were meant. The TID bound is `MaxOffsetNumber` and not `MaxHeapTuplesPerPage`
  because bm25 does not restrict the table access method. Left open and owned by #303: POST
  walks have no visit cap, and single-field WAND has no cross-block order check.
- **A block bound owes domination per document, so blocks end at document boundaries
  (#289; `docs/adr/0113`).** A per-block bound dominated only the postings its block held, and
  a multi-field document whose postings straddled a block cut was under-bounded by both
  blocks. The reader repair has to stay forever, since nothing on disk marks an old
  straddling segment: the global bound sums per-field maxima, and the deep check scores the
  continuation exactly. Adding the next block's bound instead scored up to 24% more documents
  in the worst measured cell, the exact continuation at most 4.3%. The writer repair backs off
  to a document boundary (97 to 128 postings), which needs no format change; letting a block
  exceed 128 would have been one.
- **`key_field` is stamped at build and checked at every INSERT (#292; `docs/adr/0112`).**
  `key_field` is an ordinary reloption but fixes the meaning of every stored key, so changing
  it was accepted and then corrupted or wedged the index, worst on the empty catalog of an index
  created before it is loaded. The build writes `(key_type, key_size, key_attno)` on the
  field-config page as an additive tail. A DDL-time refusal cannot work, because `amoptions`
  sees neither the old value nor the command, and an `object_access_hook` was judged fiddly and
  not taken. An index built before the stamp keeps the weaker segment-0 check until REINDEX.
- **Run splitting in single-byte databases comes from a table, and the fingerprint watches
  what the dictionary does (#295, #296; `docs/adr/0114`).** The locale cannot decide
  word bytes (it is not recorded), a conversion through `pg_conversion` can be redirected by
  the session, and the server's Unicode tables move with the major version, so the
  classification is a generated per-encoding table pinned to one PostgreSQL source and one
  Unicode version. The fingerprint's seventh component hashes the dictionary's actual output
  over a fixed word list, because a name-derived identity cannot see a stemmer that changed
  under the same name. Both ride one revision bump so users pay one REINDEX. The probe sees
  only its own words, and WIN1258 and WIN874 combining marks that are not alphabetic remain
  separators.
- **A scan the caller may not read is invisible, not answered with NULL (#301;
  `docs/adr/0115`).** The accessors are executable by PUBLIC and the registry is per backend,
  so a SECURITY DEFINER function's open scan was readable by its caller. Ownership by user id
  closes the SQL-function shape; a definer-opened refcursor registers under the caller's id
  because an index scan begins at its first fetch, so the privilege and RLS checks are needed
  as well. The first version answered NULL for a refused scan, and NULL versus a score then
  reported whether the refused ranking held a row, so refusal had to remove the scan from every
  walk. `&@@` is exempt from the privilege check because Merge Append orders on it.
- **The orphan sweep is evidence-gated and runs in share mode; the lock was not split (#300;
  `docs/adr/0116`, `0117`, `0118`).** The sweep cost O(index) under an Exclusive hold on every
  VACUUM, which cancelled autovacuum on insert-busy tables. It now runs only on a metapage
  bracket gap, a changed crash epoch, or an index this release has not yet swept, and in ShareLock with a per-page epoch rule so it never
  stamps a page an appender owns. A merge swap leaves its bracket open on purpose, since the
  swap orphans the old catalog chain. A design that split the singleton into a structure lock
  and a pending-anchor lock, which would have taken merges and reclaims off the insert path,
  was recommended by the design pass and not taken; the holds are bounded by chunking and
  releasing under one lock instead.
  The lock split was rejected as the larger change: once the gate and the share-mode sweep
  had taken the O(index) sweep off the insert path, bounding the remaining holds by chunking
  kept the lock protocol (ADRs 0019, 0066, 0102, 0107) intact, at the cost of the
  opportunistic merge pass remaining one hold (ADR 0118).
  What remains blocking is the seal (O(pending)), VACUUM's single opportunistic merge pass, a
  reclaim chunk and a forced merge pass.
- **Sealed chains are read through one walker, and each chain family ends its own cycles
  (#303; `docs/adr/0120`).** This was the third issue in the chain-walk family, and the
  pattern was the same each time: a dozen walkers, each with its own subset of checks, drifting
  apart. So the segment side got the twin of the pending walker, and the per-page checks moved
  into it. Per-site visit caps, the issue's proposal, were rejected because a cap ends a spin
  only after up to `nblocks` reads and does nothing for a wrong-page answer; a visited set was
  rejected because it puts a hash probe on the per-posting path. What ends a cycle early is an
  order rule the decode already has the values for (DICT term order, POST `(docid, field)`
  order, KEYMAP full span), plus a revisit test against the first, previous and root page,
  which costs three compares. The gen arm reads the catalog under the metapage lock because an
  independent design check found that the first draft's unlocked lookup could turn the
  standby's legitimate `40001` into a false corruption error. The walker cost a frequent-term
  WAND top-k about 6% until the single-page image hit was let past it. A link that skips forward
  or back to a page that is neither first, previous nor root is still answered from the wrong
  page; no cheap invariant exists for it.
- **The accumulator keys on the whole term (#305; `docs/adr/0125`, superseding `0076`).** ADR
  0076 kept a fixed-width key with a 32-bit hash for long terms on the premise that nobody can
  aim a 32-bit collision; a birthday search does it in about a second, which made the linear
  fallback reachable on demand inside a seal that blocks inserts. The pointer-and-length key
  0076 itself named as the right destination removed the fallback and shrank every entry from
  260 bytes to 24, which cut build, seal and merge time by a quarter to a third.
- **A jsonb query is validated as written (#304, #305; `docs/adr/0124`).** Unknown keys used to be
  ignored, so a typo of `must_not` returned the rows it was written to exclude. Each node kind
  now has a closed key set, slop must be integral, a scoring leaf's folded boost must lie in
  [1e-6, 1e6] (checked on the folded value, which is deterministic, rather than on
  `boost * idf`, which would fail or pass as df drifts), and a tree has at most 1024 nodes,
  because the leaf cap bounded shape but not leafless padding that the evaluator walks per
  candidate. The builders stayed STRICT: making `field` nullable would change NULL semantics
  that ADR 0015 settled, for an all-fields leaf that raw jsonb already expresses.
- **Writes refuse on a standby with core's wording; whole-index reports refuse under RLS (#307,
  #310; `docs/adr/0029`, `0020` addenda).** The owned gate calls `PreventCommandDuringRecovery`,
  so every write entry point fails with 25006 before it touches a page, instead of failing at
  `XLogBeginInsert` with XX000 after the test allocator had already extended the standby's file.
  The readable gate refuses when RLS applies to the caller, rather than returning partial or NULL
  figures, because everything behind it reports on the whole index; that mirrors `pg_stats`
  omitting RLS tables. It checks the leaf heap only, because its ACL check is leaf-only too.
- **Per-field knob caps apply at DDL only, and an unmatched knob warns (#304; `docs/adr/0012`'s
  addendum).** Failing loud on stored values would wedge every INSERT on an index that already
  holds one, the #292 lesson; a WARNING rather than an ERROR keeps REINDEX and dump/restore
  working for indexes that carry a typo. The cost is that an index storing an over-cap value
  fails ALTER INDEX SET and plain dump/restore until the knob is RESET; pg_upgrade skips the caps.
- **"Throw-free" binds only WAL windows over published structure (#312, #313; `docs/adr/0083`'s
  addendum).** The segment builder's windows can throw (a `palloc` in the DICT window, a
  record-size limit in the chain writer), but every page they write is an orphan until the
  publish record and the transaction aborts on error, so a throw discards private state only.
  Moving every allocation and capacity check out of the hot builder loop would have bought no
  runtime safety.
- **Corruption tests forge pages through one raw lever (#302, #303, #309; `docs/adr/0126`).**
  About twenty fields needed forging that no semantic lever could write. One owner-gated raw
  writer, aimed by a layout table built from `offsetof`, costs less than twenty levers, keeps the
  tests in the SQL suites where coverage sees them, and is checksum-clean through Generic WAL,
  which a TAP that patches relation files is not on PG18's checksum-on default.
- **WAL consistency is checked in CI through a synchronous standby (#309; `docs/adr/0074`'s
  addendum).** The check runs only where WAL is replayed, so the long-lived pg_regress primary
  streams to a standby. An asynchronous standby fell more than 4 GB behind the cassert primary
  on a 2-core runner and lost its slot, so the standby is synchronous with `remote_apply`, which
  bounds its lag by the largest in-flight transaction rather than by runner speed, and a
  watchdog releases the primary if the standby dies or stalls so that a real inconsistency
  fails the job instead of hanging it.
- **Left as they are, by decision (2026-10-05).** No file was split, although several touched
  files are past 2,000 lines (`bm25_pending.c` is the candidate for a separate decomposition).
  `seal_threshold`'s 64 kB floor stays: lowering it is the same bounded, VACUUM-recoverable class
  as raising it, and a higher floor would still leave a lever (`docs/adr/0025`'s addendum).
  `bm25_score(ctid)` projected out of scan order still reads a heap page per row per scan before
  the hash that answers it (measured, `docs/adr/0103`'s addendum); delegated `@@@` membership
  still pays for a full ranked build (unmeasured, `docs/adr/0109`'s addendum); a document over
  the token ceiling is still analyzed in full before it is refused (`docs/adr/0079`'s
  addendum). Each is a constant factor, not a wrong answer.
