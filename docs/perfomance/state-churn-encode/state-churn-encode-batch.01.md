---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: batch
order: 01
title: Current HEAD Recheck After Submission-Batch Work
date: 2026-06-02
type: validation
status: rejected
source: specs/perfomance.plan.md#L8624-L8745
---

# Current HEAD Recheck After Submission-Batch Work

**Question / hypothesis.** After the draw-submission batching and binding-override
work landed, do the new CPU-side batching structures change the Xcode GPU
counters or the hidden VS-buffer-write owner at frame60?

**Method.** `current-head-gputrace-r1`:
`scripts/tools/run_3dmark05_perf_probe.sh --suffix current-head-gputrace-r1
--frame 60 --encoder-breakdown-seq 60 --timeout 180 --dump-shaders
--baseline-joined <current-normal-gputrace-r1 joined.csv>
--compare-baseline-output <current-normal-gputrace-r1>
--require-draw-submission-batch-present`. Partial-log capture (gputrace exported;
run-level json comparison skipped). Frame replayed normal GT1 image.

**Result.** Xcode: `35.42ms` GPU, 4 CBs, 10 encoders, 396 draws. Frame-level vs
baseline: total GPU `35.456 -> 35.416ms` (-0.11%), top-three buffer write
`1628.040 -> 1628.046 MiB` (+0.00%), top-three **VS buffer write `1627.240 ->
1627.315 MiB` (+0.00%)**, dxmt CPU writer bytes `0.444 MiB` unchanged, stream/IB
handle changes `437`/`326` unchanged. CPU batching is active:
`draw_run_submits=79,946`, `binding_override_records=249,506`,
`submission_batch_submits=73,070`, `submission_batch_records=674,389`,
`draw_submission_batch_records_per_submit=9.229`,
`backend_draw_run_batch_records_per_group=1.879`,
`const_upload_passthrough=769,688`, `encode_draw_cpu_ms=21011.956`.

**Verdict.** Rejected (as a GPU fix). CPU batching is live (avg 9.229
records/group) but does not move the GT1 frame60 GPU limiter — VS buffer write
stays at ~`1627.3 MiB`, almost entirely unexplained by dxmt CPU writers and far
above the visible `184B` VSOut width. Submission batching is a CPU-side project.

**Related.** [state-churn-encode](index.md) · [state-churn-encode-binding.01](state-churn-encode-binding.01.md) (the
override CPU win) · [state-churn-encode-encoder.03](state-churn-encode-encoder.03.md) (same-frame join) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (the unmoved GPU owner) ·
[index-cache-locality](../index-cache-locality/index.md) (the accepted GPU-side win path).
