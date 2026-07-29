---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 06
title: No-Encoder Default-Policy Smoke
date: 2026-06-05
type: experiment-run
status: accepted
outdated: evidence-missing
source: traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-r1/analysis/defaultgate-noenc-baseline-r1-vs-opaque-depth-r1-run-counters.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-baseline-r1/result.json; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-r1/result.json
---

# No-Encoder Default-Policy Smoke

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Does the opaque-depth index-cache opt-in still carry
meaningful CPU cost when the standard perf wrapper runs without diagnostic
encoder breakdown rows?

**Method.** Added wrapper support for `--no-encoder-breakdown` on `--no-gputrace`
runs. This avoids setting `DXMT9_PERF_ENCODER_BREAKDOWN`, so the run has no
per-encoder/per-stream rows and is useful only as a run-level default-policy
smoke. Then compared:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-baseline-r1 --frame 50 --no-gputrace \
  --no-encoder-breakdown --timeout 180 --top 5

bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-r1 --frame 50 --no-gputrace \
  --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

Both runs timeout-finalized cleanly (`status=pass`, `returncode=143`,
`present_encoded=1440`). The comparison report is
`traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-r1/analysis/defaultgate-noenc-baseline-r1-vs-opaque-depth-r1-run-counters.md`.

**Result.** Diagnostic rows are absent as intended (`Encoder lines: 0`,
`Stream lines: 0`, `Indexed probe draw lines: 0`). Run shape is stable:
`draw_calls 1,052,119->1,052,917` (`+0.08%`), `render_pass_begin
16,896->16,903` (`+0.04%`), and tile preservation traffic
`181,233,512,448->181,097,058,304` (`-0.08%`).

The opt-in CPU side-effect is smaller than the scoped diagnostic pair but still
real: `encode_draw_cpu_ms 16,189.862->16,405.450` (`+215.588ms`, `+1.33%`) and
`encode_draw_index_setup_cpu_ms 460.346->761.186` (`+300.840ms`). The source
resolve split remains flat (`121.914->118.070`, `-3.844ms`). The opt-in-only
owners are lookup `99.368ms` and candidate `160.505ms`; within candidate,
`candidate_build` is the largest sub-bucket (`132.302ms`), while measure/gate/apply
are small (`15.339`, `12.732`, `0.009`, `2.504ms`).

The production cache path is active: `125` candidate draws, `18` skipped,
`585,116` reordered-cache lookups, `243,389` hits, `341,584` rejected hits,
`143` misses, `67` created buffers, and LRU32 miss count
`530,289->418,033`.

**Verdict.** Accepted as the current default-policy CPU boundary. Disabling
diagnostic rows reduces the apparent overhead versus [index-cache-locality-cpucost.05](index-cache-locality-cpucost.05.md),
but the opt-in still adds candidate/lookup CPU. No-gputrace GPU proxies are mixed
(`gpu_command_buffer_time_ms +0.79%`, `completion_wait_ms -11.35%`), so this does
not change the production status: opaque-depth index-cache remains a proven
opt-in, not a shared perf default.

**Related.** [index-cache-locality](index.md) · prev: [index-cache-locality-cpucost.05](index-cache-locality-cpucost.05.md)
· [index-cache-locality-opaque.06](index-cache-locality-opaque.06.md) (earlier non-diagnostic smoke) ·
[index-cache-locality-opaque.07](index-cache-locality-opaque.07.md) (Xcode proof) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).
