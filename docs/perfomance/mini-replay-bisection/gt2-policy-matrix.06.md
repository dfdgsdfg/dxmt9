---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Same-image route and capacity matrix: exact pixels, unresolved CPU timing variation"
type: experiment
status: inconclusive-performance
updated: 2026-09-15
source: experiments/output/gt2-policy-matrix-20260915-r617/analysis.json; experiments/output/gt2-policy-matrix-20260915-r617/campaign.json; experiments/output/gt2-policy-matrix-20260915-r618.noindex/analysis.json; experiments/output/gt2-policy-matrix-20260915-r618.noindex/campaign.json; experiments/output/gt2-policy-matrix-20260915-r618.noindex/verdict.json
related: docs/perfomance/mini-replay-bisection/capacity-ownership-recovery.05.md; specs/experiments/harness/replay/fixed-input-experiment.md; specs/backend/encode-scheduling/gap.md
---

# Same-image GT2 replay policy matrix

## Verdict

Follow-up: the [corrected CPU matrix](gt2-qualified-policy-matrix.09.md) uses
separate attachment-content authority and requalified images. It does not
relabel the noisy pre-correction measurements recorded here.

All 20 public comparisons pass CPU semantic qualification, strict original
GPU source/output equality and post-publication identity reauthentication.
The experiment executes the requested route and capacity mechanisms, but
**does not establish a CPU performance ranking**. Identical-policy A/A pairs
and forward/reverse comparisons expose large process-level timing shifts.
No default, renderer code, build configuration or original oracle changed.

The 1MiB/64MiB policy is the smaller-memory follow-up candidate: it avoids the
21 post-reserve growth events of Direct exact and the nine growth events plus
aggregate denials of 5MiB/64MiB in this GPU replay. This is bounded mechanism
evidence, not a throughput or FPS promotion.

## Fixed conditions and execution

- HEAD `3eb34474`; Apple M1, AC power. CPU and GPU providers are the existing
  **x86_64 release/O3** binaries running via Rosetta. The ARM64 validator is
  outside the measured iteration. No rebuild occurred during either block.
