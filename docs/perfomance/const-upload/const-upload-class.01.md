---
domain: const-upload
workload: 3DMark05 GT1
subcategory: class
order: 01
title: Cbuf Class Breakdown Run
date: undated
type: measurement
status: model
source: specs/perfomance.plan.md#L4232-L4313
---

# Cbuf Class Breakdown Run

**Question / hypothesis.** Where does the multi-GB argbuf constant-buffer write
traffic actually go? Split `argbuf_cbuf_bytes` by the four argbuf cbuf entries
(VS, FFP-VS, PS, FFP-PS) to find the dominant write bucket before optimizing.

**Method.** GT1 run with `DXMT9_PERF_ENCODER_BREAKDOWN=1` cbuf class
attribution. Output:
`experiments/output/app-d3d9-3dmark05-cbuf-class-breakdown/{dxmt9.log,result.json,cbuf-class-breakdown-summary.md}`.
Same `1260`-present GT1 window as the prior encoder-stream run.

**Result.** Run-shape counters: `draw_calls=913869`, `render_pass_begin=14695`,
`argbuf_hybrid_bytes_per_encoder=4582153064`, `transient_upload_bytes=5625485036`
(`4629.911ms` CPU), `gpu_command_buffer_time_ms=3711.844`,
`completion_wait_ms=24892.872`. Encoder-attributed cbuf class split
(total `4617491264` = 100%):

| Class | Bytes | Share |
|---|---:|---:|
| VS | `2358862880` | `51.085%` |
| FFP VS | `1455155280` | `31.514%` |
| PS | `539897808` | `11.692%` |
| FFP PS | `263575296` | `5.708%` |

Vertex-side (VS + FFP-VS) = `82.60%` of cbuf writes; pixel-side = `17.4%`. Top
encoder ordinals: `1` (`1.07GB`), `4` (`856MB`), `0` (`849MB`).

**Verdict.** Model. The cbuf bucket is overwhelmingly vertex-side, not pixel
constants. "Skip PS constants" is the wrong first fix; the next split must find
which fields inside `VsConsts`/`FfpVsConsts` are actually volatile per draw.

**Related.** [const-upload](../const-upload.md) · next: [const-upload-volatility.01](const-upload-volatility.01.md) ·
[state-churn-encode](../state-churn-encode.md) (stream/IB churn measured in the same run) ·
[hidden-backend-storage](../hidden-backend-storage.md) (the GPU bucket this CPU traffic is not).
