---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: fog
order: 01
title: Fog Source Classifier
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8398-L8503
---

# Fog Source Classifier

**Question / hypothesis.** Do fog-factor reads or the generated fog blend path
explain the force-fragment movement or the hidden VS-buffer-write bucket?

**Method.** Wrapper `--disable-fog`, `--frame 60 --encoder-breakdown-seq 60
--dump-shaders`, finalized vs `current-normal-gputrace-r1`. Finalized from a
`partial-log` run (`result.json` not written before termination); Xcode/dxmt
joined comparison, counter coverage, top-PSO attribution, and shader-dump match
gates (`9/9` VS, `9/9` PS) all passed.

**Result.**

| Metric | Baseline | Disable fog | Delta |
|---|---:|---:|---:|
| Total GPU time | `35.456ms` | `34.506ms` | `-2.68%` |
| Top-3 GPU time | `34.837ms` | `33.933ms` | `-2.59%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.294MiB` | `+0.00%` |
| Top-3 named tiled buffer | `29.500MiB` | `28.031MiB` | `-4.98%` |
| Top-3 FS buffer write | `0.800MiB` | `0.717MiB` | `-10.31%` |

VS invocations, VS B/invocation, and VSOut key (`0xfff`) fixed in all hot rows.

**Verdict.** Secondary, not the owner. Disabling fog removes a little
fragment/raster work and improves GPU time ~2.7%, but does not move the dominant
VS-write bucket, the hidden backend estimate, VS invocation count, or VS bytes
per invocation. Fog-factor reads and the fog blend path are a secondary
fragment/raster cost, not the ~1.63 GiB hidden vertex/backend write owner.

**Related.** [backend-shape-classifiers](index.md) · companion to [backend-shape-classifiers-texture.01](backend-shape-classifiers-texture.01.md) (the other secondary fragment-cost classifier) · refutes fog as the [hidden-backend-storage](../hidden-backend-storage/index.md) owner · refutes [vsout-layout](../vsout-layout/index.md) fogFactor width as the owner.
