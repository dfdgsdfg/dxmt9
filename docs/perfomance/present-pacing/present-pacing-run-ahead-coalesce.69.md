---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: overlap
order: 69
title: Run-Ahead Coalescing Creates Overlap but Fails Locality
date: 2026-06-16
type: experiment-run
status: accepted-mechanism-rejected-current-carrier
source: experiments/output/app-d3d9-3dmark05-runahead-coalesce-baseline-r1/result.json; experiments/output/app-d3d9-3dmark05-runahead-coalesce-baseline-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-runahead-coalesce-on-r2/result.json; experiments/output/app-d3d9-3dmark05-runahead-coalesce-on-r2/3dmark05-perf-summary.md; traces/app-d3d9-3dmark05-runahead-coalesce-on-r2/analysis/frame60-perf-counter-comparison.md
related: docs/perfomance/present-pacing/present-pacing-run-ahead-design.68.md, docs/perfomance/present-pacing/present-pacing-overlap-locality-gates.51.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present Pacing 69 - Run-Ahead Coalescing Creates Overlap but Fails Locality

## Question

After implementing `DXMT9_OFFSCREEN_RUN_AHEAD` plus encode-side ready-slot
coalescing, does the new path recover P4 overlap without breaking the H57
Metal locality gates?

## Verdict

The mechanism works, but the current carrier is rejected.

Present completion wait almost disappears, and the completion watcher now sees
substantial enqueue overlap during waits. However, the implementation creates
many more Metal command buffers per present, raises total completion wait, and
greatly increases GPU command-buffer time. This is the same class of failure as
the draw-count publish experiments, only through a different early-publish
path: P4 overlap is reachable, but the current ready-slot publication/coalescing
shape does not preserve Metal command-buffer locality.

Restoring `command_buffers_per_present` near baseline is a promotion gate, not
an FPS proof by itself. A successful follow-up must also reduce
`wait -> next enqueue`, total completion wait, or actual wallclock/frame cadence.

## Runs

Baseline:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix runahead-coalesce-baseline-r1 \
  --frame 60 \
  --no-gputrace \
  --timeout 120
```

Candidate:

```sh
DXMT9_OFFSCREEN_RUN_AHEAD=1 \
DXMT9_ENCODE_COALESCE_READY_SLOTS=1 \
DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT=4 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix runahead-coalesce-on-r2 \
  --frame 60 \
  --no-gputrace \
  --timeout 120 \
  --compare-baseline-output experiments/output/app-d3d9-3dmark05-runahead-coalesce-baseline-r1
```

Both accepted artifacts are `status=pass`, `capture_error=None`. An earlier
candidate rerun was killed externally and produced `missing_capture`; do not use
that partial run as evidence.

## Key Counters

`present_encoded` differs (`1782` baseline vs `1336` candidate), so interpret
the comparison as per-present shape first, not raw total work.

| Metric | Baseline | Run-ahead/coalesce r2 | Direction |
|---|---:|---:|---|
| `completion_present_wait_ms_per_present` | `29.839` | `0.202` | present wait removed |
| `completion_wait_with_enqueue_ms_per_present` | `1.915` | `20.855` | overlap created |
| `completion_wait_without_enqueue_ms_per_present` | `27.924` | `16.135` | no-enqueue wait reduced |
| `completion_wait_ms_per_present` | `29.839` | `36.990` | total wait worse |
| `completion_wait_overlap_share_pct` | `6.419` | `56.381` | overlap share up |
| `command_buffers_per_present` | `3.999` | `19.156` | locality fail |
| `sub_command_buffers_per_present` | `2.998` | `10.394` | locality fail |
| `passes_per_present` | `11.769` | `11.866` | roughly flat |
| `gpu_command_buffer_time_ms_per_present` | `3.718` | `35.197` | severe regression |
| `commit_chunk_replay_cpu_ms_per_present` | `8.363` | `28.476` | severe regression |
| `encode_chunk_cpu_ms_per_present` | `13.485` | `14.490` | slight regression |
| `chunk_publish_reason_draw_limit` | `0` | `15852` | early publish dominates |

The raw comparison reports `gpu_command_buffer_time_ms` `6626.041 -> 47023.459`
(`+609.68%`) and `command_buffers` `7127 -> 25592`. `passes_per_present` is
nearly flat, so the primary failure is not simply more render passes; it is too
many Metal command-buffer carriers for approximately the same pass structure.
Tile preservation also fails the conservative gate: total
`tile_preservation_mib` rises `+0.93%`, and normalized bytes per present rise
because the candidate completed fewer presents.

## Interpretation

The useful signal is strong:

- `Present` is now effectively the only drawable-gated synchronization point.
- Offscreen work does run while the previous present completion wait is active.
- `completion_enqueue_while_waiting_per_present` rises from `0.042` to `5.188`.

The rejected part is equally strong:

- Early publish produces `15852` draw-limit publications.
- Encode-side coalescing with limit `4` does not reconstruct the baseline Metal
  command-buffer chain.
- The extra command-buffer carrier cost is larger than the present-wait win.

Therefore H73 is validated as a design constraint: run-ahead must decouple CPU
readiness from Metal command-buffer publication, or the encoder must coalesce
ready work strongly enough that the final CB/pass/tile shape remains close to
baseline. The current implementation is an overlap proof, not a performance
promotion.

## Follow-up Gate

Do not run a `.gputrace` proof for this exact shape. The next no-gputrace
candidate should first satisfy all of:

| Gate | Required movement |
|---|---|
| Overlap | keep high `completion_wait_with_enqueue_ms_per_present` or lower `completion_wait_without_enqueue_ms_per_present` |
| Locality | `command_buffers_per_present` near baseline and no increase in render passes or tile preservation |
| Total wait | lower `completion_wait_ms_per_present`, not only present wait |
| Cadence | lower `wait -> next enqueue` / `wait -> command buffer commit`, or show better fixed-workload wallclock |
| Correctness | `status=pass`, normal visual smoke, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0` |

Candidate directions remain the H73 set: CPU-ready staging before publish, a
larger encode-side merge window that preserves deterministic ordering, or a
render-pass-boundary-only experiment whose carrier proves H57 locality before
any FPS claim.
