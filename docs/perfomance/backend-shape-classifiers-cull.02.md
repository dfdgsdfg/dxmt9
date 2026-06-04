---
domain: backend-shape-classifiers
subcategory: cull
order: 02
title: Cull State Classifier
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8301-L8397
---

# Cull State Classifier

**Question / hypothesis.** Full Xcode-counter capture: does the top hidden
VS-buffer-write bucket track cull/primitive backend shape? Crucially, separates
the *small named tiled-buffer counters* from the *large hidden VS-write bucket*.

**Method.** `DXMT_DISABLE_CULL=1` (wrapper `--disable-cull`), `--frame 60
--encoder-breakdown-seq 60 --dump-shaders`, finalized vs
`current-normal-gputrace-r1` with full coverage/PSO/shader-dump gates
(`--min-top-pso-samples-per-draw 0.90 --min-top-dxmt-joined-fraction 1.0`).
Shader dump matched `9/9` VS and `9/9` PS rows.

**Result.**

| Metric | Baseline | Disable cull | Delta |
|---|---:|---:|---:|
| Total GPU time | `35.456ms` | `36.120ms` | `+1.87%` |
| Top-3 GPU time | `34.837ms` | `35.478ms` | `+1.84%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.233MiB` | `-0.00%` |
| Top-3 named tiled buffer | `29.500MiB` | `59.531MiB` | `+101.80%` |
| Top-3 cull-unit limiter | `5.917%` | `12.084%` | `+104.22%` |
| Top-3 clip-unit limiter | `2.126%` | `4.285%` | `+101.56%` |

VS invocations and VS B/invocation stayed fixed in all hot rows (VSOut key `0xfff`).

**Verdict.** Rejected as owner. The probe is clearly *active* (named
tiler/cull/clip counters roughly double), yet `VS Buffer Device Memory Bytes
Written`, VS invocations, and the hidden backend estimate are unchanged. This
cleanly separates the small named tiled counters (~30 MiB) from the much larger
hidden bucket (~1627 MiB) — roughly **55x** apart. Named tiler counters are a
classifier, not the optimization target.

**Related.** [[backend-shape-classifiers]] · expands [[backend-shape-classifiers-cull.01]], precedes [[backend-shape-classifiers-cull.03]] and [[backend-shape-classifiers-cull.04]] · key evidence for [[hidden-backend-storage]] (named tiled ≠ hidden bucket).
