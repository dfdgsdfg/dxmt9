---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 181
title: EncodeSession PE Clear-Flush Boundary-Disabled Scout
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-pe-clear-flush-boundary-disabled-h181-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-pe-clear-flush-boundary-disabled-h181-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-pe-clear-flush-boundary-disabled-h181-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-pe-clear-flush-boundary-disabled-h181-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-boundary-disabled.180.md, docs/perfomance/present-pacing/present-pacing-pe-clear-flush.22.md
---

# Present-Pacing H181 - EncodeSession PE Clear-Flush Boundary-Disabled Scout

## Question

Does forcing the post-Present `Clear` chunk across the PE/unix bridge earlier
make the boundary-disabled EncodeSession path commit useful work during the
active completion wait?

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 DXMT9_PE_FLUSH_AFTER_CLEAR=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-pe-clear-flush-boundary-disabled-h181-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-draw-continuation-boundary-publish \
  --keep-frontmost
```

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is valid and visible:

- `status=pass`, `returncode=143`
- `present_encoded=1,020`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=9.699`
- `actual.png` has `53,647` unique RGB colors and `30.17%` non-black pixels at
  RGB `>=16`.

`DXMT9_PE_FLUSH_AFTER_CLEAR=1` reaches the runtime and creates `clear` chunks:
PE `pe_present_next_chunk reason=clear` has `1,061` samples, entry p50/p95
`18.731/32.505ms`, and bridge p50/p95 `14.856/33.145ms`.

It still does not open P4:

- `completion_wait_with_enqueue_ms=0.000`
- `completion_wait_enqueues_during_wait=0`
- `completion_wait_command_buffer_commit=0`
- `completion_wait_commit_chunk_entries=876`
- `completion_wait_commit_chunk_replay_starts=877`
- `completion_wait_commit_chunk_replay_ends=2`

Metal shape is effectively unchanged from H180:

- `command_buffers_per_present=4.006`
- `sub_command_buffers_per_present=2.993`
- `render_pass_begin_per_present=11.580`
- `completion_wait_ms_per_present=25.753`

## Interpretation

Early PE clear-flush is not enough. H180 already showed many commit-chunk
entries/replay starts during the active wait; H181 proves that making the
post-Present `Clear` cross as its own tiny chunk does not convert those entries
into encode dequeue or Metal command-buffer commit.

The blocker is therefore not simply "the PE bridge never sees work during
wait." The queue needs a semantic CPU-ready source that the carried
EncodeSession can release or append while preserving pass locality.
