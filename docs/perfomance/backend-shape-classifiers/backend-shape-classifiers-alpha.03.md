---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: alpha
order: 03
title: Scoped Alpha-Blend State-Shape Probe (large4096+alpha)
date: 2026-06-02
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L15322-L15441
---

# Scoped Alpha-Blend State-Shape Probe (large4096+alpha)

**Question / hypothesis.** With a precise class selector that hits the active
large alpha class, does disabling alpha blend materially move the Apple hidden
vertex/tiler/backend storage shape? Correctness-invalid diagnostic, but a valid
state-shape probe because scope is verifiable via
`probe_disable_alpha_blend_draws`.

**Method.** `DXMT9_PROBE_DISABLE_ALPHA_BLEND_CLASSES large4096,alpha-blend`
(wrapper `--probe-disable-alpha-blend-classes large4096,alpha-blend`). Class-only
smoke preflight first (`scoped-alpha-large4096-class-nogputrace-r1`, `9` matching
draws / `146,961` tris) because frame-60 encoder indices drift between direct
runs and two earlier row-scoped attempts (`60/4`, `60/2`) missed the active row.
Then `scoped-alpha-large4096-class-gputrace-r1`, finalized vs
`current-head-index-scout-gputrace-r1` with `--require-xcode-counter-coverage
--require-dxmt-join-coverage`. Probe scope confirmed `probe_disable_alpha_blend_draws=9`.

**Result.**

| Metric | Baseline | Alpha-blend-off class probe | Delta |
|---|---:|---:|---:|
| Total GPU time | `50.832ms` | `25.417ms` | `-50.00%` |
| Top GPU time | `50.368ms` | `25.071ms` | `-50.23%` |
| Top VS buffer write | `2236.981MiB` | `1054.495MiB` | `-52.86%` |
| Top unexplained write | `2236.772MiB` | `1054.163MiB` | `-52.87%` |
| Top VS B / VS invocation | `1266.127B` | `714.551B` | `-43.56%` |
| Top VS / expected 184B VSOut | `6.881x` | `3.883x` | `-43.56%` |

Caveat: row identities drifted — strict shared-row comparison only covers `60/0`,
`60/1`, `60/3`; baseline's dominant `60/4` large4096+alpha/depth-read row moved to
candidate `60/2` (still holds `9 draws / 53,588 tris / 160,764 vertices`). Stream/IB/PSO
churn *regressed* (`+27.88% / +6.28% / +45.45%`), so the win is not from reduced
CPU bind churn. Candidate dxmt CPU writer bytes only `0.727MiB` vs `1054.889MiB`.

**Verdict.** Alpha blending is a **confirmed significant contributor** to the
hidden vertex/tiler/backend storage shape for this workload class — disabling it
cuts the hidden estimate roughly in half. But it is **not a fix**
(correctness-invalid) and does not remove the bucket: candidate hot encoders still
write `1054.495MiB` of VS traffic at `3.883x` expected VSOut, so
primitive/backend pressure remains a secondary cause.

**Related.** [backend-shape-classifiers](index.md) · escalation of [backend-shape-classifiers-alpha.01](backend-shape-classifiers-alpha.01.md) and [backend-shape-classifiers-alpha.02](backend-shape-classifiers-alpha.02.md) · partially attributes [hidden-backend-storage](../hidden-backend-storage/index.md) · confirms [index-cache-locality](../index-cache-locality/index.md) screen-blend class is shape-sensitive.
