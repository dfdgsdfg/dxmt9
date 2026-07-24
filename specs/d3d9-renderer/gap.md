---
type: "Spec Gap"
title: "D3D9 Renderer Gap"
description: "Implementation and evidence gaps for the modern renderer opt-in path."
tags: [specs, gap, d3d9-renderer]
---

# D3D9 Renderer Gap

Domain-owned implementation and evidence gap tracker. Use the
[root gap index](../gap.md) for cross-domain rollup.

## Modern Renderer Opt-In Path

| Area | Status | Current evidence | Missing work |
|---|---|---|---|
| L0 backend boundary (`R-BACK-30.*`, `R-BACK-31.*`, `R-BACK-41.1`) | implemented | `IRenderBackend`, `IExternalDrawEmitter`, backend factory, traditional/framegraph delegation, parity seam, two-mode conformance script, and shared queue/presenter are landed. Strict framegraph delegates to the v2 encoder and keeps the traditional Metal stream. | An in-repo CI runner surface for `R-BACK-39.4` is still absent. |
| L1 device-free graph (`R-BACK-32.*`–`R-BACK-34.*`, `R-BACK-39.7`, `R-BACK-41.2`) | implemented | `fg_dag`, `fg_builder`, lifetime/passcoalesce/memoryless-classifier/dce/reorder/loadstore, `fg_linearizer`, and JSON/dot/mermaid pre/post export are covered by native specs. The builder emits RAW/WAR/WAW edges, treats Clear as a write, canonicalizes aliased surface writes to the owning texture hazard identity, and blocks edge-unsafe reorder/coalesce. The DCE cross-chunk AND/OR correctness fix is landed. | Memoryless alias allocation remains device-gated and unimplemented. |
| Production `passcoalesce` (`R-BACK-34.*`, `R-BACK-38.*`, `R-BACK-40.1`) | partial | The default-off `framegraph + progressive + passcoalesce` lane retains every source command in the DAG, validates a complete duplicate-free optimized permutation, and replays it through the existing v2 `encodeChunk` path. Store proof follows that replay order and keeps invalid evidence conservative. Strict remains feature-empty. Alias-aware GT2 frame279 preserves the `R32F write → sample → R32F write` chain while reducing `18 -> 16` render encoders. Earlier pre-fix evidence measured `18 -> 15`, but that topology moved the texture consumer before its surface producer and is invalid as a parity result. See `docs/perfomance/overview-3dmark05-gt2.md`. | Re-run the order-aware Store performance/capture gate after alias normalization, run the `R-BACK-39.1` parity capture set, wire catalogue `compat_profile`, implement the `R-BACK-34.2` fixed-precision benefit/cost gate and required production `framegraph_pass_*` benefit/reorder counters. `aggressive` profile resolution and runtime downgrade remain absent. |
| Other production L1 features | not started | Analysis-only DAG passes can classify `memoryless`, `dce`, and `reorder`; they do not change Metal emission. | Wire persistent memoryless observation and `TransientAttachmentPool`, semantic-relaxation gates, whole-pass fallback, dry-run divergence, and device-side parity evidence. |
| L2/L3 mesh, bindless, object scheduling, and GPU-driven path (`R-BACK-35.*`–`R-BACK-37.*`, `R-BACK-41.3`–`R-BACK-41.4`) | not started | Requirements and phasing are documented. | Implement route/emitter, cache namespace, bindless heap, mesh dispatch, object scheduling, ICB path, counters, and reduced-counter parity gates. |

The production command-tape lane is intentionally narrower than the staged
`executeLinearization` design. It does not bypass v2 batching, dirty-rebind,
capture, presenter, or completion behavior, and it falls back to source order
when a session, injected command buffer, Clear/helper merge boundary, or replay
permutation cannot be proven safe.
