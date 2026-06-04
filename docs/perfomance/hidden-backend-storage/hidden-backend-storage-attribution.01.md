---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: attribution
order: 01
title: Current Source Encoder Attribution
date: 2026-06-01
type: measurement
status: accepted
source: specs/perfomance.plan.md#L2093-L2212
---

# Current Source Encoder Attribution

**Question / hypothesis.** With the current normal perf profile (no
correctness-invalid toggles), how is the top-three render-encoder cost
attributed between explicit dxmt CPU writers, visible MSL `VSOut`, and the
hidden Apple backend bucket? This is the authoritative reference baseline.

**Method.** `app-d3d9-3dmark05-current-normal-gputrace-r1` (normal perf
profile, visually valid GT1). Captured `frame60.gputrace`, exported with
embedded performance data, Performance > Counters, waited for draw-counter
profiling, exported encoder counters, then `finalize_3dmark05_perf_probe.sh`
with `DXMT9_PERF_ENCODER_BREAKDOWN=1` join and the Xcode-counter / dxmt-join /
top-PSO / shader-dump gates enabled. Join key `RenderPass[seq=...,enc=...]`.

**Result (frame60).**

| Metric | Value |
|---|---:|
| Total GPU | `35.456ms` |
| Top-3 GPU / share | `34.837ms` / `98.25%` |
| Top-3 buffer write | `1628.040MiB` |
| Top-3 VS buffer write | `1627.240MiB` |
| VS buffer / Xcode buffer write | `1.000x` |
| VS buffer / expected VSOut | `7.9x` |
| VS buffer bytes / VS invocation | `1447.7B` |
| Named tiled buffer total | `29.500MiB` |
| Hidden backend write estimate | `1597.296MiB` |
| Hidden backend / VS buffer write | `0.982x` |
| dxmt CPU writer bytes | `0.444MiB` |
| Unexplained Xcode buffer write | `1627.596MiB` (ratio `1.000`) |
| dxmt draws / triangles | `385` / `715,395` |
| Backend storage class | `hidden_vertex_tiler_parameter_storage:3` |

Shader-dump join matched `9/9` nonzero top rows. Hot VS rows emit `184B`
visible `VSOut`; paired FS reads only a subset, leaving `71.7%-80.4%` of
visible `VSOut` unread. Per row: `60/2` `20.028ms`/`981.185MiB`,
`60/1` `9.061ms`/`421.124MiB`, `60/0` `5.748ms`/`224.931MiB`.

**Verdict.** accepted (baseline). The normal source is not blocked by explicit
dxmt writers (~four orders of magnitude too small), transient uploads, stream
input volume, or ordinary `VSOut` width. First-order owner is the hidden
Apple vertex/tiler/parameter bucket; unexplained ratio `1.000`. This is the
authoritative A/B baseline. The companion `DXMT9_PROBE_DISABLE_ALPHA_BLEND=1`
run was correctness-invalid (solid yellow) and left VS write at
`1627.268MiB` (`+0.00%`) while GPU regressed `+1.72%` — not a baseline.

**Related.** [[hidden-backend-storage]] · [[hidden-backend-storage-model.01]] ·
[[hidden-backend-storage-density.01]] · [[baselines]] · [[vsout-layout]] ·
[[backend-shape-classifiers]] · [[overview-3dmark05-gt1]]
