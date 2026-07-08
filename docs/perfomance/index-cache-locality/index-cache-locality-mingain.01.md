---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: mingain
order: 01
title: Cached Index Min-Gain 0 Follow-Up
date: 2026-06-04
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L20010-L20099
---

# Cached Index Min-Gain 0 Follow-Up

**Question / hypothesis.** Does lowering the production cached-index gain threshold
from `10` to `0` (admitting more cached candidates) reduce Xcode VS invocations and
GPU time beyond the accepted min-gain-10 combined run?

**Method.** `run_3dmark05_perf_probe.sh --suffix combined-opaque-screenblend-index-cache-min0-gputrace-r1
--frame 50 --optimize-opaque-depth-index-cache --optimize-opaque-depth-index-cache-min-gain-pct 0
--optimize-screen-blend-index-cache --optimize-screen-blend-index-cache-min-gain-pct 0
--target-row-key 50/0 --target-row-key 50/1 --target-row-key 50/2
--compare-baseline-output <min-gain-10 combined> --baseline-joined <combined joined>
--require-target-reordered-index-cache-hits --require-target-index-cache-opt-miss32-decrease`,
strict shape gates. Xcode replay is the accepted proof (no-gputrace hit counts drift).

**Result.** Shape/cache-hit gates passed but no Xcode counter win:
`total_gpu_ms 30.923→30.934` (`+0.04%`); `top_gpu_ms 30.302→30.330` (`+0.09%`);
`top_vs_buffer_write_mib 1,412.612→1,412.461` (`-0.01%`);
`target_vs_invocations 1,033,772→1,031,214` (`-0.25%`).
More cache events: `target_reordered_index_cache_hits 168→229` (`+36.31%`),
rejected `133→72` (`-45.86%`); but average locality weakens:
`miss_delta_pct_32 -27.021% → -23.902%`. Per-row mixed: `50/1` GPU `+4.14%`,
`50/2` `-1.41%`, `50/0` `-0.53%`.

**Verdict.** Rejected as a global threshold change. Min-gain-0 admits lower-quality
candidates: more applied buffers, weaker average gain, no hardware VS invocation/write
movement. Keep the guarded min-gain-10 production threshold; a lower threshold needs
row/class-specific Xcode VS-invocation proof, not just software LRU counters.

**Related.** [index-cache-locality](index.md) · [index-cache-locality-opaque.07](index-cache-locality-opaque.07.md)
(the accepted min-gain-10 proof) · [index-cache-locality-screenblend.03](index-cache-locality-screenblend.03.md) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (remaining `hidden_vertex_tiler_parameter_storage:3` owner).
