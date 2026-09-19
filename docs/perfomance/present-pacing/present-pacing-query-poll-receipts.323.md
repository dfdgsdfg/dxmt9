---
type: "Performance Evidence"
title: "H323: immutable query polling and bounded replay drains"
description: "Opt-in query replay receipts separate CPU-prefix ownership from GPU completion."
tags: [performance, queries, replay, verification]
---

# H323: immutable query polling and bounded replay drains

Date: 2026-09-18. Base: `af3d740e`, with the uncommitted receipt implementation.
This is implementation/correctness evidence, not a measured STALKER speedup.

## Motivation and boundary

[H322](present-pacing-query-ready-replay-frontier.322.md) observes 1,773
stable-END calls with later raw work and 5.751 seconds of summed global drain.
That scope is a candidate, not a removable-time estimate or FPS projection.
The existing core GetData reads and sometimes mutates the live Query, so simply
waiting for an earlier raw ordinal would create an ownership race.

`DXMT9_QUERY_POLL_RECEIPTS=1` instead uses two independent authorities:

1. A query-specific admission receipt proves successful CPU replay through the
   whole raw containing its latest admitted Issue (BEGIN as well as END).
2. A retained immutable snapshot proves that issue's ordinary and initializer
   GPU completion and successful result storage before exposing bytes.

The candidate remains default OFF. Eager asynchronous receipts are eligible;
missing/non-raw/synchronous/deferred-marker receipts retain the global-drain
reference. Same-raw records after END are deliberately not skipped.

## Ownership and synchronization

- `ReplayDrainLedger::captureReceipt` freezes the current raw ordinal and
  retains the target. `waitReceipt` never follows the query's future tail.
- Newer admission before the synchronized snapshot copy returns `S_FALSE`.
  Terminal failure wins. Neither case modifies caller bytes.
- `visitReadyReceipt` copies under ledger then snapshot mutex. Replay releases
  the snapshot mutex before publishing raw completion to the ledger. Backend
  polling holds neither lock.
- Provider Issue publishes the snapshot with replay TLS raw identity. Both
  worker and inline/provider paths carry that identity; direct Issue uses zero
  and falls back. Admission remains transactional across contiguous/segmented
  import; raw handle retention outlives completion publication.
- `Device::pollQuerySnapshot` consumes copied values, retained result/backend
  owners and atomic loss/reset/epoch controls. It never reads or resolves the
  live Query. Later reissue cannot redirect a copied snapshot.
- The final control check linearizes a successful poll before a subsequent
  reset/loss. Reset may overlap its return; an observed in-progress reset or
  changed epoch invalidates success. The bridge stages at most eight public
  bytes and rechecks terminal replay before copying them to the caller.
- There is no per-Issue allocation. Enabled query creation allocates its cold
  target/snapshot state; receipt copies retain existing owners. Query wire
  records and the PE/Unix ABI are unchanged.

## Verification

The shared receipt reducer has 128 exhaustive bounded native cases, invalid
frontiers and terminal-priority checks. Ledger tests cover fixed-prefix waits,
supersession, pre-effect rejection, target retention and ready-only visitors.
The snapshot fixture covers legacy fallback, result formatting, initializer
pending, immutable reissue/release, foreign owner, device/backend failure,
stale epoch and reset/loss injected between entry and final control checks.

Actual Metal tests run with validation enabled in portable and forced-tile
configurations. A real command-buffer event gate proves pending bytes remain
unchanged. Candidate and reference report exactly 1,024 visible samples or zero;
old snapshots survive reissue/release, reset invalidates them, and all 32×32
BGRA framebuffer pixels match green exactly. This is a bounded core/backend
oracle, not a Wine/game-image equivalence claim.

Local logs:

- `build/meson-logs/query-receipt-build.log`
- `build/meson-logs/query-receipt-native.log`: 10/10 focused tests.
- `build/meson-logs/query-receipt-metal.log`: 4/4 Metal reference/candidate tests.
- `build/meson-logs/query-receipt-x86-tests.log`: 8/8 including x86_64 Metal and
  Unix install-name audit.
- `build/meson-logs/query-receipt-{unix,pe64,pe32}-build.log`: canonical full
  cross builds. No Wine runtime was staged or launched for these checks.

## Evidence limits and next experiment

No candidate wild run or matched OFF/ON performance pair is claimed. Broader
public PE concurrency/reset/fault evidence and full-sequence image equality
remain promotion gates. The bounded TLA/native correspondence does not prove
arbitrary C++ scheduling or driver fault recovery.

H322's observer populations cannot be compared unchanged: when the candidate
handles a poll, the mutable core/global-drain observers are bypassed, not run
concurrently or used to restore a global wait. Their enabled rows describe
fallback calls only. A future matched STALKER experiment must identify candidate
versus fallback call populations explicitly, separate observer-OFF visual/FPS
qualification from diagnostics, and retain identical binary/prefix/settings.
