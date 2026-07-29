---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: coalesce
order: 04
title: H6 Benefit Ceiling — 38% of Tile Preservation Eliminable, ~3% of VS-write, FPS Conversion Unsettled
date: 2026-06-09
type: measurement
status: accepted-gate
---

# H6 Benefit Ceiling — 38% of Tile Preservation Eliminable, ~3% of VS-write, FPS Conversion Unsettled

**Question / hypothesis.** render-pass-store-coalesce.02 showed `passcoalesce`
removes **100%** of the distance-1 `A→B→A` re-entries on real GT1 frames. Before
paying the (large, risky) cost of the device-gated executor that would actually
merge them in the Metal stream, **measure the ceiling**: how much memory traffic
/ GPU time could H6 eliminate at most, and is that ceiling big enough vs the P0
VS-write bucket to plausibly move FPS?

**Method.** Read the run-level preservation + GPU-time counters from the
`traditional` GT1 run (1,680 presents; the DAG dump on the same run confirmed
100% of distance-1 re-entries are coalesce-eligible). The eliminable quantity is
the distance-1 RT+depth re-entry preservation (what coalescing the pair removes:
the producer's Store + the re-entry's Load). No executor needed — this is the
**upper bound** computed from measured bytes.

**Result.**

| Quantity | value | as % |
|---|---:|---|
| Total tile preservation (run) | 211.9 GB | 100% of preservation |
| **Eliminable: distance-1 RT+depth re-entry preservation** | **81.4 GB** | **38.4% of all tile preservation** |
| All same-key re-entry preservation (color 42.9 + depth 42.9) | 85.9 GB | 40.5% of preservation |
| VS-write / TVB (P0) ≈ 1,627 MiB/present | ~per-frame dominant | eliminable ≈ **~2.8% of VS-write** |

So H6 coalesce eliminates a **large share of the P1 tile-preservation budget
(38%)** — confirming it *is* the real P1 lever the domain claimed — but that
budget is itself **~3% of the P0 VS-write traffic** that owns GT1
([hidden-backend-storage](../hidden-backend-storage/index.md)).

**Two independent reasons the FPS payoff is likely small (and unsettled):**

1. **P0 dominates the memory ledger.** Eliminating 81.4 GB removes ~2.8% of the
   VS-write/TVB traffic; the coalesced passes' *draws* (and their VS invocations →
   TVB write) still execute unchanged. The P0 limiter is untouched.
2. **Wall-clock FPS is not GPU-bound here.** `gpu_command_buffer_time_ms` =
   5,254 ms / 1,680 presents = **3.13 ms/present** of GPU execution, yet the
   wall-clock envelope is ~15–22 fps — i.e. present/completion **pacing** (P4)
   dominates wall-clock, not GPU time. A GPU memory-traffic saving therefore maps
   to wall-clock FPS only if the run is both GPU-bound *and* bandwidth-bound on
   the preservation store/load — which the pacing dominance argues against.

**What is firm vs what needs the executor.**
- **Firm (byte ceiling):** 81.4 GB / 38% of tile preservation is eliminable; ~3%
  of VS-write. This bounds H6's maximum benefit.
- **Unsettled (FPS/GPU-time):** whether the eliminated store/load is on the GPU
  critical path and bandwidth-bound (base-M1 ~68 GB/s is tight; M1 Pro/Max less)
  — only the device-gated executor (drive the coalesced linearization) + a
  gputrace (per-encoder GPU time + tile store/load cost) can convert the byte
  ceiling into an FPS number. coalesce.02's structural proof + this byte ceiling
  do not.

**Verdict (gate).** Accepted as the H6 ceiling gate. H6 is **verified as the real
P1 tile-preservation lever** (eliminates 38% of preservation, 81.4 GB) but its
**FPS impact is bounded small** (~3% of VS-write) and likely near-zero at
wall-clock given pacing (P4) dominance — consistent with render-pass-store being
**P1/secondary** to the P0 VS-write bucket. Building the device-gated executor is
therefore justified only to *settle* the bandwidth-bound question (a potential
single-digit GPU-time win on base-M1), not as a likely large FPS lever. The FPS
mover remains **P0 = VS-invocation reduction** ([index-cache-locality](../index-cache-locality/index.md),
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md)), not pass coalescing.

**2026-06-12 current confirmation.** A fresh normal-visual no-gputrace run
(`app-d3d9-3dmark05-dag-current-20260612-203736`, `status: pass`, 1,680
presents) reproduces the same shape:

| Quantity | value |
|---|---:|
| Total tile preservation | 211.47 GB |
| Same-key re-entry preservation | 84.25 GB |
| Distance-1 RT+depth re-entry preservation | 79.68 GB |
| Distance-1 re-entry count | 3,399 / 3,762 same-key re-entries |
| DAG frame 48..52 pre-opt candidates | 5 / 5 safe-relocatable |
| DAG frame 48..52 post-opt candidates | 0 / 5 after `passcoalesce` |
| `framegraph_passes_coalesced` | 5 |
| GPU command-buffer time | 5.09 s total, 3.03 ms/present average |
| Completion/present wait | 39.52 s total, p50 25.60 ms, p95 33.38 ms |
| Frame sampling | p50 17.42 fps, tail samples around 23-24 fps |

This confirms the current visual-correctness state has not changed the H6
classification: passcoalesce is still a real P1 store/load lever, but the
wall-clock envelope is still dominated by completion/present pacing and the P0
hidden vertex/TVB path remains the likely FPS mover. The new wrapper also makes
the pre-opt/post-opt distinction explicit: pre-opt discovers candidates,
post-opt verifies that the analysis-only optimizer removes them.

**Related.** render-pass-store-coalesce.02 (100% coalesceable) ·
render-pass-store-coalesce.01 (WAW edge) ·
render-pass-store-passchain.01 (H5/H6) · [hidden-backend-storage](../hidden-backend-storage/index.md) (P0) ·
[present-pacing](../present-pacing/index.md) (P4 wall-clock) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).
