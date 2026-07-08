---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: dontcare
order: 01
title: Render-Pass Store Action DontCare Opt-In (design)
date: undated
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L3898-L3940
---

# Render-Pass Store Action DontCare Opt-In (design)

**Question / hypothesis.** Can a color/depth live-out proof let a render pass
choose `StoreActionDontCare` when its stored contents are dead before the next
use, removing part of the repeated Store/Load preservation traffic?

**Method.** Designed two proof families plus an opt-in flag.
- Color today always stores (`Store`, or `MultisampleResolve` for resolve
  targets); `flushRender()` marks every active color handle as touched, so a
  later same-RT pass cannot use first-use `DontCare` and pays a `Load`.
- `DXMT9_AGGRESSIVE_COLOR_DONTCARE=1` lets the color proof return dead-at-end
  `DontCare` when the color handle does not reappear in the rest of the chunk and
  no Present is seen; default stays conservative (only next-clear can
  DontCare-store color). Companion `DXMT9_AGGRESSIVE_DEPTH_DONTCARE` covers the
  depth side.
- New `render_pass_color_proof_*` counters separate next-clear, draw-target,
  texture-sample, present, and disabled-dead-at-end cases.

**Result.** Evidence from the frame120 Xcode trace that motivated this:
color store is always active; depth look-ahead proof exists but reported
`render_pass_store_action_depth_dontcare=0` in that trace. The concrete target is
same RT/depth re-entry — `rt=0x30000460000000c,depth=0x300000100000001` appears
twice in `frame120` and accounts for `24.643 ms` / `73.32%` of the frame. Split
shape: `render_split_rt_change=9844`, `render_split_clear=3576`,
`render_split_present=1253`.

**Verdict.** Design is representable, but there is a GT1 measurement gap: the live
counter proof needs a fresh unlocked GT1 run. (The subsequent runs show the cheap
proofs do not actually fire on GT1 — see [render-pass-store-dontcare.02](render-pass-store-dontcare.02.md).)

**Related.** [render-pass-store](index.md) · prior: [render-pass-store-reentry.01](render-pass-store-reentry.01.md)
(the re-entry budget this attacks) · next: [render-pass-store-dontcare.02](render-pass-store-dontcare.02.md)
(the color next-clear run that found zero hits) · [baselines](../baselines/index.md) (frame120
24.643 ms re-entry).
