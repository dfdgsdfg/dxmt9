---
type: "Spec Gap"
title: "Render Provider Policy Gap"
description: "Implementation and evidence gaps for the rendering-provider registry."
tags: [specs, backend, render-provider, gap]
---

# Render Provider Policy Gap

| Area | Status | Current evidence | Missing work |
|---|---|---|---|
| Policy classification (`R-BACK-42.1`–`42.7`) | specified | The registry separates stable providers, experimental candidates, diagnostics, retired selectors, implementation state, and activation state. Existing domain specs own mode semantics. | Add an audit that compares runtime-mutating selectors with this registry and the environment-variable rules. |
| Stable scheduling resolver | partial | `R-BACK-2.66` names the stable axes; `DXMT9_CPU_READY_TAPE` reaches the streaming implementation and serial identity is the production partition path. | Implement canonical source/partition/segment selectors, immutable resolution, requested/resolved reporting, and the mode matrix. |
| Producer replay classification | implemented / documentation normalized | Inline rollback and engine-default offload are both implemented and regression covered. | A canonical positive typed selector may replace the boolean opt-out surface only if migration preserves `DXMT9_OFFLOAD_COMMIT_REPLAY`. |
| Submission and Present policies | partial | Mid-chunk, acquire, and boundary pure resolvers plus native matrices exist. Framebuffer-only drawable behavior is specified as D3D9-neutral. | Classify or retire `per-n-records`; add canonical single selectors for acquire/boundary only if legacy precedence and app overrides are preserved. Deferred boundary remains experimental. Add the missing framebuffer-only Lock/GetRenderTargetData conformance evidence routed through the D3D9 WSI gap. |
| Binding and tile providers | binding implemented; tile correctness-blocked | Stage 1/Stage 2/Stage 2b/resource-array are implemented. The tile candidate passes the full-screen single-draw fixture, but `dxmt9_tile_ffp_partial_rect_preserves_clear_readback` proves that the attachment-wide post-process fogs uncovered clear pixels under the diagnostic route. Non-diagnostic `tile-auto` therefore resolves to portable. | Keep Stage 1 and portable fallbacks. Tile promotion requires coverage/prior-colour composition plus partial, overlap, alpha-reject, and multi-draw GPU equality before any workload perf gate. Add requested/resolved startup reporting shared with the registry. |
| Experimental-candidate cleanup | open | Active rules identify candidate and diagnostic selectors; retired families are explicitly not honored. | Remove rejected candidates when no new workload justifies them, or promote them through an owning requirement. Do not preserve correctness-invalid probes as provider modes. |
| Opaque-depth LRU32 candidate economy | implemented / opt-in | The original-order fallback is default. Opt-in candidate construction now rejects production draws below 256 primitives before locality measurement, bounds the builder at 65,536 triangles / 256 frontier entries / deterministic linear selection work, and aborts the complete candidate without changing output when a bound is reached. Gate failures are attributed by primitive bucket; pre-gate and typed budget abort counters are included in the shutdown report. Diagnostic probes retain an explicit unbounded builder path. | Re-measure only after the pre-gate/budget shape is validated; promotion still requires workload benefit, exact visual equality, and locality conservation. |
