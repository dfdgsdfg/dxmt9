---
domain: present-pacing
workload: serialized query burst and production-loop release fixture
title: "H422: Clean-context producer acknowledgement reuses the closed prefix"
type: evidence
status: range-candidate-integrated-wild-promotion-open
updated: 2026-09-30
source: traces/query-ranges-h422/{native-final.log,native-detail.log,native-repeat.log,x64-final.log,x64-detail.log,tla.log,python.log}; tests/scripts/test_query_release_burst.py
related: present-pacing-close-consumer-refinement.421.md; ../../../specs/backend/query-scope-ranges.md
---

# H422: The retained ClosePass no longer always wakes Encode

Base `14a1b1ad` commits H421. `releaseCpuReadySessionBeforeOrderedControl`
now calls the consumer for qualified retained DirectObservation/ClosePass
requests. The ordinary FIFO event is still posted. Receipt, queue/epoch/work,
exact publication/execution coverage, current head and policy must all match.
No new runtime knob is added; the enclosing range lane stays default OFF.

## Context and synchronization preservation

While holding the queue mutex, the consumer takes `dirtyMutex_`, checks the
mask and all six high-water counters for zero, and keeps both locks through
the actual FIFO/raw acknowledgement. All current dirty setters and
`consumePendingDirty` release the dirty lock without acquiring the queue lock;
this ordering must be preserved. A concurrent dirty producer linearizes before
the check (ordinary fallback) or after the local acknowledgement.

The consumer never clears dirty data. Clean context consumption would be empty;
the no-active helper uses neither a completion snapshot nor context observer
pointers. A dirty context continues through the actual Encode helper path.
No source is submitted/completed by the fast path, and query FLUSH retains its
separate submission obligation. Failure leaves the FIFO head pending for Encode.

Success renews the receipt, advances covered raw identity and notifies release
waiters without waking Encode. Source mutation, publication mismatch, terminal,
stop, nonretained writer and submission requests cannot use this path.

## Evidence layers

- The H421 native checker covers 46,656 bounded serial/interleaved histories;
  the existing guard tests cover stale identities, policies and exactly-once.
- The actual production coordinator pauses after certifying a close. A clean
  repeated retained request must return while Encode is still paused. A dirty
  request must queue, stay blocked, then succeed through Encode after resume.
- The real Metal serialized burst uses exact query samples/pixels with
  contiguous/segmented transport and inline/offloaded replay. Candidate mode
  must record at least one actual receipt reuse; merely passing pixels without
  executing the consumer is insufficient.
- `SessionReleaseReuse` is append-only trace vocabulary. It records the local
  producer ack point, not a simulated helper outcome. Analyzer timing aliases
  that point as both action/ack endpoints (zero action-to-ack scope), reports
  `reused` separately, and leaves pending-session/record observations unknown.
  It rejects mixed reuse/helper transcripts and incompatible policy/thread data.

All four ON transport/replay combinations produce one actual `Closed` and
33 `reused` requests; all four OFF combinations retain two `Closed`, 31
`NoActivePass` and one no-session request. The main query remains exactly
1,024 samples, the empty query remains zero and the pixels remain exact.
Native focused tests pass 20/20, x86_64 tests plus install-name audit pass 20/20,
and Python analyzer tests pass 41/41. Three further native repetitions pass
60/60. The unchanged TLA suite passes all 227 configurations (65 safety, five
progress, 157 expected counterexamples). This confirms actual round-trip elision
for the fixture, not an FPS delta for STALKER.

The unchanged TLA suite is prerequisite regression evidence, not an unbounded
proof of every queue/reset/callback interleaving. Metal validation/readback is
correctness evidence, not performance measurement. No wild run or FPS improvement
is claimed here. Next qualify full-scene output and repeat observer-OFF STALKER
OFF/ON in reversed order; H416's old negative performance result is not replaced
by a micro-fixture activation result.
