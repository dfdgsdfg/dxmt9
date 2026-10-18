---
domain: present-pacing
workload: STALKER CoP H502/r5 original captured input
title: "H612 — recorded draw calls are less than half of the index/issue region"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-issue-h612/analysis.json; traces/stkcop-encode-issue-h612/runs.json; traces/stkcop-encode-issue-h612/build.json; traces/stkcop-encode-issue-h612/native-tests.json; traces/stkcop-encode-issue-h612/detail-tests.json
related: docs/perfomance/present-pacing/present-pacing-encode-setup.611.md; docs/perfomance/present-pacing/present-pacing-encode-phases.610.md; docs/perfomance/present-pacing/present-pacing-encode-owner.609.md
---

# H612 — captured index/issue ownership

Recorded Metal draw calls occupy 42–43% of the observed index/issue region,
not the whole region. The surrounding work is also distributed: index
preparation and volatile push are individually smaller than the remaining
policy/diagnostic/control path. This is bounded captured-input attribution,
not a removable-time result or proof of the largest live-tail owner.

## Fixed experiment

Retain H602 default archives, H609 root/outer scopes and the H502/r5 original
capture. Three private runtime members change: queue, chunk encoder and draw
encoder. Product sources, shared build outputs and Wine staging remain intact.
There is no new rendering, cache, scheduling or lifetime policy transition.

In `src/dxmt9/dxmt9_draw_encoder_draw.mm`, `encodeDraw`, four optional scopes
observe the region after the BaseState phase mark through return, the existing
index setup block, `buildDrawVolatile`/vertex-bytes push/bookkeeping, and each
of the four `recordedDraw*` call sites. The issue region includes nonindexed
fallthrough, unlike H610's separate tiny tail. Nested scopes are subtracted
from the region remainder. Record-only contexts fail observation qualification.

The recorded-call scopes include the wrapper in
`src/dxmt9/dxmt9_draw_encoder_diagnostics.mm` and its Metal call; they are not
pure driver CPU. The index block includes its diagnostic alternative if taken,
not just buffer lookup. Volatile scope includes immediate counters/trace gates.
Region remainder contains setup, disabled gates, diagnostics, visibility or
tile-FFP postpass where applicable; it is not all redundant work.

Fixed O0,D0,D1,O1 uses one image, fresh processes, detail OFF/ON/ON/OFF,
product counters OFF and all roots selected. No runs are replaced or appended.
Prospective sequence8 retains 526 DrawRuns and 530 subrange loops. Four original
pixel/query/source/lifetime controls pass: seven mapped sources, 670 balanced
wrappers and source/output SHA
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.

Native tests pass 12 controls, 22 parser corruptions and four detail-switch
controls, including nested unwind and explicit finish. Independent raw decoding
conserves all36 roots and verifies source reversal, image hashes and the
three-member archive diff. An initial pre-build source derivation rejected
substring-ambiguous indentation at two call sites; newline-anchored matching
fixed it before compilation or any measured run. No product edit was involved.

## Measurements

Times are milliseconds per execution of this captured source, not live frames.

| Run | Root CPU interval | Whole-loop wall |
|---|---:|---:|
| O0 | 36.354–36.381 | 13.099 |
| D0 | 38.444–38.473 | 15.785 |
| D1 | 37.874–37.898 | 14.036 |
| O1 | 36.765–36.787 | 13.857 |

| Detailed region | Calls | D0 wall | D1 wall | D1 conservative CPU interval |
|---|---:|---:|---:|---:|
| Issue region, inclusive | 3125 | 3.247 | 3.240 | — |
| Index preparation | 3112 | 0.331 | 0.326 | 0.175–0.326 |
| Volatile push | 3125 | 0.448 | 0.470 | 0.319–0.470 |
| Recorded draw calls | 3125 | 1.407 | 1.372 | 1.220–1.372 |
| Region remainder, exclusive | 3125 | 1.060 | 1.072 | 0.920–1.072 |

All D0 child CPU lower bounds are zero because complementary root wall leaves
too much off-CPU uncertainty. D1 recorded-call CPU exceeds each other individual
child upper bound, but this does not establish a repeated CPU ranking in D0.
The two observed recorded-call wall shares are43.35% and42.33% of issue wall,
or8.92% and9.77% of whole-loop wall. Those are measured wall shares only.

Detailed-versus-coarse finite contrasts are +0.179–2.686ms in loop wall and
+1.087–2.119ms in root CPU. Detailed instrumentation materially perturbs the
execution. These finite contrasts are not a calibrated clock tax; do not
subtract them from children or equate the scope times to unobserved default
cost. H610/H611 are separate populations, not paired optimization controls.

## Consequence for the full objective

This rejects treating the whole index/issue bucket as driver time or a large
index-copy bottleneck. It does not justify replay-side index caching, deleting
required draw emission, or reopening H499's already-rejected volatile shadow.
H487 scratch and H497 raster-cache timing gates remain closed too.

H609 still covers only a small fraction of whole-window live Encode CPU.
Before another small removal trial, close that attribution-coverage gap:
measure all root/whole-loop intervals in the selected late episode, qualify
observer perturbation against identical detail-OFF images, and account for
boundary-straddling roots and CPU outside roots. This is the next ownership
gate; captured subphase rankings cannot substitute for it. The global
default-tail largest operation and qualified removable CPU remain unproved.
