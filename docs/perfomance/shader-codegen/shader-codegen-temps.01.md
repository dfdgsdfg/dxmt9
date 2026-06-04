---
domain: shader-codegen
workload: 3DMark05 GT1
subcategory: temps
order: 01
title: Vertex Temp Array Trim Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L3554-L3600
---

# Vertex Temp Array Trim Probe

**Question / hypothesis.** Does the conservative 32-slot `float4 r[]` temp array
emitted by translated vertex shaders inflate the hidden Xcode
`VS Buffer Device Memory Bytes Written` bucket? If so, sizing `r[]` from observed
temp usage should reduce VS write traffic.

**Method.** `DXMT9_TRIM_VERTEX_TEMPS=1` sizes `float4 r[]` from
`collectConstantUsage().maxTempIndex + 1` instead of the default 32-slot array;
the flag is in the shader source debug-env key so the PSO/source cache cannot
serve a stale source across the A/B. Run via the GT1 wrapper
`scripts/tools/run_3dmark05_perf_probe.sh --trim-vertex-temps` (frame 60, paired
`--dump-shaders` + gputrace). Finalizer gates:
`--require-top-vs-buffer-write-decrease` and
`--max-top-unexplained-buffer-write-ratio 0.50`.
Candidate run `app-d3d9-3dmark05-trim-varyings-vertex-temps-frame60-r1`.

**Result.**
- MSL shape did change: top translated VS rows now emit `float4 r[1]`, with
  `VS temp span` `0/1/0` and no dynamic or relative temp access.
- Xcode counter did not move: `top_vs_buffer_write_mib` `1627.321` → `1627.325`
  MiB; `top_unexplained_buffer_write_ratio` stayed `1.000`.
- Total GPU time improved `34.719ms` → `33.545ms` (`-3.38%`), but with no
  corresponding VS-write reduction, so this is not proof that temp trimming
  touches the dominant bottleneck.

**Verdict.** Rejected. Source-visible `r[]` temp width is not the owner of the
~1.627 GiB VS buffer-write bucket; the finalizer rejected the candidate on
`--require-top-vs-buffer-write-decrease`. Confirms the central finding that the
bucket is hidden backend storage below the visible MSL, not a translated temp
array.

**Related.** [[shader-codegen]] · [[shader-codegen-scratch.01]] (the immediate
follow-up scratch probe) · [[hidden-backend-storage]] · [[vsout-layout]]
