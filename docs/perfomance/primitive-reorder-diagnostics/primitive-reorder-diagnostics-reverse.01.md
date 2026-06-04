---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 01
title: Reverse Indexed Triangle-Order Classifier
date: 2026-06-02
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L9242-L9383
---

# Reverse Indexed Triangle-Order Classifier

**Question / hypothesis.** Does perturbing primitive/index *order* (not vertex
expansion) move the hidden VS-buffer-write bucket? The probe keeps Metal indexed
draws enabled (`draw_expanded_indexed=0`) and replaces each triangle-list index
buffer with a transient IB whose triangle order is reversed but per-triangle
winding is preserved.

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-indexed-triangles-gputrace-r1
--frame 60 --encoder-breakdown-seq 60 --probe-reverse-indexed-triangles
--measure-index-reuse --top 4 --hot-gpu-share 95 --baseline-joined <measure-index-cache>
--require-xcode-counter-coverage --require-dxmt-join-coverage --require-top-pso-attribution`.
Standard Xcode replay/export/finalizer. Baseline: `measure-index-cache-gputrace-r1`.

**Result.** GPU `34.391 -> 26.346ms` (`-23.39%`); hot/top GPU `-24.73%`; hot/top
VS buffer write `1472.747 -> 1036.222MiB` (`-29.64%`); hot/top unexplained write
`-29.93%`; hot/top VS bytes/inv `856.265 -> 735.415B` (`-14.11%`); FS tiles
`-34.21%`; texture write `-34.21%`; depth write `-36.55%`. Probe applied to
704/704 draws, `5.47MiB` run-level reorder bytes. dxmt CPU writer was only
`0.005x` of Xcode buffer write. Hidden backend estimate `1013.254MiB`
(`0.978x` of VS write).

**Verdict.** Inconclusive / strong classifier, NOT a clean optimization. The
hot-row set *changed* from `60/0,60/1,60/3,60/4` to `60/0,60/1,60/2,60/9`;
draws `-14.49%`, vertices/triangles `-18.80%` (see [[primitive-reorder-diagnostics-reverse.04]]
shape-gate reclassification, which rejects this as a same-frame proof). The big
aggregate drop comes from a different, lighter frame shape, not a per-row win.
Texture/depth/FS-tile drops show visibility/overdraw also changed. Confirms
primitive order can move hidden Apple vertex/tiler/backend traffic, motivating
the scoped subset probes that follow.

**Related.** [[primitive-reorder-diagnostics]] · next: [[primitive-reorder-diagnostics-reverse.02]]
· [[hidden-backend-storage]] (confirms order moves the TVB bucket) ·
[[index-reuse-measurement]] (cache64/reuse model) · [[baselines]] (measure-index-cache).
