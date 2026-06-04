---
domain: const-upload
subcategory: sparse
order: 02
title: Sparse Const Split Xcode Validation
date: 2026-06-01
type: validation
status: rejected
source: specs/perfomance.plan.md#L6166-L6252
---

# Sparse Const Split Xcode Validation

**Question / hypothesis.** Re-run the sparse-const candidate
([[const-upload-sparse.01]]) with a same-frame `.gputrace` + exported Xcode
encoder counters: does the ~`31%` const-upload byte reduction move the primary
GPU bottleneck (`VS Buffer Device Memory Bytes Written`)?

**Method.** `run_3dmark05_perf_probe.sh --suffix split-sparse-const-gputrace-r2
--frame 60 --timeout 180 --split-sparse-const-records --compare-baseline-output
.../basevertex-instrument-base-r1 --baseline-joined
.../draw-size-gputrace-r1/.../frame60-xcode-dxmt-joined-summary.csv
--require-binding-overrides-present --require-draw-submission-batch-present
--require-const-upload-passthrough-present --require-const-upload-break-bytes-decrease
--max-const-upload-break-count-ratio 1.02`. Output:
`experiments/output/app-d3d9-3dmark05-split-sparse-const-gputrace-r2/` and
`traces/app-d3d9-3dmark05-split-sparse-const-gputrace-r2/analysis/`.

**Result.** Mechanism confirmed: `const_upload_break_bytes_per_draw`
`275.977→190.393` (`-31.01%`), `const_upload_passthrough_per_draw` `+21.00%`;
but `draw_run_records_per_submit` flat (`4.115→4.113`) and
`draw_submission_batch_records_per_submit` regressed (`9.212→8.900`). Xcode:
total GPU `35.261→33.902ms`; top-three GPU `34.737→33.373ms`;
**top-three VS buffer write `1627.395→1627.316MiB`** (`-0.079MiB`); top
unexplained/buffer-write ratio `1.000x` unchanged; top dxmt CPU writer `0.444MiB`,
top stream/IB handle changes `437`/`326` all unchanged.

**Verdict.** Rejected (as primary GPU fix). The `~1.627GiB` VS buffer-write bucket
is untouched; the ~`-3.9%` top-frame GPU-time move is run noise / scheduling
variance since the dominant memory-write counter and top encoder shape are
identical. A valid CPU payload-volume reduction only. Primary GT1 limiter remains
GPU-side vertex-stage/internal buffer-write pressure.

**Related.** [[const-upload]] · prev: [[const-upload-sparse.01]] · the surviving
GPU owner → [[hidden-backend-storage]] · [[tvb-mechanism-proof]] (the VS-write
bucket it leaves intact) · [[state-churn-encode]] (coupled stream/IB churn).
