---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: overlap
order: 70
title: CPU-Ready Run-Ahead Restores Some CB Locality but Misses FPS and Correctness Gates
date: 2026-06-17
type: experiment-run
status: accepted-locality-refinement-rejected-correctness-and-promotion
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-cpu-ready-rback240-r1/result.json; experiments/output/app-d3d9-3dmark05-cpu-ready-rback240-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-cpu-ready-rback240-r1/3dmark05-perf-frames.csv; experiments/output/app-d3d9-3dmark05-cpu-ready-rback240-r1/actual.png; traces/app-d3d9-3dmark05-cpu-ready-rback240-r1/analysis/frame60-perf-counter-comparison.md; traces/app-d3d9-3dmark05-cpu-ready-rback240-r1/analysis/frame60-perf-counter-comparison-vs-singlecb.md; traces/app-d3d9-3dmark05-cpu-ready-rback240-r1/analysis/frame60-perf-counter-comparison-vs-on-r2.md
related: docs/perfomance/present-pacing/present-pacing-run-ahead-coalesce.69.md, docs/perfomance/present-pacing/present-pacing-run-ahead-design.68.md, docs/perfomance/present-pacing/index.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present Pacing 70 - CPU-Ready Run-Ahead Restores Some CB Locality but Misses FPS and Correctness Gates

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

Current-code note (2026-06-18): this document is historical experiment
evidence. The CPU-ready run-ahead implementation and env knobs were later
reverted, and current HEAD no longer honors `DXMT9_OFFSCREEN_RUN_AHEAD`,
`DXMT9_ENCODE_COALESCE_READY_SLOTS`, or
`DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT`. See
[present-pacing-run-ahead-current-code.73](present-pacing-run-ahead-current-code.73.md) before scheduling any follow-up
run.

## Question

In the historical R-BACK-2.40 prototype, CPU-ready staging held replayed work
independently of the final Metal command-buffer boundary. Did that preserve
enough baseline command-buffer locality to turn the earlier
run-ahead/coalescing overlap proof into an FPS-facing win?

## Verdict

CPU-ready staging was the right structural direction compared with the first
run-ahead/coalescing carrier, but this prototype still failed promotion.

The run restores much of the command-buffer shape versus the prior coalesced
experiments: `command_buffers_per_present` falls from `19.156` in
`runahead-coalesce-on-r2` and `14.686` in `singlecb-r1` to `5.741`, while
`sub_command_buffers_per_present` falls to `1.287`. Present completion wait is
also effectively removed (`0.116ms/present`).

The FPS gate still fails. Against the fresh baseline, total completion wait
worsens (`29.839 -> 40.347ms/present`), fixed-workload progress is lower
(`1782` baseline presents vs `1141` CPU-ready presents in roughly the same
120s scout window), and `commit_chunk_replay_cpu_ms_per_present` rises sharply
(`8.363 -> 40.441`). The run proves that restoring CB locality near baseline is
necessary but not sufficient; the candidate must also reduce total wait,
wait-to-next-enqueue, or fixed-workload wallclock.

The visual correctness gate also fails. `actual.png` shows a large black
vertical occluder/band over the left side of the scene behind and around the
muzzle bloom. The harness `status=pass` therefore only means the process
completed without reported Metal/launcher errors; it is not a valid visual
smoke pass for promotion.

## Run

```sh
DXMT9_OFFSCREEN_RUN_AHEAD=1 \
DXMT9_ENCODE_COALESCE_READY_SLOTS=1 \
DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT=4 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix cpu-ready-rback240-r1 \
  --frame 60 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --compare-baseline-output experiments/output/app-d3d9-3dmark05-runahead-coalesce-baseline-r1
```

The run finished with `status=pass`, `timed_out=false`, `returncode=0`,
`capture_error=None`, `gpu_command_buffer_errors=0`,
`draw_skipped_no_pipeline=0`, `map_buffer_wait_ms=0.000`, and
`queue_sequence_wait_ms=0.000`. These counters are clean for crash/error
triage, but `actual.png` has the large black vertical artifact described above,
so visual correctness is broken. It emitted `1141` frame-sampling rows. The
summary reports `sampled_frames=1140`, `sampled_wall_ms=109971.013`, and
`sampled_avg_fps=10.366`.

The result JSON carries only wrapper-level elapsed time
(`process_elapsed_sec=124.720`), not the 3DMark UI score. Older baseline and
run-ahead comparison artifacts did not preserve non-empty frame-sampling CSVs,
so the FPS row above is a current-run scalar, not a same-instrumentation
baseline-vs-candidate FPS A/B.

## Comparison To Baseline

