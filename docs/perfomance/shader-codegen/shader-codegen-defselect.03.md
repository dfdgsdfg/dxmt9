---
domain: shader-codegen
workload: 3DMark05 GT3 and SFIV Benchmark
subcategory: defselect
order: 03
title: GT3 And SFIV Visual Gates Pass; SFIV Is Unaffected
date: 2026-07-29
type: experiment-run
status: accepted-visual-gate
source: traces/defsel-visual-gt3/cand; traces/defsel-visual-gt3/glitch; traces/defsel-visual-sfiv/cand; experiments/output/app-d3d9-sfiv-benchmark-defsel-perf-base; experiments/output/app-d3d9-sfiv-benchmark-defsel-perf-cand; experiments/output/app-d3d9-3dmark05-defsel-gt3-cand
related: docs/perfomance/shader-codegen/shader-codegen-defselect.01.md; docs/perfomance/shader-codegen/shader-codegen-defselect.02.md
---

# GT3 And SFIV Visual Gates Pass; SFIV Is Unaffected

**Question / hypothesis.** `d63f7a65` changes the D3D9→MSL translator, so it
affects every app, but [defselect.01](shader-codegen-defselect.01.md) and
[.02](shader-codegen-defselect.02.md) validated only GT1 and GT2. The first
attempt at this change (`959c848c`, reverted) made every skinned character in
GT1 vanish while leaving the environment intact, so the specific risk is
skinned geometry. GT3 has crew figures; SFIV is a fighting game whose entire
subject is skinned characters. Do they render, and does SFIV's performance move?

**Method.** Visual gates on the candidate build. GT3: two capture ranges,
frames `300..1500` step `200` and `2200..2600` step `100`, the second chosen to
cover the `1:06-1:08` window `agents/rules/metal_debugging.rules.md` flags as
visually sensitive. SFIV: frames `200..1100` step `300`. SFIV performance was
then measured separately, without frame capture, on both the candidate and a
rebuilt `959c848c^` baseline, using `DXMT9_PERF_FRAME_SAMPLING=1`.

**Result — GT3.** All twelve captured frames render correctly: airship, canyon,
deck detail, water. **Crew figures are present and correctly skinned** — two
uniformed figures at the railing at `0:32.99`, one beside the orrery at
`0:43.94`. The `1:06-1:08` window is covered by frames at `1:01.56`, `1:04.32`,
`1:07.76`, `1:10.86`, `1:13.62` and shows no corruption. Scene rate `37.12 fps`
over `2,659` samples.

**Result — SFIV.** The world-map intro and both character scenes render
correctly: **Ryu and Sakura's models are fully intact** — faces, hands, gloves,
and clothing all deform properly. These are precisely the models the reverted
attempt would have removed.

| SFIV lane | samples | median frame | fps |
|---|---:|---:|---:|
| baseline (`959c848c^`) | `1,393` | `88.76 ms` | `11.27` |
| candidate (`d63f7a65`) | `1,357` | `87.74 ms` | `11.40` |

`+1.2%`, inside noise. SFIV is unaffected, which is consistent with its cause
already having been fixed for its *pixel* shaders by H226 (`7abaa20e`); its
vertex shaders evidently do not combine relative constant addressing with a DEF.

**Verdict.** ACCEPTED. All four workloads are now validated: GT1 and GT2 gained
(`+8.8%` and `+110%` scene fps), GT3 and SFIV render correctly with no
regression. The skinned-geometry failure mode that the first attempt introduced
does not appear on any of them.

**An unrelated discrepancy this surfaced, not caused by this change.**
`docs/perfomance/overview-sfiv.md` records `45.416` sampled fps for SFIV over a
`124.493 s` run. Both lanes here measure `~11.3 fps` over a `90 s` run, and
SFIV's own on-screen counter agrees (`AVERAGE: 10.68`). Since baseline and
candidate agree with each other, this fix is not the cause. Whether the gap is
run-length and scene coverage, or a real regression from some other change, is
open and needs its own investigation — do not read `45.416` as the current SFIV
figure without re-measuring it.

**Related.** [shader-codegen-defselect.01](shader-codegen-defselect.01.md) ·
[shader-codegen-defselect.02](shader-codegen-defselect.02.md) ·
[shader-codegen](index.md) · [overview-sfiv](../overview-sfiv.md)
