---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 05
title: Direct Prefix Repair + Scoped CPU Gap Refresh
date: 2026-06-05
type: tooling-measurement
status: accepted
source: traces/app-d3d9-3dmark05-cpugap-opaque-depth-r2/analysis/cpugap-baseline-r2-vs-opaque-depth-r2-run-counters.md; experiments/output/app-d3d9-3dmark05-cpugap-baseline-r2/result.json; experiments/output/app-d3d9-3dmark05-cpugap-opaque-depth-r2/result.json
---

# Direct Prefix Repair + Scoped CPU Gap Refresh

**Question / hypothesis.** Can the current no-gputrace CPU-gap pair be refreshed
after artifact cleanup, and does the remaining opaque-depth opt-in CPU side-effect
still belong to cache/candidate work rather than base index-source resolve?

**Method.** The first baseline attempt (`cpugap-baseline-r1`) failed before launch:
the perf wrapper set `DXMT_3DMARK05_DIRECT=1`, while the direct launcher defaulted
to the optional `app-d3d9-3dmark05-verify` prefix, which no longer contained
`3DMark05.exe`. Fixed `run_3dmark05_perf_probe.sh` to pass
`DXMT_3DMARK05_PREFIX=experiments/prefixs/app-d3d9-3dmark05`, then reran:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix cpugap-baseline-r2 --frame 50 --no-gputrace --timeout 180 --top 5

bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix cpugap-opaque-depth-r2 --frame 50 --no-gputrace --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

Both r2 runs timeout-finalized cleanly (`status=pass`, `returncode=143`,
`present_encoded=1440`). The comparison report is
`traces/app-d3d9-3dmark05-cpugap-opaque-depth-r2/analysis/cpugap-baseline-r2-vs-opaque-depth-r2-run-counters.md`.

**Result.** The opt-in path is active: global run counters show
`586,014` reordered-cache lookups, `243,720` hits, `342,151` rejected hits,
`143` misses, and `67` created buffers. Scoped frame50 runtime proof reports
`198` lookups, `102` applied, `96` skipped, and LRU32 `460,019->333,936`
(`-126,083`, `-27.41%`).

CPU attribution matches the earlier conclusion: `encode_draw_index_setup_cpu_ms`
increases `499.330->1,036.356` (`+537.026ms`), while the narrow
`encode_draw_index_source_resolve_cpu_ms` is flat (`117.667->115.101`, `-2.18%`).
The opt-in-only owners are lookup `102.249ms` and candidate `430.685ms`
(`original_measure 82.453`, `candidate_build 275.823`, `candidate_measure 72.258`,
`gate 0.015`, `apply 2.907`).

**Verdict.** Accepted (tooling fixed, attribution refreshed). The source-resolve
bucket is not the CPU owner. This run is scoped by
`DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50`, so its candidate CPU is diagnostic-heavy
and should not replace the non-diagnostic fast-measure default-policy gate
([[index-cache-locality-opaque.06]]). Use it to confirm the wrapper prefix fix,
active cache behavior, and source-attribution boundary.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-cpucost.04]]
· [[index-cache-locality-opaque.06]] (non-diagnostic CPU gate) ·
[[index-cache-locality-opaque.07]] (Xcode proof) ·
[[overview-3dmark05-gt1]].
