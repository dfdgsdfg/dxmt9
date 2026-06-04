---
domain: backend-shape-classifiers
subcategory: texture
order: 01
title: Texture Source Classifier
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8505-L8622
---

# Texture Source Classifier

**Question / hypothesis.** Is fragment texture sampling the narrow source feature
behind the force-fragment-color movement or the hidden VS-buffer-write bucket?
The probe replaces fragment texture samples with `float4(1.0f)` while preserving
normal draw geometry and visible `VSOut`.

**Method.** Wrapper `--force-texture-white`, `--frame 60
--encoder-breakdown-seq 60 --dump-shaders`, finalized vs
`current-normal-gputrace-r1` (partial-log run; coverage/PSO/shader-dump gates
`9/9` passed).

**Result.**

| Metric | Baseline | Force texture white | Delta |
|---|---:|---:|---:|
| Total GPU time | `35.456ms` | `34.138ms` | `-3.72%` |
| Top-3 GPU time | `34.837ms` | `33.866ms` | `-2.79%` |
| Top-3 VS buffer write | `1627.240MiB` | `1574.470MiB` | `-3.24%` |
| Top-3 unexplained write | `1627.596MiB` | `1574.780MiB` | `-3.24%` |
| Top-3 named tiled buffer | `29.500MiB` | `20.750MiB` | `-29.66%` |
| Top-3 VS / VSOut | `7.868x` | `7.705x` | `-2.07%` |

The whole VS-write drop is concentrated in textured row `60/2` (`-52.814MiB`,
mostly a bytes/invocation effect); `60/1` and `60/0` stay flat (±0.03 MiB).

**Verdict.** Secondary, not the first-order owner. Fragment texture sampling is a
real contributor to the hot textured back-cull/scissor/alpha-blend pass (`60/2`),
but removing it only cuts top-3 GPU `2.79%` and leaves `1574.470MiB` VS write at
`7.705x` visible VSOut. Pass-specific source-shape sensitivity — not a general
write owner. The remaining primary owner is still hidden vertex/tiler/parameter
backend storage.

**Related.** [[backend-shape-classifiers]] · companion to [[backend-shape-classifiers-fog.01]] · refutes texture sampling as the [[hidden-backend-storage]] owner · refutes [[vsout-layout]] visible-width-alone.
