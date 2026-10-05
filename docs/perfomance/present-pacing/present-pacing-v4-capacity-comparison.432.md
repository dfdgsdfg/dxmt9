---
domain: present-pacing
workload: STALKER CoP capture29
title: "V4 capacity candidate qualification and bidirectional CPU comparison"
type: experiment
status: measured-no-stable-capacity-advantage
updated: 2026-10-03
source: experiments/output/stkcop-v4-capacity-14/verification.md; experiments/output/stkcop-v4-capacity-14/paired-{exact,1mib,5mib}-r1/result.json; experiments/output/stkcop-v4-capacity-14/experiment-{exact-aa,exact-1mib,1mib-exact,exact-5mib,5mib-exact}-r1/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-fixed-input-route-comparison.431.md; specs/experiments/harness/replay/spec.md
---

# V4 retained-capacity comparison

Same-binary exact-fit, 1 MiB and 5 MiB ProductionSlot candidates pass original
CPU logical-stream and original GPU query/pixel qualification on capture29.
Positive candidates share a 64 MiB aggregate budget. All preserve 2,799 draws
and produce five GPU encode chunks with zero command-buffer errors.

Direct draw counts are 534 / 781 / 648, respectively. Continuation capacity
rejections are 26 / 19 / 23. Positive candidates each provision once; neither
shows aggregate denial, capacity rotation or retained-capacity reuse. Larger
nominal headroom therefore does not imply greater Direct coverage. The exact
physical dimension responsible for the difference is not established.

Five independently qualified ProductionSlot comparisons each use two warmups
and 16 measured fresh processes per arm. All 180 executions and final
authentication pass (20 warmups, 160 measured samples):

| Ordered arms | Body wall medians | Process CPU medians |
|---|---:|---:|
| Exact / Exact | 69.782 / 69.951 ms | 70.158 / 70.175 ms |
| Exact / 1 MiB | 69.648 / 70.986 ms | 69.957 / 71.227 ms |
| 1 MiB / Exact | 69.558 / 70.133 ms | 69.941 / 70.446 ms |
| Exact / 5 MiB | 70.084 / 69.847 ms | 70.589 / 70.194 ms |
| 5 MiB / Exact | 69.957 / 69.978 ms | 70.337 / 70.429 ms |

The 1 MiB wall effect changes sign (+1.920% forward, -0.820% reverse). The
5 MiB wall effect shrinks from -0.339% to -0.030%; CPU reductions are 0.560%
and 0.131%. A/A median differences are 0.241% wall and 0.024% CPU, not a
confidence interval or universal noise bound. There is no established material
capacity advantage in this cold input; Direct coverage alone is not a useful
optimization verdict. This is a cold replay CPU body with immediate FIFO consumer
completion, not Metal encode time or game FPS. No reuse occurs, so this input
cannot establish a warm persistent-slot reuse benefit.

The remaining full policy-interface work is independent bounded grouping:
actual changed publication partitions with preserved original logical/query
prefix/pixel evidence. This capacity matrix does not close that requirement.

The [verification record](../../../experiments/output/stkcop-v4-capacity-14/verification.md)
separates body route deltas from process-wide cumulative events and ledger gauges.
Ledger retained bytes are neither RSS nor strict source-byte bounds.
