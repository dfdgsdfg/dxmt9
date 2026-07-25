---
type: "Spec Gap"
title: "D3D9 Renderer Gap"
description: "Implementation and evidence gaps for the modern renderer path."
tags: [specs, gap, d3d9-renderer]
---

# D3D9 Renderer Gap

Domain-owned implementation and evidence gap tracker. Use the
[root gap index](../gap.md) for cross-domain rollup.

## Modern Renderer Path

| Area | Status | Current evidence | Missing work |
|---|---|---|---|
| L0 backend boundary (`R-BACK-30.*`, `R-BACK-31.*`, `R-BACK-41.1`) | implemented | `IRenderBackend`, `IExternalDrawEmitter`, backend factory, traditional/framegraph delegation, parity seam, two-mode conformance script, and shared queue/presenter are landed. Strict framegraph delegates to the v2 encoder and keeps the traditional Metal stream. | An in-repo CI runner surface for `R-BACK-39.4` is still absent. |
| L1 device-free graph (`R-BACK-32.*`–`R-BACK-34.*`, `R-BACK-39.7`, `R-BACK-41.2`) | implemented | `fg_dag`, `fg_builder`, lifetime/passcoalesce/memoryless-classifier/dce/reorder/loadstore, `fg_linearizer`, and JSON/dot/mermaid pre/post export are covered by native specs. The builder emits RAW/WAR/WAW edges, treats Clear as a write, canonicalizes aliased surface writes to the owning texture hazard identity, and blocks edge-unsafe reorder/coalesce. The DCE cross-chunk AND/OR correctness fix is landed. | Memoryless alias allocation remains device-gated and unimplemented. |
| Production `passcoalesce` (`R-BACK-34.*`, `R-BACK-38.*`, `R-BACK-40.1`) | partial / default | The default `framegraph + progressive + passcoalesce` lane retains every source command in the DAG, validates a complete duplicate-free optimized permutation, and replays it through the existing v2 `encodeChunk` path. Store proof follows that replay order and keeps invalid evidence conservative; explicit traditional, strict, and empty-feature rollbacks remain. Alias-aware GT2 frame279 preserves the `R32F write → sample → R32F write` chain while reducing `18 -> 16` render encoders. Post-fix GT1/GT2/GT3 runs completed with zero GPU/pipeline failures and reduced render-pass/present counts; exact GT3 captures across 1:06–1:08 do not reproduce the quadrant glitch. An env-clean default SFIV run reaches a valid rendered scene; a separate perf run encodes `7,320` Presents and `1,674,130` draws with zero chunk rejects, GPU command-buffer errors, pipeline-build failures, or missing-pipeline draws. Earlier pre-fix `18 -> 15` evidence is invalid because it moved a consumer before its producer. See `docs/perfomance/overview-3dmark05-gt2.md` and `docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.40.md`. | Run the device-backed `R-BACK-39.1` pixel parity capture set, wire catalogue `compat_profile` for per-app strict rollback, and implement the `R-BACK-34.2` fixed-precision benefit/cost gate plus required production `framegraph_pass_*` benefit/reorder counters. `aggressive` profile resolution and runtime downgrade remain absent. |
| Other production L1 features | not started | Analysis-only DAG passes can classify `memoryless`, `dce`, and `reorder`; they do not change Metal emission. | Wire persistent memoryless observation and `TransientAttachmentPool`, semantic-relaxation gates, whole-pass fallback, dry-run divergence, and device-side parity evidence. |
| L2/L3 mesh, bindless, object scheduling, and GPU-driven path (`R-BACK-35.*`–`R-BACK-37.*`, `R-BACK-41.3`–`R-BACK-41.4`) | not started | Requirements and phasing are documented. | Implement route/emitter, cache namespace, bindless heap, mesh dispatch, object scheduling, ICB path, counters, and reduced-counter parity gates. |

The production command-tape lane is intentionally narrower than the staged
`executeLinearization` design. It does not bypass v2 batching, dirty-rebind,
capture, presenter, or completion behavior, and it falls back to source order
when a session, injected command buffer, Clear/helper merge boundary, or replay
permutation cannot be proven safe.
