---
domain: baselines
workload: 3DMark05 GT1
subcategory: frame50
order: 04
title: Watchdog-Cleanup No-Gputrace Scout
date: 2026-06-06
type: scout
status: accepted
source: experiments/output/app-d3d9-3dmark05-watchdog-cleanup-smoke-r1/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-watchdog-cleanup-smoke-r1/dxmt9-perf-counter-comparison.md
---

# Watchdog-Cleanup No-Gputrace Scout

**Question / hypothesis.** After the 3DMark05 final-frame hang forced a
manual kill, re-run the current no-gputrace baseline through the new
top-level watchdog + Wine-prefix cleanup path. The goal is not a new GPU
proof; it is to verify that the supervised timeout path preserves the
same run-level performance shape and leaves no detached Wine process.

**Method.**

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix watchdog-cleanup-smoke-r1 \
  --frame 50 \
  --no-gputrace \
  --no-encoder-breakdown \
  --timeout 180 \
  --top 5

python3 scripts/tools/compare_3dmark05_perf_counters.py \
  experiments/output/app-d3d9-3dmark05-defaultgate-noenc-baseline-r1 \
  experiments/output/app-d3d9-3dmark05-watchdog-cleanup-smoke-r1 \
  --before-label baseline \
  --after-label watchdog
```

The wrapper watchdog fired at `timeout + slack = 225s`; the finalizer
synthesized counters from the final `[dxmt9-perf]` line because
`result.json` was not written. The run is therefore a **valid counter
sample**, but not a wallclock fps sample.

**Result.**

| Metric | Baseline | Watchdog cleanup | Delta |
|---|---:|---:|---:|
| `present_encoded` | 1,440 | 1,440 | 0 |
| `draw_calls` | 1,052,119 | 1,052,045 | -0.01% |
| `render_pass_begin` | 16,896 | 16,881 | -0.09% |
| `gpu_command_buffer_time_ms` | 4,201.354 | 4,207.759 | +0.15% |
| `completion_wait_ms` | 31,028.817 | 31,071.820 | +0.14% |
| `encode_draw_cpu_ms` | 16,189.862 | 16,086.742 | -0.64% |
| `d3d9_snapshot_draw_submission_cpu_ms` | 19,680.375 | 19,719.847 | +0.20% |
| `transient_upload_cpu_ms` | 3,244.832 | 3,206.023 | -1.20% |
| `render_pass_tile_preservation_bytes` | 181,233,512,448 | 180,717,559,808 | -0.28% |
| `argbuf_hybrid_bytes_per_encoder` | 1,219,486,376 | 1,218,338,320 | -0.09% |
| `transient_upload_bytes` | 1,219,607,252 | 1,218,459,196 | -0.09% |

Derived per-present shape:

| Metric | Watchdog cleanup |
|---|---:|
| Draws per present | 730.587 |
| Passes per present | 11.723 |
| `completion_wait_ms / present` | 21.578 ms |
| `gpu_command_buffer_time_ms / present` | 2.922 ms |
| `encode_draw_cpu_ms / present` | 11.171 ms |
| `d3d9_snapshot_draw_submission_cpu_ms / present` | 13.694 ms |
| `render_pass_tile_preservation_bytes / present` | 125.498 MB |
| `transient_upload_bytes / present` | 846.152 KB |

The comparison report's derived metrics are effectively flat:
`draws_per_present` -0.01%, `passes_per_present` -0.09%,
`completion_wait_ms_per_present` +0.14%,
`draw_run_records_per_submit` +0.17%, and
`backend_draw_run_batch_records_per_group` -0.12%.

```mermaid
flowchart TD
  Run["watchdog-cleanup-smoke-r1\npartial-log, 1440 presents"] --> Stable{"matches baseline shape?"}
  Stable -- "yes: GPU +0.15%, wait +0.14%" --> Counter["valid counter sample"]
  Stable -- "no" --> Reject["would reject as timeout artifact"]

  Counter --> Present["present pacing\n31.1s completion wait\n21.58ms/present"]
  Counter --> Encode["encode draw CPU\n16.1s total\n11.17ms/present"]
  Counter --> Snapshot["PE snapshot CPU\n19.7s total\n13.69ms/present"]
  Counter --> Store["pass/store traffic\n180.7GB preservation\n125.5MB/present"]
  Counter --> GPU["GPU command-buffer time\n4.21s total\n2.92ms/present"]

  Present --> PP"[present-pacing"]
  Encode --> SCE"[state-churn-encode"]
  Snapshot --> SNAP"[snapshot-cache"]
  Store --> RPS"[render-pass-store"]
  GPU --> HBS"[hidden-backend-storage"]

  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef bad fill:#f8d7da,stroke:#a33,color:#600
  class Counter,Stable accepted
  class Present,Encode,Snapshot,Store,GPU,PP,SCE,SNAP,RPS,HBS open
  class Reject bad
```

**Verdict.** Accepted as the current no-gputrace counter baseline after
the timeout-cleanup fix. The supervised watchdog path does not create a
new performance shape: presents, draws, render passes, GPU command-buffer
time, completion wait, draw batching, and upload bytes all stay within
ordinary run noise of [baselines-frame50.03](baselines-frame50.03.md) /
`defaultgate-noenc-baseline-r1`.

This run also clarifies the current residual budget:

- Wallclock/pacing: `completion_wait_ms` is still ~31 s, all previous
  evidence says it is present/display-sync paced ([present-pacing](../present-pacing.md)).
- CPU encode: `encode_draw_cpu_ms` is still ~16 s; current bind-skip work
  did not move wallclock, so the next CPU attribution needs sampling or
  finer internal timers, not more broad bind-cache guesses.
- PE-side state rebuild: `d3d9_snapshot_draw_submission_cpu_ms` is ~19.7 s
  and remains a major parallel CPU budget ([snapshot-cache](../snapshot-cache.md)).
- GPU/pass traffic: `gpu_command_buffer_time_ms` and tile preservation are
  stable; no new Xcode budget is justified unless a candidate preflight moves
  VS invocations, hidden-backend proxy bytes, or a semantic-safe locality gate.

**Related.** [baselines](../baselines.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) ·
[baselines-frame50.03](baselines-frame50.03.md) · [present-pacing](../present-pacing.md) · [state-churn-encode](../state-churn-encode.md) ·
[snapshot-cache](../snapshot-cache.md) · [render-pass-store](../render-pass-store.md) · [hidden-backend-storage](../hidden-backend-storage.md).
