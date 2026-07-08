---
type: "Spec Gap"
title: "Benchmarks Gap"
description: "Implementation and evidence gaps for benchmark coverage."
tags: [specs, gap, benchmarks]
---

# Benchmarks Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Benchmarks Layer

No benchmarks exist yet. All R-BENCH-1.x through R-BENCH-5.x are not started,
and the architecture bottleneck/concurrency evidence for R-ARCH-2.6-R-ARCH-2.7
and R-ARCH-6.* is still missing.

| Area | Status | Spec |
|---|---|---|
| `dxmt9-bench` harness | ❌ | R-BENCH-1.1 |
| Draw call throughput workload | ❌ | R-BENCH-2.2 |
| Bridge operation budget counters | ⚠️ | `dxmt9-bridge-ops-spec` provides static opcode-budget evidence; workload-level runtime counters and benchmark JSON still required for R-BENCH-2.3-R-BENCH-2.5, R-BENCH-5.3 |
| Architecture bottleneck/concurrency counter baselines | ❌ | R-ARCH-2.6-R-ARCH-2.7, R-ARCH-6.*, R-BENCH-2.6 |
| PSO compile cold/warm workload | ❌ | R-BENCH-2.1 |
| Reference stack baselines (wined3d, DXVK+MoltenVK) | ❌ | R-BENCH-3.1 |
| `bench_compare.sh` regression script | ❌ | R-BENCH-4.3 |

---
