---
domain: present-pacing
workload: STALKER CoP capture29
title: "Existing query clocks do not explain the grouping replay-wall gap"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-query-timing-20/verification.md; experiments/output/stkcop-v4-query-timing-20/{l0-r1,l3-r1,l64-r1}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-inprocess-repetitions.436.md; specs/experiments/harness/replay/spec.md
---

# Query setup, transcript and oracle attribution

The private diagnostic now exports existing query clocks and setup counts in
an optional strict `dxmt9.render_tape.v4_query_timing.v1` block. No runtime clock
or query behavior changed. Legacy absence is accepted; malformed present
measurements reject. Failed/inspection/query-free observations remain null.

On one final executable and identical loaded images, capture29 L0/L3/L64 each
pass two warm-ups plus 16 measured executions. All 54 match original source and
output bytes and EVENT readiness, with zero GPU errors.

| Policy | Replay wall median ms | Checkpoint setup median ms | Captured GetData median ms | Supplemental oracle ms |
|---|---:|---:|---:|---:|
| L0 | 356.343 | 7.526 | 0.002396 | 0 |
| L3 | 333.644 | 7.672 | 0.002479 | 0 |
| L64 | 325.987 | 7.304 | 0.002458 | 0 |

These are overlapping elapsed service scopes, not additive CPU/GPU times.
Checkpoint restoration creates two completed EVENTs with two END calls and
roughly 33,000--35,000 polls/yields per execution. Yield counts are not blocking
wait duration and do not describe the live game's query frequency. The setup
ordinary queue-publication delta is zero, which says nothing about initializer
or fence submissions. The captured poll is already ready; no supplemental
oracle work occurs.

The observed query setup medians vary by less than 0.4 ms, whereas the replay
wall medians differ by 23--30 ms. These query scopes therefore do not explain
the grouping gap. This does not eliminate the unmeasured pre-arm checkpoint,
admission or finish/semantic-validation work. The duplicate admission found in
[H436](present-pacing-v4-inprocess-repetitions.436.md) remains an unpriced
candidate, not a proven live-game bottleneck.

Both canonical native/x86 builds, native CLI 2/2 and x86 CLI/install-name 3/3
pass. No new TLA run is claimed for serialization-only changes. Background
load, uncontrolled caches and fresh-device limitations remain; no policy/FPS
promotion or numeric occlusion qualification follows. Next is prepared-only
attribution of admission, pre-arm checkpoint and finish before changing their
ownership or caching behavior.
