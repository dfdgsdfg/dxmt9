---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 162
title: EncodeSession Writer-Active CpuReady Publish
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-writer-active-cpuready-publish-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-cpuready-publish-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-cpuready-publish-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-cpuready-publish-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-writer-active-slot-shape.161.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H162 - EncodeSession Writer-Active CpuReady Publish

## Question

H161 showed that the dominant writer-active/no-wait blocker usually has a small
non-present writing-slot source already available. If the encode thread cuts
that current writing slot as a `SemanticBoundary` source while a tail-less
`EncodeSession` is waiting, does it create useful wait-window overlap without
breaking visual correctness or command-buffer/pass locality?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-writer-active-cpuready-publish-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-writer-active-cpu-ready-publish
```

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- `present_encoded=840`
- output image shows normal GT1 battlefield content (`mean_luma=72.922`,
  `variance=5239.346`)
- no giant vertical black artifact and no first-frame freeze
- no `D3DERR`, `INVALIDCALL`, `DXMT_ASSERT`, fatal, or abort strings in the
  output logs
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`

The new source-publication path is active:

- `chunk_publish_reason_semantic_boundary=3408` (`4.057/present`), up from
  H161's `1722` (`1.794/present`)
- `open_cb_tail_present_head_appended=2567`, up from H161's `872`
- `encode_session_carry_deferred_active_render_chunks=3394`
- `chunk_publish_reason_present_split_before=0`
- `chunk_subcb_count_max=4`

But useful overlap does not improve:

- semantic release submissions fall from H161 `149` to `83`
- completion-wait command-buffer commits fall from H161 `148` to `83`
- enqueues during completion wait fall from H161 `148` to `83`
- completion wait with enqueue falls from H161 `4.604ms/present` to
  `3.192ms/present`
- completion wait without enqueue rises from H161 `15.887ms/present` to
  `16.418ms/present`
- ready-source/no-wait blocks rise from H161 `187` to `2065`
- writer-active/no-wait blocks rise from H161 `1380` to `3078`
- writer-active empty-slot misses appear at `1254`; H161 had `0`

Locality is not catastrophically broken, but it is not a promotion:

- command buffers: H161 `4.158/present`, H162 `4.104/present`
- sub-command buffers: H161 `2.998/present`, H162 `2.999/present`
- render-pass begins: H161 `10.354/present`, H162 `10.432/present`
- GPU command-buffer time: H161 `2.476ms/present`, H162 `2.305ms/present`

## Interpretation

H162 confirms the writer-active CPU-ready cut is mechanically safe enough for a
default-off diagnostic: it can publish extra non-present sources, append them
through the carried `EncodeSession`, and keep ordered completion without visual
or GPU errors in this smoke.

It is still the wrong promotion policy. Cutting the writing slot from the encode
thread after observing a no-wait miss mostly creates more ready-source append
work outside the useful completion-wait window. The old empty-ready/no-wait
blocker turns into ready-source/no-wait and empty-writer-slot churn, while the
same-window commit/enqueue count drops. This means the cut is happening after
the relevant window has already been missed, not early enough to raise the FPS
ceiling.

Keep `DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH` default-off. The remaining
production direction is still R-BACK-2.40/R-BACK-2.43 in its stronger form:
make CPU-ready/source boundaries deterministic and earlier in the producer
pipeline, or merge the logical source tape before encode, while preserving the
open render encoder across those source boundaries. An encode-thread reactive
cut is useful evidence, not the final structure.
