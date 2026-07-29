---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: varying
order: 03
title: Dump-First VSOut Liveness Replay (semantic-safe)
date: undated
type: validation
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L3072-L3114
---

# Dump-First VSOut Liveness Replay (semantic-safe)

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Can the existing payload dump exercise a pair-local
`VSOut` liveness trim *without changing pixels* — i.e. is the trim semantically
safe — for row `50/2` (depth-read / no-alpha)?

**Method.** `scripts/tools/run_3dmark05_mini_replay.py` on the row `50/2` semantic
payload manifest with `--trim-vsout-to-fs-reads`, real D24X8 `--depth-input`
sidecar, `--compile --run`, and a `--color-output` PPM. The single trimmed shader
variant kept `{position, texcoord0, texcoord1, texcoord6, texcoord7, fogFactor}`
and removed `{color, secondaryColor, texcoord2..5, pointSize}`. Original-vs-trim
same-input exact image compare (`...-exact-compare.md`).

**Result.**

| Check | Result |
|---|---:|
| Compared pixels | `786,432` |
| Changed pixels | `0` |
| Max delta | `0` |
| SSIM | `1.000000` |
| Active pixels before/after | `2309 / 2309` |

**Verdict.** The trim is **semantically SAFE** (0 changed pixels, SSIM 1.000) —
a useful correctness result. But the paired Xcode liveness capture
([vsout-layout-varying.02](vsout-layout-varying.02.md)) showed the VS-write bucket barely moved
(`-0.01%`), so visible VSOut width is still **rejected** as the owner. Safe to do,
not worth doing for perf.

**Related.** [vsout-layout](index.md) · paired with the Xcode-rejection [vsout-layout-varying.02](vsout-layout-varying.02.md) · [mini-replay-bisection](../mini-replay-bisection/index.md) · [hidden-backend-storage](../hidden-backend-storage/index.md).
