---
domain: tvb-mechanism-proof
workload: 3DMark05 GT1
subcategory: proof
order: 02
title: TVB / Parameter-Buffer Design Reference
date: 2026-06-03
type: conceptual-model
status: model
outdated: retired-journal
source: specs/perfomance.plan.md#L18825-L18846, specs/perfomance.plan.md#L20005-L20008
---

# TVB / Parameter-Buffer Design Reference

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** What is the external architectural model that explains
Xcode's large `VS Buffer Device Memory Bytes Written` bucket, and why is a
standalone row-local replay reporting `0 MiB` an expected reading rather than a
fidelity defect?

**Method.** Synthesis of public Apple/PowerVR/Asahi documentation cross-checked
against MoltenVK source. No dxmt9 code change — this is the model the
mechanism proof ([tvb-mechanism-proof-proof.01](tvb-mechanism-proof-proof.01.md)) is measured against. Full
design rationale and citations: `docs/superpowers/specs/2026-06-03-tvb-mechanism-proof-design.md`.

**Result (the model).** Apple Silicon's vertex-stage writes go through a
firmware-owned **Tiled Vertex Buffer / Parameter Buffer (TVB/PB)** living in
**device RAM**. Key properties:

- Size scales with `visible_vertices × per_vertex_VSOut_bytes` per pass.
- On overflow the GPU triggers a **partial render** to flush vertex data,
  adding store/reload traffic counted in the same Xcode counter.
- **No `MTLBuffer` storage-mode or texture-usage flag affects the TVB.** The
  mechanism is selected by driver/firmware at submission, not by application
  APIs — so it cannot be tuned away through D3D9→Metal binding choices.
- A row-local mini-replay submits one encoder with a small, self-contained
  payload; the PB is allocated fresh and never reaches the spill threshold, so
  `VS Buffer Device Memory Bytes Written` reads `0 MiB`. This is
  architecturally correct, not a missing-capture bug. A full GT1 frame (many
  encoders, accumulated PB residency, large indexed volumes) crosses the spill
  threshold many times and reports gigabytes.

Joined-CSV derived fields expose this distinction:
`dxmt_tvb_pressure_proxy_mib`, `dxmt_tvb_named_to_proxy_ratio`,
`dxmt_vs_buffer_write_to_tvb_proxy_ratio`. Full-frame attribution found named
tiled counters account for only ~15% of the expected TVB proxy while Xcode VS
buffer write stays 6–9× larger than that proxy — so named tiled bytes are
**subtype evidence**, and the production gate requires the larger hidden
estimate to move.

**References.**
- WWDC20 #10632 "Optimize Metal Performance for Apple silicon Macs" — TVB as the vertex→fragment hand-off in device memory.
- Alyssa Rosenzweig, Asahi GPU part 5 — Parameter Buffer as a firmware-managed dynamic heap, partial-render flush on overflow.
- Imagination "What is the Parameter Buffer?" — canonical size model; device-memory bandwidth as primary cost.
- MoltenVK source — confirms no `MTLBuffer`/texture flag affects the TVB.

**Verdict.** MODEL (accepted as the explanatory framework). It supplies the
linear-scaling law that [tvb-mechanism-proof-proof.01](tvb-mechanism-proof-proof.01.md) verifies empirically and
underpins the central finding that the GT1 cost is hidden GPU-side
vertex/tiler/parameter storage.

**Related.** [tvb-mechanism-proof](index.md) · [tvb-mechanism-proof-proof.01](tvb-mechanism-proof-proof.01.md) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [index-cache-locality](../index-cache-locality/index.md) · [vsout-layout](../vsout-layout/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