| Metric | Baseline | CPU-ready r1 | Direction |
|---|---:|---:|---|
| `draws_per_present` | `738.907` | `762.986` | similar |
| `command_buffers_per_present` | `3.999` | `5.741` | still above baseline |
| `sub_command_buffers_per_present` | `2.998` | `1.287` | improved |
| `passes_per_present` | `11.769` | `12.076` | roughly flat |
| `tile_preservation_mib` | `215,140.918` | `159,391.855` | lower total |
| `completion_wait_ms_per_present` | `29.839` | `40.347` | worse |
| `completion_present_wait_ms_per_present` | `29.839` | `0.116` | present wait removed |
| `completion_wait_with_enqueue_ms_per_present` | `1.915` | `27.502` | overlap created |
| `completion_wait_without_enqueue_ms_per_present` | `27.924` | `12.845` | no-enqueue wait lower |
| `completion_wait_overlap_share_pct` | `6.419` | `68.164` | overlap share up |
| `commit_chunk_replay_cpu_ms_per_present` | `8.363` | `40.441` | severe regression |
| `encode_chunk_cpu_ms_per_present` | `13.485` | `12.095` | local encode lower |
| `encode_draw_cpu_ms_per_present` | `10.042` | `9.320` | local draw encode lower |
| `no_enqueue_wait_to_next_enqueue_ms_per_present` | `31.632` | `52.724` | worse |

The no-enqueue stage split explains the shift:
`commit_entry -> publish` falls `13.329 -> 0.610ms/present`, but
`publish -> encode_dequeue` rises `0.228 -> 18.407ms/present`. That rise is
consistent with CPU-ready staging deliberately holding published work until the
encoder can form a compatible group. It is not by itself a bug, but the total
pipeline result is not yet favorable because the hold does not translate into
better fixed-workload throughput.

## Comparison To Prior Run-Ahead Carriers

Relative to `runahead-coalesce-on-r2`, CPU-ready staging is a clear locality
refinement:

| Metric | Coalesce r2 | CPU-ready r1 | Direction |
|---|---:|---:|---|
| `command_buffers_per_present` | `19.156` | `5.741` | much lower |
| `sub_command_buffers_per_present` | `10.394` | `1.287` | much lower |
| `completion_wait_with_enqueue_ms_per_present` | `20.855` | `27.502` | overlap higher |
| `completion_wait_without_enqueue_ms_per_present` | `16.135` | `12.845` | lower |
| `completion_wait_ms_per_present` | `36.990` | `40.347` | worse |
| `commit_chunk_replay_cpu_ms_per_present` | `28.476` | `40.441` | worse |
| `encode_chunk_cpu_ms_per_present` | `14.490` | `12.095` | lower |

Relative to `singlecb-r1`, the same pattern holds:
`command_buffers_per_present` falls `14.686 -> 5.741` and
`sub_command_buffers_per_present` falls `5.931 -> 1.287`, but total completion
wait rises `36.861 -> 40.347ms/present` and commit replay rises
`28.261 -> 40.441ms/present`.

## Interpretation

The result splits the design question cleanly:

- The earlier critique was valid: mid-chunk splits and per-pass sub-CBs made
  coalescing mostly meaningless as a locality restoration path.
- CPU-ready staging plus stronger encode-side grouping moves the carrier toward
  the desired shape; it no longer explodes CBs by `4-5x`.
- The current bottleneck is not simply `command_buffers_per_present`.
  Bringing CBs near baseline does not raise FPS when replay/staging/pacing
  costs grow enough to keep total completion wait and wait-to-next-enqueue high.
- The severe `commit_chunk_replay_cpu_ms_per_present` increase is the next
  owner to split before another architecture bet. It may be real replay work
  accumulation, staging/coalescing bookkeeping, different completed-present
  denominator, or a measurement artifact from the changed pacing shape, but it
  is large enough that no FPS claim should proceed without attribution.

## Next Gate

Do not spend `.gputrace` on this exact shape. The next no-gputrace candidate
must first pass these gates:

| Gate | Required movement |
|---|---|
| Locality | move `command_buffers_per_present` closer to `3.999` without increasing pass count or tile preservation per present |
| Total wait | reduce `completion_wait_ms_per_present`, not only present wait |
| Replay/staging | split and reduce the new `commit_chunk_replay_cpu_ms_per_present` regression |
| Cadence | reduce `no_enqueue_wait_to_next_enqueue_ms_per_present` or prove better fixed-workload wallclock |
| FPS proof | rerun the baseline with `--frame-sampling` if the candidate claims sampled-FPS movement |
| Correctness | remove the black vertical artifact; keep `status=pass`, normal visual smoke, skipped/error/hazard/map/sequence wait counters clean |

The next implementation question is therefore narrower than "can run-ahead
work?" It is: can CPU-ready staging keep the improved CB/sub-CB shape while
avoiding the replay/staging and wait-to-next-enqueue regressions?
