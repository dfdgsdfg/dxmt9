---
domain: const-upload
workload: 3DMark05 GT1
subcategory: sparse
order: 01
title: Sparse Const Split Run-Level Probe
date: 2026-06-01
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L6078-L6165
---

# Sparse Const Split Run-Level Probe

**Question / hypothesis.** Splitting one merged min/max const-upload record into
the actual changed-register runs should cut const-upload break payload bytes
without exploding the const-upload break *count* — and ideally let the draw-run
scanner cross more const records.

**Method.** `run_3dmark05_perf_probe.sh --suffix split-sparse-const-frame60-r1
--frame 60 --no-gputrace --split-sparse-const-records --min-free-mb 64` (env
`DXMT9_SPLIT_SPARSE_CONST_RECORDS=1`). Run-level gate vs baseline
`current-source-frame60-r3` via `compare_3dmark05_perf_counters.py
--require-const-upload-break-bytes-decrease --max-const-upload-break-count-ratio
1.02 --require-const-upload-passthrough-present`. Output:
`experiments/output/app-d3d9-3dmark05-split-sparse-const-frame60-r1/`.

**Result.** `const_upload_break_bytes` `278141376→192150320` (`-30.92%`),
`const_upload_break_registers` `-30.92%`, `const_vs_f_bytes` `-30.90%`,
`const_ps_f_bytes` `-31.82%`; `const_upload_break_count` `652335→653190`
(`+0.13%`, essentially flat); `const_upload_passthrough` `+21.99%`;
`draw_run_records_per_submit` `4.131→4.102` (unchanged). No gputrace, so no GPU
frame impact measured here.

**Verdict.** Accepted (CPU mechanism). It does what it was designed to do —
~`31%` fewer const-upload payload bytes/registers without inflating the break
count. But the break count is unchanged, so it does NOT make the scanner cross
const records, and draw-run coverage did not improve. A byte-volume reduction,
not a batching fix; GPU impact still unproven (needs Xcode).

**Related.** [const-upload](../const-upload.md) · next: [const-upload-sparse.02](const-upload-sparse.02.md) (the Xcode
validation that rejects it as a GPU fix) · [state-churn-encode](../state-churn-encode.md) (the draw-run
batching it does not fix) · [hidden-backend-storage](../hidden-backend-storage.md).