- Original bundle:
  `experiments/render-tapes/gt2-gpu-source-stream-20260914-r405/frame-1651063776832900-1`.
  Both original RGBA hashes are
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
- Every policy uses v2 `capture-source`, source-plan SHA-256
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`,
  and command limit zero. Positive per-slot values share **64MiB aggregate**;
  exact policies use the required zero/zero disabled spelling.
- Each block runs A/A, Compatibility/Direct exact, exact/1MiB, 1MiB/5MiB,
  exact/5MiB, the four reversed pairs, then A/A again. Each pair has CPU
  verification twice and cold-device GPU verification twice per policy,
  followed by two CPU warm-ups and 16 measured iterations per policy.
- Total: 80 CPU verification iterations, 80 GPU iterations, 80 CPU warm-ups,
  and **640 measured CPU iterations**, all serialized. Child timeout is 180s.
  There is no Wine scene, GPU trace, sampler or concurrent build/test run.
- `run_matrix.py` in r617 records commands, policies, original identity,
  provider/library hashes and before/after host snapshots. r618 reuses that
  script, changing only the output directory to a `.noindex` suffix after
  Spotlight activity was observed. It changes no global indexing settings.
  Both blocks, including every slow sample, remain preserved.
- Each block's `analyze_matrix.py` reauthenticates CPU/GPU receipts, actual
  loaded images, staged/original bundles, exact readbacks and final publication
  through the existing acceptance predicates. Both audits pass 10/10.
  The combined r618 `verdict.json` pins identical mechanism observations across
  both blocks and explicitly records `performance_ranking_accepted=false`.

The measured CPU interval includes runtime resource setup, replay, controlled
completion and reset, excluding outer import/image hashing. Its completion
mode is `after-command` with one pending source; it does **not** measure pure
replay alone, encoder CPU, Wine arrival timing, GPU backpressure or game FPS.

## CPU timing: ranking withheld

The following are paired process medians over 16 iterations, in milliseconds.
Each A/A pair executes exactly the same Compatibility policy.

| Block / A/A | First process | Second process | Second vs first |
|---|---:|---:|---:|
| r617 start | 258.379 | 231.824 | -10.28% |
| r617 end | 236.854 | 231.472 | -2.27% |
| r618 start | 232.582 | 495.764 | +113.16% |
| r618 end | 237.310 | 233.147 | -1.75% |

For example, Direct exact versus Compatibility changes from +0.99% to -8.03%
when order is reversed in r617, and from -2.82% to +0.93% in r618. Direct exact
versus 5MiB reverses direction in both blocks too. Geometrically averaging these
ratios is recorded descriptively in `analysis.json`, not accepted as a gain.

| Policy | Range of all measured-process CPU medians (ms), both blocks |
|---|---:|
| Compatibility exact | 231.472–495.764 |
| ProductionSlot exact | 228.930–450.930 |
| ProductionSlot 1MiB / 64MiB | 229.892–233.327 |
| ProductionSlot 5MiB / 64MiB | 229.407–363.271 |

The 1MiB process medians are consistently narrow in this sample. Other policies
sometimes enter much slower modes even though their own ordered semantics,
executed counts and image identities match. CPU time closely follows wall time
in representative slow processes, so this is not explained by wall-only queue
waiting. Spotlight activity was observed during r617, but the `.noindex`
repeat still has large shifts: indexing is **not** established as the cause.
Before/after process snapshots and absence of a `pmset` thermal warning do not
prove core residency, frequency, allocator/cache state or within-iteration
host isolation. The slow-mode cause remains unassigned.

## Executed CPU mechanism

Values below are per iteration. Every CPU policy submits, completes and
reclaims 185 sources, with 656 source-summary commands and 1,866 draws.

| Policy | Direct / compatibility draws | Direct leases | Optional provision / reuse | Post-reserve growth | Capacity-ledger retained peak |
|---|---:|---:|---:|---:|---:|
| Compatibility exact | 0 / 1,866 | 0 | disabled | 0 | unmeasured |
| ProductionSlot exact | 1,856 / 10 | 182 | disabled | 1 | unmeasured |
| 1MiB / 64MiB | 1,856 / 10 | 182 | 3 / 179 | 0 | 1,193,400 B |
| 5MiB / 64MiB | 1,856 / 10 | 182 | 1 / 181 | 0 | 5,222,072 B |

CPU aggregate denial, exact recovery reuse/replacement/rejection and capacity
rotation are zero throughout. Thus larger headroom saves two optional
provision operations relative to 1MiB here; it does not change source count or
exercise the GPU-pressure denial behavior. Optional-provision counters do not
count all physical allocation calls in the exact path.

## Real GPU mechanism and original-pixel gate

Every GPU iteration encodes 186 chunks with zero GPU errors and exact original
source/output bytes. Compatibility executes 1,866 compatibility draws in 191
batches; each Direct policy executes 1,856 Direct draws through 182 leases and
10 compatibility draws in one batch. Seven additional unbatched draws are
reported separately by the GPU provider for every policy.

| Policy | Growth / GPU iteration | Optional provision / reuse, process total | Denials, process total | Exact recovery reuse / replacement, process total | Retained peak |
|---|---:|---:|---:|---:|---:|
| ProductionSlot exact | 21 | disabled | 0 | 0 / 0 | unmeasured |
| 1MiB / 64MiB | 0 | 52 / 312 | 0 | 0 / 0 | 21,872,392 B (20.86MiB) |
| 5MiB / 64MiB | 9 | 24 / 284 | 56 | 12 / 0 | 64,839,552 B (61.84MiB) |

GPU process totals cover **two** cold-device iterations, not one frame. CPU
stderr totals cover two warm-ups plus 16 measured iterations; per-iteration
CPU counts come from the native rows, with lease/growth averages explicitly
divided by all 18 iterations. Retained-byte gauges and peaks are never divided
or summed. They are capacity-ledger observations, **not RSS** or total resource
memory; zero with disabled headroom means unmeasured, not zero allocation.
All exact-recovery rejections and physical-path fallback draws are zero.
The 56 optional denials are safe policy outcomes, not GPU errors.

## Next gate

1. Isolate identical-policy fast/slow process modes before declaring a CPU
   winner: separate setup/replay/completion/reset cost in a diagnostic pair,
   then distinguish resource/allocator/cache work from CPU execution placement.
   Keep this diagnostic separate from low-observer timing.
2. Carry 1MiB/64MiB as the memory-efficient candidate, not a new default.
   Larger retention has no established benefit under these fixed boundaries.
3. Test locality/throughput only in a separate appropriately qualified
   completion/supply lane, followed by a supervised wild run. This immediate
   CPU lane cannot rank asynchronous queue pressure or cross-source batching.
