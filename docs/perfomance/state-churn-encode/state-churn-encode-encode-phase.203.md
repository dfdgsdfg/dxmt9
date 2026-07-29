---
domain: state-churn-encode
workload: native backend regression
subcategory: encode-phase
order: 203
title: Direct-Cbuf Payload-Source Dirty-Rebind Regression
date: 2026-07-20
type: implementation-validation
status: accepted-correctness-gate-closed-default-on
source: src/dxmt9/dxmt9_uniform_dirty.hpp; src/dxmt9/dxmt9_uniform_dirty.cpp; src/dxmt9/dxmt9_draw_encoder.mm; src/dxmt9/dxmt9_pipeline_cache.cpp; tests/native/core/draw_uniforms_dirty_spec.cpp; tests/native/backend/backend_pipeline_key_spec.cpp
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.148.md
---

# Encode Phase 203 - Direct-Cbuf Payload-Source Dirty-Rebind Regression

## Question and Verdict

Can the phase-148 direct-cbuf stale-binding fix be pinned without depending on
3DMark timing or a visually similar capture?

Yes. The payload-source comparison and dirty-range reconstruction now live in
the shared `dxmt9_uniform_dirty` pure value-transform surface, and the live
draw encoder calls that same implementation. The native regression covers
unchanged sources, `A -> B -> A`, an empty replacement payload, and a
vertex-only source transition.

This closes the deterministic correctness gate identified by phase 202.
Together with that cross-workload CPU/visual result, Stage 2b direct-cbuf is
now default-on. `DXMT9_ARGBUF_DIRECT_CBUF=0` remains the explicit rollback;
repeated SFIV/GT3 GPU-phase sampling is follow-up monitoring rather than a
promotion blocker.

## Runtime Shape

`classifyDirectCbufPayloadSourceChange()` compares the previous and current
VS/PS source hashes only after a previous payload exists. When either source
changes, `applyDirectCbufPayloadSourceChange()` reconstructs dirty state from
the current payload's live float, integer, and boolean counts.

An empty current stage sets a sentinel float dirty bit with a zero high-water
range. This forces the direct slot to move to the minimum valid backing slab
instead of retaining the old payload's buffer.

Both records are flat POD values. The hot path keeps the hash comparison
inline and calls the range reconstruction only when a source actually changes.

## Deterministic Coverage

| Transition | Pinned result |
|---|---|
| first payload | no synthetic delta; pass-initial dirty state owns the first bind |
| `A -> A` | no VS or PS dirty bits |
| `A -> B` | only B's live VS/PS categories and ranges become dirty |
| `B -> A` | A becomes dirty again; an older A binding is not treated as current |
| `A -> empty` | VS-F and PS-F sentinel bits select minimum backing slabs |
| VS-only hash change | VS becomes dirty while PS remains clean |

## Verification

The focused and related native gate passed `6/6`:

- `dxmt9-draw-uniforms-dirty-spec`
- `dxmt9-state-draw-transform-spec`
- `dxmt9-argbuf-hybrid-spec`
- `dxmt9-argbuf-hybrid-msl-spec`
- `dxmt9-backend-pipeline-key-spec`
- `dxmt9-shader-argbuf-binding-value-spec`

The complete `build-arm64-nowine` Meson suite then passed `609/609`, including
the default-on Stage 2b and Stage 1 shader readback corpus, bridge/backend
tests, manifest and performance-document audits, and `dxmt9-verify-tla`. A
separate shader readback with `DXMT9_ARGBUF_DIRECT_CBUF=0` pins the rollback
lane. The pure policy regression also fixes unset to ON, empty/`0` to OFF, and
other non-empty values to ON.

No new GPU capture was required because this gate concerns deterministic
payload-source and dirty-range semantics. The cross-workload visual and GPU
health evidence remains owned by phase 202.
