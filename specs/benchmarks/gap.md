---
type: "Spec Gap"
title: "Benchmarks Gap"
description: "Implementation and evidence gaps for benchmark coverage."
tags: [specs, gap, benchmarks]
---

# Benchmarks Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Benchmarks Layer

No deterministic native `dxmt9-bench` suite exists yet. All R-BENCH-1.x through
R-BENCH-5.x remain not started. Wild-run architecture attribution now provides
useful current evidence for R-ARCH-2.6-R-ARCH-2.7 and R-ARCH-6.* — counters,
Time Profiler thread ownership, PE module/PC sampling, and Metal interval
sampling — but it does not satisfy the reproducible native benchmark contract.

| Area | Status | Spec |
|---|---|---|
| `dxmt9-bench` harness | ❌ | R-BENCH-1.1 |
| Draw call throughput workload | ❌ | R-BENCH-2.2 |
| Bridge operation budget counters | ⚠️ | `dxmt9-bridge-ops-spec` provides static opcode-budget evidence; workload-level runtime counters and benchmark JSON still required for R-BENCH-2.3-R-BENCH-2.5, R-BENCH-5.3 |
| Architecture bottleneck/concurrency counter baselines | ⚠️ | The 2026-08-25 GT2 current-cap run and H236 join no-gputrace counters, clean Time Profiler producer/replay/encode ownership, a 250 Hz PE module/PC sample, and a Metal GPU interval sidecar. This is architecture evidence, not a committed deterministic `dxmt9-bench` workload, schema, threshold, or reference-stack baseline required by R-ARCH-2.6-R-ARCH-2.7, R-ARCH-6.*, R-BENCH-2.6. |
| PSO compile cold/warm workload | ❌ | R-BENCH-2.1 |
| Reference stack baselines (WineD3D, D9VK+MoltenVK) | ⚠️ Wild 3DMark05/06 and SFIV baselines are recorded in `docs/perfomance/baselines/`; committed deterministic `dxmt9-bench` JSON baselines remain missing. | R-BENCH-3.1-R-BENCH-3.3 |
| `bench_compare.sh` regression script | ❌ | R-BENCH-4.3 |
| Actual PE constant-setter economy | ⚠️ Implemented and builtin PE32/PE64 qualified: H280's eight matched-image processes pass 528 cells, with explicit process CPU scope, source/getter checks, strict native parser, environment and image receipts. App-local execution and whole-workload CPU/FPS promotion remain open. This is separate from native `dxmt9-bench` and does not close its gaps. See `docs/perfomance/present-pacing/present-pacing-pe-constant-economy.280.md`. | R-BENCH-6.1–6.4 |

---
