---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality"
type: leaf
status: current
updated: 2026-08-30
source: experiments/output/app-d3d9-3dmark05-cpu-ready-active-terminal-h238-final-off-gt2-20260830; experiments/output/app-d3d9-3dmark05-cpu-ready-active-terminal-h238-final-on-gt2-20260830; experiments/output/app-d3d9-3dmark05-cpu-ready-active-head-reject-audit-h238-on-gt2-20260830
related: docs/perfomance/present-pacing/present-pacing-cpu-ready-tape-promotion-gate.237.md; specs/backend/encode-scheduling/gap.md; specs/verification/spec.md
---

# Present-Pacing #238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality

## Result

The CPU-ready coordinator can now tentatively retain one complete Ready head
behind an `ActiveRenderComplete` session, restore it before ordinary planning,
and preserve the active-render seed. The coordinator-owned terminal-suffix
transaction also accepts a carried session without replacing its command
buffer, completion prefix, callbacks, or capture ownership. Native fixtures
pin successor join and natural stop drain; `CpuReadyActiveHeadLookahead.tla`
pins pre-effect hold, FIFO restore, seed preservation, exactly-once effects,
and progress. The full TLA suite and all canonical native/Wine builds pass.

The final same-build GT2 pair used byte-identical staged binaries in both
lanes. It still fails the strict locality gate:

| Metric | Tape off | Tape on | Delta |
|---|---:|---:|---:|
| Presents | 1,624 | 1,632 | — |
| Sampled average FPS | 25.075 | 25.176 | +0.40% |
| Command buffers / Present | 3.9994 | 4.0092 | +0.25% |
| Render passes / Present | 15.7488 | 15.9400 | +1.21% |
| Tile preservation MiB / Present | 103.339 | 108.194 | +4.70% |
| GPU command-buffer ms / Present | 2.042 | 2.157 | +5.66% |

The FPS movement is noise and cannot override the command-buffer, pass, and
tile failures. `DXMT9_CPU_READY_TAPE` remains default off; no GT1, GT3, or SFIV
promotion matrix is justified.

## Rejection conservation

The diagnostic GT2 run makes the remaining boundary exact:

| Retained-head outcome | Count |
|---|---:|
| Attempts | 3,490 |
| Held and successor Ready | 2,018 |
| Reject: Present head | 1,245 |
| Reject: no ordered-tail Writing successor identity | 227 |
| Every other rejection reason | 0 |

The accounting is exact: `3,490 = 2,018 + 1,245 + 227`. Active-session
attempts were 1,932 with 461 successful holds; fresh attempts were 1,558 with
1,557 successful holds. Every held head reached its exact successor, restore
failures were zero, and GPU errors and chunk rejects were zero. The active
whole-head implementation therefore consumes every currently provable
Writing-successor opportunity. The remaining 227 observations occur before a
future source identity exists; they are not borrow-shape, source compatibility,
admission, capacity, stale-Writing, or reservation-race failures.

The carried terminal-suffix counters were all zero in the final GT2 pair. That
extension closes the code/spec ownership contract and its native cases, but the
measured GT2 stream does not contain that exact four-command transaction.

## Next gate

The next bounded design is an atomic producer-to-coordinator next-source intent
or publication reservation. It must expose the exact generation-stamped future
identity before the current source becomes Ready, reserve bounded capacity,
and provide cancellation and wake transitions. It must not create a Metal
effect, completion entry, or heuristic timeout. A refinement model must prove
identity conservation, cancellation progress, FIFO restore, and exactly-once
publication before production implementation.

Waiting longer without an identity is rejected: it would trade the measured
locality gap for an unbounded producer-progress dependency and would not satisfy
the queue liveness contract.
