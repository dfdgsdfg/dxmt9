---
domain: const-upload
workload: 3DMark05 GT1
subcategory: slice
order: 01
title: FFP VS Stable Slice Reuse Run
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L4398-L4473
---

# FFP VS Stable Slice Reuse Run

**Question / hypothesis.** Because `FfpVsConsts` is ~100% unchanged inside an
encoder ([const-upload-volatility.01](const-upload-volatility.01.md)), defer it out of the generic argbuf
dirty mirror, compare against an encoder-local cached host copy, and — when the
bytes are unchanged — repoint the fresh argbuf table at the existing low-level
FFP-VS slice instead of uploading another transient copy.

**Method.** FFP-VS cbuf built after the pre-transformed viewport override, then
byte-compared with the encoder-local cache; on match, argbuf `id(1)` points at
the cached slice. GT1 run, passed image capture, same `1260`-present window.
Output: `experiments/output/app-d3d9-3dmark05-ffpvs-cache/{dxmt9.log,result.json,ffpvs-cache-summary.md}`.
Baseline = field-volatility run.

**Result.** vs field baseline: `argbuf_hybrid_bytes_per_encoder`
`4584324456→3177699416` (`-30.68%`), `transient_upload_bytes` `-25.13%`,
`transient_upload_cpu_ms` `4667.523→3254.799` (`-30.27%`), `encode_draw_cpu_ms`
`-15.76%`. `gpu_command_buffer_time_ms` `3634.590→3643.395` (same class).
Cbuf class: total `-31.13%`; **FFP VS `1455604720→31437480` (`-97.84%`)**; VS
`-0.42%`, PS `-0.50%`. FFP-VS rewrite-unchanged bytes collapsed
`1423999695→2055`, removing ~`1.42GB` of repeated unchanged cbuf writes.

**Verdict.** Accepted (CPU/upload win). The targeted ~`1.42GB` FFP-VS bucket
disappears, but `gpu_command_buffer_time_ms` does not move — FFP-VS cbuf was a
real CPU/upload amplifier, NOT the GPU limiter. Active target shifts to
`VsConsts` unchanged float-prefix bytes.

**Related.** [const-upload](../const-upload.md) · prev: [const-upload-volatility.01](const-upload-volatility.01.md) · next:
[const-upload-range.01](const-upload-range.01.md) · confirms GPU limiter is elsewhere →
[hidden-backend-storage](../hidden-backend-storage.md) · [state-churn-encode](../state-churn-encode.md) (stream/IB churn unchanged class).
