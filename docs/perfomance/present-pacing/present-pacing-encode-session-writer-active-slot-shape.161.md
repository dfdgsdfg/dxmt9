---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 161
title: EncodeSession Writer-Active Slot Shape
date: 2026-06-21
type: no-gputrace
status: diagnostic-observed-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-shape-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-shape-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-shape-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-shape-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-writer-active-slot-state.160.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H161 - EncodeSession Writer-Active Slot Shape

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

H160 showed that every dominant empty-ready/no-wait semantic-release miss
already has non-present work in the active writing slot. Is that slot carrying
enough work to justify a CPU-ready source boundary, and what kind of boundary
should the next carrier try?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-writer-active-slot-shape-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish
```

## Verdict

Diagnostic observed, runtime promotion rejected.

The run is correctness-safe:

- `status=pass`
- `present_encoded=960`
- output image shows normal GT1 content (`mean_luma=75.813`,
  `variance=5448.090`)
- no giant vertical black artifact
- no `D3DERR`, `INVALIDCALL`, `DXMT_ASSERT`, fatal, or abort strings in the
  output logs
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`

The writer-active slot-shape counters match the H160 blocker:

- writer-active/no-wait/non-present slot-shape samples: `1380`
- total commands in sampled slots: `19815`
- average commands per sampled slot: `14.359`
- maximum commands in one sampled slot: `67`
- draw-run commands: `18435`
- average draw-run commands per sampled slot: `13.359`
- draw items: `48382`
- average draw items per sampled slot: `35.059`
- maximum draw items in one sampled slot: `159`
- non-draw commands: `1380`
- average non-draw commands per sampled slot: `1.000`
- payload bytes: `11191320`
- average payload bytes per sampled slot: `8109.652`
- maximum payload bytes in one sampled slot: `39480`

The broader open-CB shape remains diagnostic, not promotable:

- `command_buffers=3992` (`4.158/present`)
- `sub_command_buffers=2878` (`2.998/present`)
- `render_pass_begin=9940` (`10.354/present`)
- `chunk_subcb_count_max=4`
- `open_cb_tail_present_semantic_release_submitted=149`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active=1380`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_nonpresent=1380`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_empty=0`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_present=0`
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=187`
- `completion_wait_ms=19672.195` (`20.492ms/present`)
- `completion_wait_with_enqueue_ms=4420.318` (`4.604ms/present`)
- `completion_wait_without_enqueue_ms=15251.877` (`15.887ms/present`)

## Interpretation

H161 narrows H160's owner. The writer-active slot is not empty and not a
whole-frame backlog. It is usually a small semantic unit: about fourteen
commands, one non-draw boundary command, thirty-five draw items, and eight KiB
of payload at the sampled miss points.

Publishing those units directly as ordinary encode-visible chunks would likely
add roughly one or two extra source units per present and repeat the known
fragmentation class unless the source boundary remains metadata-only from the
Metal encoder's point of view. The viable next carrier is therefore a
CPU-ready/session boundary that lets the existing `EncodeSession` append the
source while preserving the open render encoder, not an arbitrary draw-count
cutoff and not a broader deterministic release policy.

This evidence justifies a default-off H162 probe:
`DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH=1` may cut the current
writer-active non-present slot as `chunk_publish_reason_semantic_boundary` only
while a tail-less semantic-boundary pending session is waiting with no ready
source and no active completion wait. Promotion still requires the R-BACK-2.50
gates: visual safety, ordered completion, no inline completion of visible work,
and no command-buffer/render-pass/tile/load-store regression.
