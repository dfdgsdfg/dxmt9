---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: alphatest
order: 01
title: Alpha-Test Discard Classifier
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8077-L8200
---

# Alpha-Test Discard Classifier

**Question / hypothesis.** The force-fragment-color probe moved `seq=60 enc=2`
VS invocations, clip/tiled counters, and VS writes while keeping VSOut key
`0xfff`. The smallest separation point is the fragment alpha-test
`discard_fragment()` path: does stripping just the alpha-test discard reproduce
that movement or own the ~1.63 GiB VS-write bucket?

**Method.** `DXMT_DISABLE_ALPHA_TEST=1` (wrapper `--disable-alpha-test`) keeps
the normal FS body and texture sampling but strips the generated alpha-test
`discard_fragment()` branch from FFP/translated source, mixes into the shader
debug-env key, and sets `FfpPsConsts.alphaTestEnable = 0`. `--frame 60
--encoder-breakdown-seq 60 --dump-shaders`, finalized vs
`current-normal-gputrace-r1` with full coverage/PSO/shader-dump gates.

**Result.**

| Metric | Normal | Disable alpha-test | Delta |
|---|---:|---:|---:|
| Total GPU | `35.456ms` | `36.010ms` | `+1.56%` |
| Top-3 GPU | `34.837ms` | `35.438ms` | `+1.72%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.268MiB` | `+0.00%` |
| Top-3 VS B / invocation | `1447.741B` | `1447.766B` | `+0.00%` |
| Top-3 alpha blend/test/eff-test draws | `145/0/0` | `145/0/0` | unchanged |

Shader dump: hot FS source hashes changed for `60/1`/`60/2` (strip path active),
`alpha_test_effective_draws = 0`, no `discard_fragment()` in generated source. VS
invocations and VSOut key (`0xfff`) fixed.

**Verdict.** Rejected. Disabling alpha-test changed PS source hashes but left VS
write, VS invocations, named tiled counters, and the hidden estimate stable — it
does not reproduce the force-fragment `seq=60 enc=2` movement. Alpha-test discard
is not the owner of either the force-fragment delta or the ~1.63 GiB bucket; the
force-fragment movement is tied to broader fragment/raster backend shape.

**Related.** [[backend-shape-classifiers]] · separates from [[backend-shape-classifiers-alpha.01]] (blend vs test) · refers forward to [[backend-shape-classifiers-fog.01]] / [[backend-shape-classifiers-texture.01]] as the broader-FS classifiers · confirms [[hidden-backend-storage]].
