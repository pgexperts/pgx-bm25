---
id: 0063
title: The POS and POST decode paths copy the page and unlock before decoding
date: 2026-08-18
status: Accepted
summary: A buffer content lock is an LWLock and LWLockAcquire holds off interrupts, so CHECK_FOR_INTERRUPTS is a no-op under one; the POS cursor and the POST page walk now take a private BLCKSZ copy and release the lock before decoding, which is what makes the scan's two cancellation points actually fire.
---

# 0063. The POS and POST decode paths copy the page and unlock before decoding

## Context

`LWLockAcquire` calls `HOLD_INTERRUPTS()`, so `CHECK_FOR_INTERRUPTS()` does
nothing while any buffer content lock is held — `ProcessInterrupts` returns at
its guard while `InterruptHoldoffCount != 0`. This codebase documents that
mechanism in three separate places, and then placed its most important
cancellation check inside exactly such a hold.

`pos_cursor_open` SHARE-locked a POS page and the cursor held one continuously
until close: `pos_cursor_next_page` unlocked the old page only *after* locking
the next. So the check in `bm25_seg_scan_postings`' decode loop — whose own
comment called it "what makes a long scan cancellable at all" — was dead on
every phrase and proximity query. A phrase on a common term with `df` in the
millions ignored `pg_cancel_backend`, SIGINT and `statement_timeout` until the
entire term's POST+POS replay finished.

The issue framed this as a POS-cursor problem. It is not only that: the POST page
lock independently killed a *second* check, the one inside `chain_read_at`,
reachable only from the `cb`/`pos_cb` callbacks while that page is held — along
with its worst case, a cursor backward jump re-walking a ~500-page NORMS chain
per posting. Peak content-lock nesting on the phrase path was three.

## Decision

Both pages are copied and unlocked before decoding.

`PosCursor` no longer holds a buffer at all — `Buffer buf` is removed from the
struct, which IS the invariant: the cursor holds nothing, pinned or locked,
between calls. It keeps a `palloc(BLCKSZ)` private copy. One `pos_cursor_load()`
centralises the fetch in a load-bearing order: extent bound and cycle cap before
the page is touched at all; `ReadBuffer` + `LockBuffer`; kind and gen validation
**under the lock** (that contract exists so the gen cannot be read torn against a
replay-time FPI re-init); `bm25_page_content_bytes` **under the lock**, before
the copy is trusted; `memcpy`; `UnlockReleaseBuffer`; and only then derive
`next`/`cur`/`end` from the copy.

The same idiom is applied to the POST page in `bm25_seg_scan_postings`.

The copy is per PAGE, not per frame. A POS frame is `varbyte(tf)` followed by
`tf` deltas; the INSERT path caps `tf` at 65535 but `ambuild` does not (see
issue #158), so a frame can reach hundreds of megabytes. `BLCKSZ` is the only
sound bound.

## Alternatives considered

- **Release and re-acquire the lock around each interrupt check.** Rejected: it
  re-validates the page every cycle, leaves the lock-across-decode shape in
  place, and does nothing for the nested `chain_read_at` check.
- **Fix only the POS cursor**, as the issue describes. Rejected on measurement:
  the POST lock kills `chain_read_at`'s check on its own, so half the defect
  would have survived — including the uncancellable NORMS re-walk, which is the
  worse of the two worst cases.
- **Add more `CHECK_FOR_INTERRUPTS` calls without breaking the hold.** This is
  the trap ADR 0041 already recorded: a raw count cannot distinguish a live check
  from a dead one, and adding dead ones raises the CI floor while changing
  nothing.
- **Copy per frame rather than per page.** Rejected: unbounded, per above.

## Consequences

- The two checks that matter go from dead to live **without changing the
  interrupt-check count at all** — the mirror image of ADR 0041's 68→61. A count
  floor cannot see this fix; only the latency harness can.
- Lock nesting on the phrase path drops from three levels to one, then to zero
  during decode. Every existing lock-ordering edge this removed (POS-before-POST,
  POST-before-NORMS/LIVE/DOCMAP) is strictly one fewer deadlock edge, and the
  `palloc`/`hash_search`/`repalloc` that `seg_phrase_pos_cb` performed under
  three content locks now runs under none.
- Cost is one 8 KB `memcpy` per page crossed, amortised over the thousands of
  postings or positions that page holds — against which the old behaviour held a
  shared-buffer content lock across an entire term's replay.
- Correctness of decoding off a copy rests on published segment pages being
  write-once (the only post-publish writer, `bm25_livedocs_clear`, touches
  LIVE/SEGCAT/META only) and on reuse being gated by the retire horizon (ADR 0019
  addendum). A substitution of the NEXT page is still caught by that page's own
  gen check under its own lock, so the standby-without-feedback case remains a
  retryable serialization failure.
- Testing this needs a LATENCY harness, not an outcome assertion: the query
  cancels either way, just far later. `sql/66_scan_interrupts` states that
  explicitly. The boundary was verified by A/B rather than assumed — with the
  copies reverted the assertion returns f, with them it returns t — and the file
  records that the discrimination comes from the gap between one page and one
  full term, so a slow runner should raise the row count and boundary together
  rather than shrinking the boundary.

## Addendum (2026-10-05)

This record's claim that page-granularity standby exposure "remains a retryable serialization
failure" held only for pages that carry a generation. Pending pages now carry a chain epoch,
and the scan-side walkers raise `ERRCODE_T_R_SERIALIZATION_FAILURE` for a page re-initialized
after the scan's snapshot (#291, ADR 0110). The walkers read through `bm25_pending_walk_read`,
which returns the buffer SHARE-locked, so a caller may still copy the page and release at once,
as here, or decode in place. One exposure stays XX002: a pending page recycled as a SEGCAT or
RETIRED page, whose `seg_gen` is 0.
