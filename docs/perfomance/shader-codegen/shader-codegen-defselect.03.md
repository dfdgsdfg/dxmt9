---
domain: shader-codegen
workload: 3DMark05 GT3 and SFIV Benchmark
subcategory: defselect
order: 03
title: GT3 And SFIV Visual Gates Pass; SFIV Is Unaffected
date: 2026-07-29
type: experiment-run
status: accepted-visual-gate
source: traces/defsel-visual-gt3/cand; traces/defsel-visual-gt3/glitch; traces/defsel-visual-sfiv/cand; experiments/output/app-d3d9-sfiv-benchmark-defsel-perf-base; experiments/output/app-d3d9-sfiv-benchmark-defsel-perf-cand; experiments/output/app-d3d9-3dmark05-defsel-gt3-cand; experiments/output/app-d3d9-sfiv-benchmark-sfiv-perfprofile-base; experiments/output/app-d3d9-sfiv-benchmark-sfiv-perfprofile-cand
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

**Both lanes above ran on the `debug` profile** (see the correction at the
bottom), so the `~11.3 fps` is a debug figure and is not SFIV's rate. The
comparison is unharmed — both sides carry the same instrumentation — but do not
quote these numbers as SFIV performance.

**Verdict.** ACCEPTED. All four workloads are now validated: GT1 and GT2 gained
(`+8.8%` and `+110%` scene fps), GT3 and SFIV render correctly with no
regression. The skinned-geometry failure mode that the first attempt introduced
does not appear on any of them.

## Correction, 2026-07-31: the SFIV "discrepancy" was the debug profile

This section originally read as an open question — `docs/perfomance/overview-sfiv.md`
records `45.416` sampled fps while both lanes here measured `~11.3`, and it told
readers not to trust `45.416` without re-measuring. **That was wrong, and the
instruction it gave was backwards.** There is no SFIV regression; the two lanes
above ran on the `debug` profile.

The artifacts say so directly. `dxmt9.log` is `653 MB` for the baseline lane and
`619 MB` for the candidate, and its content is per-D3D9-call `debug:` tracing:

```
[dxmt9-device] debug: device_set_vertex_shader_constant_f device=028D0020 start=137 count=3 data=05177AFC
```

Both directories are named `...-defsel-perf-...`, which is what made this hard to
see, and both predate the launcher change that records the resolved profile, so
`result.json:profile` is `null` rather than `debug`. Log size was the only
surviving signature.

The `perf`-profile SFIV pair from the same investigation settles it. Re-measured
from the per-frame `wall_ms` samples in each run's log:

| run | `dxmt9.log` | profile class | `sampled_avg_fps` | median fps |
|---|---:|---|---:|---:|
| `defsel-perf-base` | `653 MB` | debug | `13.094` | `11.20` |
| `defsel-perf-cand` | `619 MB` | debug | `13.506` | `11.33` |
| `sfiv-perfprofile-base` | `22.7 MB` | perf | `41.296` | `58.83` |
| `sfiv-perfprofile-cand` | `22.9 MB` | perf | `43.020` | `59.70` |

`43.020` against the overview's `44.668` is the same class over a different
window (`171.3 s` here versus a `126 s` duration-matched median), so
`overview-sfiv.md` stands as written and needs no re-measurement on this account.

**What survives unchanged:** the verdict. Both lanes carried identical
instrumentation, so `+1.2%` is a valid like-for-like comparison and "SFIV is
unaffected by `d63f7a65`" is still the correct reading.

**The transferable lesson** is the one
`agents/rules/test_wild.rules.md` already draws from an earlier instance of the
same trap: a directory named `perf` is not evidence of the `perf` profile. Since
2026-07-29 the launcher resolves and records the profile in `result.json:profile`
and the summary header, so this is now checkable from the artifact. For runs
older than that, `dxmt9.log` size is the signature — debug is two orders of
magnitude larger.

**Related.** [shader-codegen-defselect.01](shader-codegen-defselect.01.md) ·
[shader-codegen-defselect.02](shader-codegen-defselect.02.md) ·
[shader-codegen](index.md) · [overview-sfiv](../overview-sfiv.md)
