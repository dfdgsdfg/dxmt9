---
domain: index-cache-locality
subcategory: opaque
order: 06
title: Fast-Measure Opt-in vs Baseline Gate
date: 2026-06-05
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L491-L575
---

# Fast-Measure Opt-in vs Baseline Gate

**Question / hypothesis.** After the dense-adjacency / LRU32-only candidate CPU
cleanup ([[index-cache-locality-cpucost.03]]), is the opt-in's CPU side-effect now
small enough, while the GPU-side win holds, on a paired non-diagnostic smoke?

**Method.** Paired `run_experiment.py run app-d3d9-3dmark05` under
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_3DMARK05_RESULT_FILE=dxmt9_gt1.3dr`,
opt-in adding `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` / `_MIN_GAIN_PCT=10`,
`--timeout 180`. Both timeout-finalized pass, `present_encoded=1440`.

**Result.** Opt-in active: `indexed_cache_opt_candidate_draws=125`,
`reordered_index_cache_hits=243,183`, `reordered_index_cache_rejected_hits=341,046`,
`reordered_index_cache_created=67`. CPU now smaller but not free:
`encode_draw_index_setup_cpu_ms 314.614→623.709` (`+98.25%`, ≈`+309ms` over the full
1440-present run); lookup `103.023ms`, candidate `159.933ms`, apply `2.737ms`.
GPU does not regress: `gpu_command_buffer_time_ms 4,318.507→4,293.641` (`-0.58%`);
`completion_wait_ms 31,672.734→27,901.769` (`-11.91%`).

**Verdict.** Accepted (GPU win preserved, CPU side-effect much reduced). Index setup
still adds ~`309ms`, so keep `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` off in the
shared `perf` profile until that CPU cost drops or a broader runtime gate proves
net positive. Completion-wait is a timing proxy, not a sole proof gate.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-cpucost.03]]
(the speedup) · next: [[index-cache-locality-opaque.07]] (the Xcode proof) ·
[[tvb-mechanism-proof]].
