---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: memoryless
order: 01
title: Transient D3D9 RT Memoryless Promotion (design)
date: 2026-06-06
type: conceptual-model
status: model
source: design proposal — frame graph / TBDR-friendly pass restructuring track
---

# Transient D3D9 RT Memoryless Promotion (design)

**Question / hypothesis.** D3D9 intermediate render targets (shadow map, glow
buffer, post-process intermediates) that are produced and consumed entirely
within one frame, with no CPU readback, never need to touch device RAM. Can
classifying those RTs and allocating their Metal textures with
`MTLStorageModeMemoryless` remove that bandwidth from the GT1 budget without
violating D3D9 semantics? This sits alongside [render-pass-store-dontcare.01](render-pass-store-dontcare.01.md):
DontCare attacks the store action, memoryless attacks the storage residency
itself.

**Method (proposed, not yet implemented).** Two-part design.

1. **D3D9 transient RT classifier** — per-RT, per-frame lifecycle tracking.
   Mark as a memoryless candidate when, within the frame:
   - no `IDirect3DSurface9::LockRect` / `UnlockRect`,
   - no `IDirect3DDevice9::GetRenderTargetData`,
   - no `StretchRect` source or destination outside the same render pass,
   - no cross-frame usage (allocated `D3DPOOL_DEFAULT`, released or reused
     before the next `Present`),
   - no D3D9 `SetTexture` binding outside the producing pass's consumer chain.

2. **Promotion gate** — when an RT clears every classifier check, request
   `MTLStorageModeMemoryless` at texture allocation. Failure must demote to
   `MTLStorageModePrivate` and trigger a full texture realloc; the demotion
   must be observable through a counter so wild-test failures attribute back
   to misclassification.

**Critical caveat — same-render-pass scope.** `MTLStorageModeMemoryless` is
scoped to a single Metal render pass. Across render-pass boundaries the
attachment contents do not persist in tile memory and must be re-cleared or
re-rendered. That means memoryless promotion only pays off when the
producing and consuming work share one Metal render pass. The currently
visible candidates:

| D3D9 intermediate pattern | Same-pass producible? | Notes |
|---|---|---|
| Single post-process step (color → color blit + sample) | sometimes | needs producer+consumer fused into one render pass |
| Shadow map (depth → sampled in main pass) | no | producer and consumer are different passes |
| Glow / bloom intermediate | sometimes | only if chain merged |
| FSAA resolve target | yes | already pass-local |
| Multi-stage post-process chain | no without coalesce | needs [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) P1 pass coalescing |

Most D3D9 intermediates do **not** share a Metal render pass with their
consumer today, so memoryless lands only after the P1 coalesce track makes
producer+consumer pass-mergeable. Without that, memoryless promotion applies
to a narrow surface (resolve targets and a small number of single-step
post-process intermediates).

**Risks.**
- **Misclassification → D3D9 semantic break.** If the app later reads the RT
  (lazy `LockRect`, surface copy, sampled in a later frame's pass), the
  memoryless contents are gone and the visual is wrong. Conservative default
  (everything is `Private`, opt-in promotes) plus per-app `compat_profile`
  overrides.
- **Cross-frame preservation.** D3D9 surfaces can be read on a future frame.
  Classifier must observe at least one prior frame before promoting, or rely
  on a per-app whitelist.
- **Multi-pass producer.** If the same RT is written across multiple Metal
  passes (state-churn breaking a pass), memoryless does not preserve content
  between them.
- **Lock-on-recycle.** Apps that recycle the same RT handle across frames and
  occasionally `Lock` it for debug/tooling would silently lose those reads.

**Verification gates (proposed counters / tests).**
- `render_pass_memoryless_rt_promoted` — RTs allocated as memoryless this frame.
- `render_pass_memoryless_rt_bytes` — total memoryless attachment bytes per frame.
- `render_pass_memoryless_demoted_via_lock` / `_via_readback` / `_via_cross_frame_use` —
  demotion classifiers; each non-zero value is a misclassification incident.
- `tests/native/backend/render_pass_actions_spec.cpp` — extend with
  promote-allow / demote-on-lock / demote-on-readback / demote-on-cross-frame cases.
- D3D9 conformance + catalogue wild runs must show zero visual regression
  (`docs/research/dxvk-d3d9-quirk-checklist.md` set as the baseline shape).

**Result / status.** Currently a proposal — no implementation, no GT1
measurement. Implementation cost estimate is modest for the classifier plus
storage-mode parameter, but the **landing surface is narrow without P1
pass-coalescing** (see `H6` in [render-pass-store](../render-pass-store.md)). The honest framing is
that memoryless promotion is a multiplier on whatever coalesce work makes
producer+consumer passes mergeable; it is not a standalone GT1 lever.

**Verdict.** Open proposal. Worth recording so the option survives across
sessions, but its measurable GT1 contribution is gated on the P1 coalesce
track. Safe path: implement the classifier and counters first (no behavior
change), confirm transient share from real D3D9 traces, then promote only
after coalesce shows producer+consumer same-pass cases.

**Related.** [render-pass-store](../render-pass-store.md) · [render-pass-store-dontcare.01](render-pass-store-dontcare.01.md)
(orthogonal store-action proof) · [render-pass-store-passchain.01](render-pass-store-passchain.01.md) (the
pass-chain analysis that constrains where memoryless would even apply) ·
[hidden-backend-storage](../hidden-backend-storage.md) (P0 owner; memoryless is bandwidth-only and does
not move the hidden VS-write bucket).
