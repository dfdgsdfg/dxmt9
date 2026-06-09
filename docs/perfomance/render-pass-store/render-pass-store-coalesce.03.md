---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: coalesce
order: 03
title: Per-draw D3D9 Detail Confirms the Re-entry Role Pair from the DAG Dump
date: 2026-06-09
type: measurement
status: accepted-counter-sample
source: traces/app-d3d9-3dmark05-dagcheck-draws/analysis/dag/dag-frame50-chunk50-pre-opt.json, src/dxmt9/framegraph/fg_debug_export.cpp (draws_detail), specs/d3d9-renderer/requirements.md (R-BACK-39.7 DXMT9_RENDERER_DUMP_DAG_DRAWS)
---

# Per-draw D3D9 Detail Confirms the Re-entry Role Pair from the DAG Dump

**Question / hypothesis.** [[render-pass-store-reentry-distance.01]] established
H13/H14 — the same-key re-entry is a stable role pair (opaque-depth-write ↔
textured/screen-blend depth-read; `Clear+Store` ↔ `Load+Store`) — by joining
encoder attribution to load/store actions. Can the modern-renderer DAG dump
**independently confirm those D3D9 roles at the per-draw state level**, in one
artifact, now that it carries per-draw D3D9 detail (`DXMT9_RENDERER_DUMP_DAG_DRAWS`,
R-BACK-39.7)?

**Method.** Device-gated 3DMark05 GT1 run on the default `traditional` encode
path (`status: pass`) with `DXMT9_RENDERER_DUMP_DAG` (frame 50 ±2) +
`DXMT9_RENDERER_DUMP_DAG_DRAWS=1`. The debug dump resolves each pass's draws
through `core::ChunkSlot` and emits, per draw, the D3D9 primitive type/count,
VS/PS shader hash, texture mask, and key render states (alpha_blend, z_enable,
z_write, z_func, alpha_test, cull). Read the frame50 pre-opt re-entry pair P0/P2
(same RT `0x..09` + depth `0x..01`). Debug-only; the Metal encode is unchanged.

**Result.** The DAG dump shows the H13/H14 role pair directly in D3D9 state:

| Pass | draws | `alpha_blend` | `z_write` | `z_enable` | distinct VS/PS shapes | role |
|---|---:|:--:|:--:|:--:|---:|---|
| **P0** (producer) | 42 | **0** (opaque) | **1** (depth write) | 1 | **2** | opaque depth-write (H14 `Clear+Store`) |
| **P2** (re-entry) | 187 | **1** (blended) | **0** (depth read) | 1 | **16** (textured) | blended depth-read (H14 `Load+Store`) |

P0 is homogeneous opaque depth-writing geometry (2 shader shapes, `blend=0`,
`z_write=1`); P2 re-enters the *same* RT+depth as a heterogeneous blended
depth-reading pass (16 shader shapes, `blend=1`, `z_write=0`, `texmask=127/31`).
This is exactly the H13 role alternation and H14 action shape — now read from a
single per-chunk dump artifact (per-draw D3D9 state), independent of the encoder
join.

**Why it matters for H6.** P2's `z_write=0` means it **reads** the depth P0
wrote, and its `blend=1` means it reads the color destination P0 wrote — so the
re-entry's `Load` genuinely consumes P0's `Store` on **both** attachments.
Coalescing P0+P2 (keeping color+depth in tile memory across the merged pass) is
therefore the semantically-correct elimination of that round-trip, and the
draws_detail makes the correctness argument checkable at D3D9 level (the merged
pass must preserve P2's depth-read of P0's depth and P2's blend over P0's color,
which holds because no pass writes `0x..09`/`0x..01` between them —
[[render-pass-store-coalesce.01]] `P0→P2` WAW is the only edge on those handles).

**Limits.** Debug-only (`DXMT9_RENDERER_DUMP_DAG_DRAWS`), resolved from ChunkSlot;
no GPU measurement (cost/bytes are gputrace/counter territory). Confirms the
re-entry **roles/semantics**, not the saving — which remains the device-gated
executor proof ([[render-pass-store-coalesce.02]]).

**Verdict.** Accepted as counter-sample. The DAG dump's per-draw D3D9 detail
independently reproduces the H13/H14 re-entry role pair (opaque depth-write ↔
blended depth-read) and exposes the depth-read + blend dependency that makes the
H6 coalesce semantically correct — all from one CPU-side, deterministic,
GPU-capture-free artifact.

**Related.** [[render-pass-store-coalesce.01]] (WAW edge) ·
[[render-pass-store-coalesce.02]] (passcoalesce removes 100%) ·
[[render-pass-store-reentry-distance.01]] (H13/H14) · [[render-pass-store]] ·
[[overview-3dmark05-gt1]].
