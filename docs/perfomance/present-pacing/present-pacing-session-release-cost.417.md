---
domain: present-pacing
workload: STALKER CoP Day
title: "H417: Same-prefix ClosePass round trips exist; regression attribution remains open"
type: evidence
status: mechanism-observed-default-off
updated: 2026-09-30
source: traces/query-ranges-h417/{comparison.json,summarize.py,wild.py,native.txt,x64-restaged.txt,tla.log,rebuild-x64-retry.txt}; traces/query-ranges-h417/{on-narrow1,on-early1,on-active1,off-active1}/{supply.csv,release.json,invocation.json,completion.json}; traces/query-ranges-h417/on-diag2/supply.csv; traces/query-ranges-h417/on-diag1/runner.log; experiments/output/app-d3d9-stkcop-bench-query-ranges-h417-{on-narrow1,on-early1,on-active1,off-active1}/result.json
related: present-pacing-query-range-wild.416.md; ../../../specs/backend/query-scope-ranges.md
---

# H417: Repeated release transport is real, but not the complete FPS explanation

Base commit `0be3b922` records H416. This change adds observation only: no
release, publication, acknowledgement, submission or completion is skipped.
Query ranges stay default OFF.

## Observation contract

Existing bounded `DXMT9_CPU_SUPPLY_TRACE` records ordered release post, raw,
requested policy and unique queue identity, Encode action begin, successful
acknowledgement and caller return. Release-state owner plus event ordinal
joins the stages. No runtime type/signature or lifetime capability changes;
disabled hooks perform cached gates only.

`scripts/tools/analyze_session_release.py` rejects duplicate stages, bad
identities/order/CPU endpoints and incomplete interior caller groups. It
separates terminal, consumer-only internal releases, stopped callers and
boundary clipping. Consecutive retained DirectObservation/ClosePass requests
must have adjacent ordinals, the same queue/owner/sequence and nondecreasing
raw identity, with the prior caller returned before the next post.

This classification is **not a reset epoch or active-pass witness**. Return
means the acknowledgement wait ended; a SubmitAndWait completion wait, initial
queue acquisition, writer commit and full release-ring wait are outside scope.
Diagnostic clocks execute under the queue lock and perturb scheduling.

## Runtime evidence

Sikarugir CX24.0.7, Day/high, 1280×720 windowed, two game threads and sixteen
sound targets, matching H416's policy settings. Four qualified narrow runs
share all five binary receipts and requested config SHA
`1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.
Unix receipt: 10,928,192 bytes, `83b1d1914553fdf4`.
All four exit successfully with zero recorded GPU errors. No new full-scene
visual/exact claim; these are diagnostic runs, not FPS comparisons.

| Run | Encoded Present window | Complete caller releases | Retained releases | Same-prefix repeats | Total caller round trip ms |
|---|---|---:|---:|---:|---:|
| ON narrow1 | 1750–1754 | 5 | 0 | 0 | 0.670 |
| ON early1 | 300–304 | 5 | 0 | 0 | 1.007 |
| ON active1 | 2000–2004 | 545 | 540 | 535 | 13.795 |
| OFF active1 | 2000–2004 | 9 | 0 | 0 | 6.359 |

The first two windows did not exercise retained occlusion release. H416's
Present-level scope observations identify activity concentrated near the end;
that evidence motivated the 2000–2004 window. OFF also has nine consumer-only
internal releases, excluded from the caller timing population.

**The ON/OFF windows are not matched work.** This time-driven benchmark reaches
different scene positions at the same Present ordinal when throughput differs.
Do not subtract the table's times as an optimization effect or compare these
runs' FPS with H416 observer-OFF values.

Within ON active1, the 535 repeated-prefix requests account for:

| Scope | Total over five Presents, ms |
|---|---:|
| Post → Encode action begin, wall | 3.572 |
| Encode action → ack, wall | 1.378 |
| Ack → caller return, wall | 2.671 |
| Complete repeated round trips, wall | 7.621 |
| Posting-thread CPU across repeated round trips | 4.072 |
| Encode action CPU | 1.336 |

The three wall partitions form the round-trip envelope. CPU measurements are
overlapping observations, not additional frame costs. Repeats amount to 107
requests and 1.524 ms observed elapsed per Present in this small diagnostic
window; this is neither product-only overhead nor a demonstrated FPS ceiling.
All 535 repeats have pending session and record, so the close helper is called.
The trace does not distinguish its `Closed` and `NoActivePass` return values.

## Verification and rejected evidence

- Python supply/analyzer suite: 39 tests, including enum-order binding,
  release timing partition, identity/sequence/ordinal separation, duplicate,
  missing, invalid order/policy/CPU and stopped/clipped/consumer-only cases.
- Native supply/release/serialized query matrix: 11/11 Meson tests.
- Final x86_64/Rosetta equivalent set plus install-name audit: 12/12.
- Existing TLA inventory passes: 65 safety, 5 progress and 157 expected
  counterexamples. No new scheduling transition or composed proof is claimed.
- Initial 1750–1780 trace overflows by 340,106 rows; analyzer rejects it. The
  successful game exit does not qualify that trace. Storage was not enlarged;
  the window was narrowed to five Presents.
- One launch failed before the game during staging: prepending llvm-mingw
  selected its bare `clang++` for Unix work, incompatible with the current SDK
  linker stubs. Forcing the Apple toolchain first also changed `ar`, which
  rejected the existing `csrD` flags. Restoring the configured host tool lookup,
  appending llvm-mingw and explicitly setting `SDKROOT` repaired the build.
  A premature test invocation during relinking/fixup was unqualified; the
  complete post-build set was rerun and passed. Failed logs remain preserved.

## Decision and next gate

Repeated same-prefix synchronization is a concrete remaining cost, but these
observations do not explain H416's entire 9.7% FPS deficit. Do not implement a
sequence-number-only early return.

The next bounded optimization gate is a same-input release fixture that
distinguishes actual pass closure from `NoActivePass`, with an
identity/epoch/action-qualified closed-prefix witness. Reuse must invalidate on
new encoder work, preserve raw/FIFO progress, reject stale/reset/terminal
identities, and never replace submission or GPU-completion obligations.
Compare the unchanged and candidate paths on that input before a new wild
performance pair. Global Arena/session exclusion remains a separate, unpriced
candidate; it must not be charged to this release timing bucket.
