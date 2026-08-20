---
type: "Spec"
title: "Producer Concurrency Spec"
description: "Actor/lock-domain architecture, the bridge entry classification table, ownership assignments, and the verification mapping for producer–queue concurrency."
tags: [specs, backend, producer-concurrency]
---

# Producer Concurrency Spec

Companion to [requirements.md](requirements.md). The design history and
measured motivation live in
`docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`; this
document is the durable statement of the resulting architecture.

## 1. Actors and lock domains

Four actors touch queue/pool state:

| Actor | Runs | Enters via |
|---|---|---|
| Producer | game thread | `dxmt9c_*` bridge entries (synchronous, in-process calls) |
| Replay worker | `ReplayOffloadWorker` thread | chunk replay → `submit*` paths; last-ref drops → destroy/reclaim |
| Encode thread | queue encode loop | slot dequeue/encode/submit |
| Completion | queue finish loop / GPU callbacks | watermark advance → reclaim |

Lock domains, from narrowest to widest:

| Domain | Protects | Notes |
|---|---|---|
| `HandleArena` internal mutex | slot metadata, `lastUsedSeqId` stamps | The **arena-stamp exception**: publishers may stamp retained objects without the queue mutex (pool header contract; production callers: producer bulk mark, worker draw mark, both via `restampIfTicketAdvancedLocked`'s frozen-ticket protocol). |
| low-4GB shadow pool mutex | wow64 shadow block pool | `7302fa32`; release reachable from the worker. |
| wire-object cache mutex | PE wire-identity map | create/destroy frequency only. |
| `CommandQueue::mutex_` | slot lifecycle, watermarks, capture, map waits, pool structure mutation, reclaim | The residual `queue-shared` domain; per-site contention is observable via `DXMT9_PERF_QUEUE_MUTEX_SPLIT` (42 tagged sites + lifecycle segment holds). |

## 2. Established ownership assignments

Evidence: the four source audits in the design doc §7 and the
`ProducerMarkReclaim` model.

| State | Class (R-BACK-43.4) | Evidence / mechanism |
|---|---|---|
| Buffer rename ring (`renameRing`, `renameActiveIndex`) | `producer-owned` | Referenced only inside the pool; worker/encode consume the commit-time snapshot. |
| Chunk buffer-binding capture read-set (`contents`, `contentRevision`, `desc.size`, flavor flags) | `producer-owned` (writes) + `owner-published` via commit-time snapshot | Every writer is a game-thread path (create/Lock/Unlock/finalize). |
| `lastUsedSeqId` per record | `arena-protected` | Stamped via `markStampUpper` under the arena mutex; monotone max. |
| `nextSeqId_` | `owner-published` | Writers under the queue mutex with release stores; lock-free acquire reads via `markTicketAcquire()` paired with the re-stamp protocol. |
| `completedSeqId_` | `queue-shared` today; T2c targets `owner-published` (atomic) | Written by completion, read by map fast path. |
| Writing slot contents | `worker-owned` between `ensureWritingSlot` and publish, EXCEPT the producer's map-wait force-publish | The exception is why unlocked appending is unsafe without the reserve-copy-commit protocol (T2d, model required). |
| Retainer pins / warm epochs | `producer-owned` (PE side) | Program-ordered release strictly after same-chunk marking; release can synchronously drive reclaim on the releasing thread. |
| Reclaim gate (`destroyPending` + watermark) | `queue-shared` | Three driving actors (producer, worker, completion), all under the queue mutex; the pin premise keeps marked records out of `destroyPending`. |

## 3. Bridge entry synchronicity classification

Taxonomy per R-BACK-43.1. The full per-entry table is maintained below;
class totals summarize it. (Initial classification 2026-08-21; a new entry
must be added here in the change that introduces it.)

The wire surface is the five `device_c_bridge_*.cpp` forwarder files
(`dxmt9c_device_commit_chunk` is defined there too, at
`device_c_bridge_device_state_draw.cpp:291` — see the correction note below);
the `dxmt9p_*` bodies are the same implementations compiled under macro
renaming (`device_c_provider_macros.hpp`/`device_c_provider_undefs.hpp`), so a
`dxmt9c_*` occurrence in a provider `.cpp` that only includes
`device_c_provider.hpp` (no undef) is not a real wire symbol — it compiles to
`dxmt9p_*`. **161 distinct symbols**, mechanically counted by
`scripts/check/audit_bridge_entry_classification.py`
(`dxmt9-bridge-entry-classification-audit`), which also scans every other
`.cpp` under `src/` for a stray non-macro-renamed `dxmt9c_*` definition so a
future direct-entry addition outside the five files is caught. The
`device_c_chunk_replay.cpp:1548` line cited by the 2026-08-21 inventory as
"the one hot entry that IS its own wire symbol" is a provider-macro TU
(includes `device_c_provider.hpp` without the undef): its
`dxmt9c_device_commit_chunk` token compiles to `dxmt9p_device_commit_chunk`,
called by the real (and only) wire definition in the bridge file. The
inventory's prior **~89** total was consequently an undercount — real
per-entry granularity nearly doubles it, mostly because the desc/wire-identity
getter surface is wider than the original family table named (see the G1
resolution below). Drain mechanisms: global `DXMT9_DRAIN_OR_RETURN`
(cv-wait on offload queue depth), resource-scoped buffer-lock/unlock ledger
waits with NOOVERWRITE/DISCARD bypass classes, and the fail-fast
`DXMT9_TERMINAL_OR_RETURN` poison check.

| Family (entries) | Class | Blocking today | Freq (GT2) |
|---|---|---|---|
| Factory enumeration/caps/create (17) | `app-return-value` / `state-mutation-ack` (create_device2) | never-blocks (no drain — precedes the chunk pipeline; the whole file carries no drain macro) | adapter_count high; rest cold |
| Desc/count getters: texture level count/desc, buffer/surface get_desc, swapchain params, query size/type, shader bytecode, vdecl decl (9) | `app-return-value` | never-blocks (`DXMT9_TERMINAL_OR_RETURN` only — no `DXMT9_DRAIN_OR_RETURN`; resolved by G1, full-file confirmed) | mid |
| Wire-identity + navigational getters: texture/buffer/surface/shader/vdecl/query `get_wire_identity`, `texture_get_surface_level`, `surface_get_container_texture`, `swapchain_get_back_buffer`, `swapchain_get_depth_stencil` (10) | `app-return-value` | never-blocks (`DXMT9_TERMINAL_OR_RETURN` only) — same mechanism as the desc/count group but not named in the original 2026-08-21 family table | mid |
| Device-state getters: viewport/scissor/transform/material/render_state/TSS/sampler/clip/fvf/vs+ps consts/RT/DS/frame-latency (14) | `app-return-value` by contract, **executed as `ordering-fence`** (full global drain) | may-drain | mid |
| Buffer lock/unlock (2) | `visibility-wait` | resource-scoped cv (measured 0.105 ms/present blocked total, all plain-MANAGED; NOOVERWRITE/DISCARD bypass) | high (21.7 pairs/present) |
| Surface/texture lock/unlock, GetRenderTargetData, StretchRect/ColorFill/UpdateSurface/UpdateTexture, check_device_state/test_cooperative_level (11) | `visibility-wait` | global drain (surface lock's measured cost is unix CPU, not the drain) | low-mid |
| Scene markers: begin/end_scene (2) | `state-mutation-ack` | never-blocks (`DXMT9_TERMINAL_OR_RETURN` only — deliberately not drain-fenced; see the in-file comment) | high (2/present) |
| State setters + draws + present + reset + creates + texture ops + swapchain/query/stateblock ops + device-level check_device_multisample (73) | `ordering-fence` | global drain (R-BACK-2.51 single-FIFO invariant) | setters/draws high |
| addref/release pairs (20) | `state-mutation-ack` | mostly never-blocks; device addref/release pay the global drain wait with the result discarded | buffer pairs were 663×2/present pre-warm-epochs |
| Shader/vdecl create (3) | `state-mutation-ack` | terminal-check only (no queue-depth wait) — the one create family that does not drain | low-mid |
| `factory_create_device2` (1) | `state-mutation-ack` | never-blocks (no drain macro in `device_c_bridge_factory.cpp`) — writes `outDevice` and returns an HRESULT ack, unlike `factory_create`/`factory_create_device`'s bare pointer return | cold |
| `device_cancel_render_tape_present_capture` (1) | `state-mutation-ack` | never-blocks (no drain macro; a null check on `arg0`, then a fire-and-forget call) | cold |
| `commit_chunk` (offload path) (1) | `record-only` | enqueue + sync mark/capture, no drain unless the chunk carries a READBACK record (inline lane) | high (15.9/present) |

Class totals (conserved against the audit's 161): `app-return-value` 33,
`ordering-fence` 87, `state-mutation-ack` 27, `visibility-wait` 13,
`record-only` 1.

The full per-entry list lives in the fenced `classification` block
immediately below; it is the audit's source of truth. The table above is the
human-readable summary — keep both in sync when an entry's class changes or a
new `dxmt9c_*` wire symbol is added.

```classification
dxmt9c_buffer_addref state-mutation-ack
dxmt9c_buffer_get_desc app-return-value
dxmt9c_buffer_get_wire_identity app-return-value
dxmt9c_buffer_lock visibility-wait
dxmt9c_buffer_release state-mutation-ack
dxmt9c_buffer_unlock visibility-wait
dxmt9c_device_addref state-mutation-ack
dxmt9c_device_begin_scene state-mutation-ack
dxmt9c_device_begin_state_block ordering-fence
dxmt9c_device_cancel_render_tape_present_capture state-mutation-ack
dxmt9c_device_capture_render_tape_color_snapshot ordering-fence
dxmt9c_device_capture_render_tape_d24x8_snapshot ordering-fence
dxmt9c_device_check_device_multisample ordering-fence
dxmt9c_device_check_device_state visibility-wait
dxmt9c_device_clear ordering-fence
dxmt9c_device_color_fill visibility-wait
dxmt9c_device_commit_chunk record-only
dxmt9c_device_create_additional_swap_chain ordering-fence
dxmt9c_device_create_cube_texture ordering-fence
dxmt9c_device_create_cube_texture_shared ordering-fence
dxmt9c_device_create_depth_stencil ordering-fence
dxmt9c_device_create_index_buffer ordering-fence
dxmt9c_device_create_index_buffer_shared ordering-fence
dxmt9c_device_create_offscreen_surface ordering-fence
dxmt9c_device_create_pixel_shader state-mutation-ack
dxmt9c_device_create_query ordering-fence
dxmt9c_device_create_render_target ordering-fence
dxmt9c_device_create_state_block ordering-fence
dxmt9c_device_create_texture ordering-fence
dxmt9c_device_create_texture_shared ordering-fence
dxmt9c_device_create_vertex_buffer ordering-fence
dxmt9c_device_create_vertex_buffer_shared ordering-fence
dxmt9c_device_create_vertex_declaration state-mutation-ack
dxmt9c_device_create_vertex_shader state-mutation-ack
dxmt9c_device_create_volume_texture ordering-fence
dxmt9c_device_create_volume_texture_shared ordering-fence
dxmt9c_device_draw_indexed_primitive ordering-fence
dxmt9c_device_draw_indexed_primitive_up ordering-fence
dxmt9c_device_draw_primitive ordering-fence
dxmt9c_device_draw_primitive_up ordering-fence
dxmt9c_device_end_scene state-mutation-ack
dxmt9c_device_end_state_block ordering-fence
dxmt9c_device_finish_render_tape_present_capture ordering-fence
dxmt9c_device_finish_render_tape_present_source_capture ordering-fence
dxmt9c_device_get_caps ordering-fence
dxmt9c_device_get_clip_plane ordering-fence
dxmt9c_device_get_depth_stencil ordering-fence
dxmt9c_device_get_fvf ordering-fence
dxmt9c_device_get_material ordering-fence
dxmt9c_device_get_maximum_frame_latency ordering-fence
dxmt9c_device_get_ps_const_f ordering-fence
dxmt9c_device_get_render_state ordering-fence
dxmt9c_device_get_render_target ordering-fence
dxmt9c_device_get_render_target_data visibility-wait
dxmt9c_device_get_sampler_state ordering-fence
dxmt9c_device_get_scissor_rect ordering-fence
dxmt9c_device_get_swap_chain ordering-fence
dxmt9c_device_get_swap_chain_count ordering-fence
dxmt9c_device_get_texture_stage_state ordering-fence
dxmt9c_device_get_transform ordering-fence
dxmt9c_device_get_viewport ordering-fence
dxmt9c_device_get_vs_const_f ordering-fence
dxmt9c_device_light_enable ordering-fence
dxmt9c_device_negotiate_command_chunk ordering-fence
dxmt9c_device_present ordering-fence
dxmt9c_device_release state-mutation-ack
dxmt9c_device_reserve_render_tape_present_capture ordering-fence
dxmt9c_device_reset ordering-fence
dxmt9c_device_reset_ex ordering-fence
dxmt9c_device_set_clip_plane ordering-fence
dxmt9c_device_set_depth_stencil ordering-fence
dxmt9c_device_set_fvf ordering-fence
dxmt9c_device_set_indices ordering-fence
dxmt9c_device_set_light ordering-fence
dxmt9c_device_set_material ordering-fence
dxmt9c_device_set_maximum_frame_latency ordering-fence
dxmt9c_device_set_pixel_shader ordering-fence
dxmt9c_device_set_ps_const_b ordering-fence
dxmt9c_device_set_ps_const_f ordering-fence
dxmt9c_device_set_ps_const_i ordering-fence
dxmt9c_device_set_render_state ordering-fence
dxmt9c_device_set_render_target ordering-fence
dxmt9c_device_set_sampler_state ordering-fence
dxmt9c_device_set_scissor_rect ordering-fence
dxmt9c_device_set_stream_source ordering-fence
dxmt9c_device_set_stream_source_freq ordering-fence
dxmt9c_device_set_texture ordering-fence
dxmt9c_device_set_texture_stage_state ordering-fence
dxmt9c_device_set_transform ordering-fence
dxmt9c_device_set_vertex_declaration ordering-fence
dxmt9c_device_set_vertex_shader ordering-fence
dxmt9c_device_set_viewport ordering-fence
dxmt9c_device_set_vs_const_b ordering-fence
dxmt9c_device_set_vs_const_f ordering-fence
dxmt9c_device_set_vs_const_i ordering-fence
dxmt9c_device_stretch_rect visibility-wait
dxmt9c_device_test_cooperative_level visibility-wait
dxmt9c_device_update_surface visibility-wait
dxmt9c_device_update_texture visibility-wait
dxmt9c_device_wait_for_vblank ordering-fence
dxmt9c_factory_adapter_count app-return-value
dxmt9c_factory_addref state-mutation-ack
dxmt9c_factory_check_device_format app-return-value
dxmt9c_factory_check_device_format2 app-return-value
dxmt9c_factory_check_device_multisample app-return-value
dxmt9c_factory_check_device_type app-return-value
dxmt9c_factory_create app-return-value
dxmt9c_factory_create_device app-return-value
dxmt9c_factory_create_device2 state-mutation-ack
dxmt9c_factory_enum_adapter_modes app-return-value
dxmt9c_factory_get_adapter_display_mode app-return-value
dxmt9c_factory_get_adapter_identifier app-return-value
dxmt9c_factory_get_adapter_luid app-return-value
dxmt9c_factory_get_adapter_mode_count app-return-value
dxmt9c_factory_get_adapter_monitor app-return-value
dxmt9c_factory_get_caps app-return-value
dxmt9c_factory_release state-mutation-ack
dxmt9c_query_addref state-mutation-ack
dxmt9c_query_get_data ordering-fence
dxmt9c_query_get_data_size app-return-value
dxmt9c_query_get_type app-return-value
dxmt9c_query_get_wire_identity app-return-value
dxmt9c_query_issue ordering-fence
dxmt9c_query_release state-mutation-ack
dxmt9c_shader_addref state-mutation-ack
dxmt9c_shader_get_bytecode app-return-value
dxmt9c_shader_get_wire_identity app-return-value
dxmt9c_shader_release state-mutation-ack
dxmt9c_stateblock_addref state-mutation-ack
dxmt9c_stateblock_apply ordering-fence
dxmt9c_stateblock_capture ordering-fence
dxmt9c_stateblock_release state-mutation-ack
dxmt9c_surface_addref state-mutation-ack
dxmt9c_surface_get_container_texture app-return-value
dxmt9c_surface_get_desc app-return-value
dxmt9c_surface_get_wire_identity app-return-value
dxmt9c_surface_lock_rect visibility-wait
dxmt9c_surface_release state-mutation-ack
dxmt9c_surface_unlock_rect visibility-wait
dxmt9c_swapchain_addref state-mutation-ack
dxmt9c_swapchain_get_back_buffer app-return-value
dxmt9c_swapchain_get_depth_stencil app-return-value
dxmt9c_swapchain_get_present_params app-return-value
dxmt9c_swapchain_present ordering-fence
dxmt9c_swapchain_release state-mutation-ack
dxmt9c_texture_addref state-mutation-ack
dxmt9c_texture_generate_mip_sublevels ordering-fence
dxmt9c_texture_get_level_count app-return-value
dxmt9c_texture_get_level_desc app-return-value
dxmt9c_texture_get_surface_level app-return-value
dxmt9c_texture_get_wire_identity app-return-value
dxmt9c_texture_lock_rect visibility-wait
dxmt9c_texture_release state-mutation-ack
dxmt9c_texture_sample_2d ordering-fence
dxmt9c_texture_set_lod ordering-fence
dxmt9c_texture_set_palette ordering-fence
dxmt9c_texture_unlock_rect visibility-wait
dxmt9c_vdecl_addref state-mutation-ack
dxmt9c_vdecl_get_declaration app-return-value
dxmt9c_vdecl_get_wire_identity app-return-value
dxmt9c_vdecl_release state-mutation-ack
```

**G1 — resolved (2026-08-21 audit, full-file read of
`device_c_bridge_resources.cpp` and
`device_c_bridge_swapchain_query_stateblock.cpp`).** The desc/count getters
carry no `DXMT9_DRAIN_OR_RETURN` — confirmed at file:line for every entry
named in the 2026-08-21 inventory:

| Entry | File:line | Macro present |
|---|---|---|
| `texture_get_level_count` | `device_c_bridge_resources.cpp:129-132` | `DXMT9_TERMINAL_OR_RETURN` only |
| `texture_get_level_desc` | `device_c_bridge_resources.cpp:134-137` | `DXMT9_TERMINAL_OR_RETURN` only |
| `buffer_get_desc` | `device_c_bridge_resources.cpp:192-195` | `DXMT9_TERMINAL_OR_RETURN` only |
| `surface_get_desc` | `device_c_bridge_resources.cpp:221-224` | `DXMT9_TERMINAL_OR_RETURN` only |
| `swapchain_get_present_params` | `device_c_bridge_swapchain_query_stateblock.cpp:84-87` | `DXMT9_TERMINAL_OR_RETURN` only |
| `query_get_data_size` | `device_c_bridge_swapchain_query_stateblock.cpp:113-116` | `DXMT9_TERMINAL_OR_RETURN` only |
| `query_get_type` | `device_c_bridge_swapchain_query_stateblock.cpp:118-121` | `DXMT9_TERMINAL_OR_RETURN` only |

No entry's class or blocking column needed correction — "no drain macro"
meant no `DXMT9_DRAIN_OR_RETURN` (the cv-wait that can block), and that holds
for all seven. Each does still carry `DXMT9_TERMINAL_OR_RETURN`, the fail-fast
poison check (a non-blocking `replayTerminal()` load, not a wait) — the
original row's "never-blocks" wording is accurate, but "no drain observed"
undersold that a cheap check is still present, so the table above now says
"`DXMT9_TERMINAL_OR_RETURN` only" instead. The same full-file read surfaced
ten more entries with the identical no-drain mechanism that the 2026-08-21
family table never named — the six `get_wire_identity` getters plus
`texture_get_surface_level`, `surface_get_container_texture`,
`swapchain_get_back_buffer`, and `swapchain_get_depth_stencil` — now recorded
as their own family row above and in the classification block. `shader_get_bytecode`
and `vdecl_get_declaration` (in `device_c_bridge_shader_vdecl.cpp`, not part of
this G1 file pair) were already covered by the original desc/count row and are
unaffected.

**Open classification items** (from the 2026-08-21 inventory; each is a
review obligation, not a resolved fact):

- **G2 — RESOLVED (2026-08-21 source audit): no migration.** Of the 14
  drained device-state getters, **nine are dead on the steady-state path** —
  the public D3D9 `Get*` answers entirely from the PE shadow and never
  crosses (viewport, scissor, material, clip_plane, fvf, vs/ps consts,
  max-frame-latency, plus RT/DS in their cached common case; material and
  clip_plane additionally have no-op unix stubs). **Five have a genuine
  shadow-miss fallback crossing** (render_state, TSS, sampler_state,
  transform, and RT/DS's uncached branches — DS's `!dsSurfaceExplicit_`
  being the likeliest live one), and there the read target is
  replay-mutated state, so the drain is **load-bearing when reached**.
  Measured GT2 cost of all 14 combined: zero (absent from the opcode table
  and the drain-site breakdown). Verdict: classification stands as written
  (`app-return-value` by contract, executed as `ordering-fence`); drain
  removal is neither safe on the live fallbacks nor worth anything on the
  dead paths. The get_swap_chain precedent was different in kind: that
  getter crossed every call.
- **G3 — RESOLVED (2026-08-21 source audit): by design.** The shader/vdecl
  create bodies are pure value construction — bytecode parse/hash/copy into
  a fresh standalone `D9CShader`/`D9CVertexDecl`, with **no** `d->iface`,
  `d->dev()`, pool, or queue access (`device_c_shader_vdecl.cpp`) — so the
  family has no replay-ordering surface and `DXMT9_TERMINAL_OR_RETURN` is
  the correct minimal contract (refuse creation on a poisoned pipeline,
  wait for nothing). Resource creates differ in kind: they enter the core
  device/pool (`d->iface->CreateTexture` etc.) and keep the conservative
  `ordering-fence`. The asymmetry is the taxonomy working as intended.
- **G4 — RESOLVED (2026-08-21 source audit): by design, no change.** The
  inventory's "paying a fence they cannot act on" was wrong in both
  directions. `dxmt9c_device_addref` has **zero PE callers** (the wrapper's
  COM AddRef is PE-local) — it never pays anything; classify cold/dead.
  `dxmt9c_device_release` has exactly **one** caller, the PE device
  destructor (once per device lifetime), where the drain IS the
  teardown-ordering guard — the device must not be released with deferred
  replay pending — and the discarded result is intentional fail-open so a
  poisoned pipeline cannot block teardown (same policy as the shader/vdecl
  lifetime entries). Measured GT2 cost: absent from the opcode table.
- **G5** — the state-setter family's `high` frequency is inferred from class
  totals, not per-opcode measurement.

## 4. Ordering protocols

- **Arena-stamp + frozen-ticket re-stamp** (`R-BACK-43.6` reference): a mark
  ticket read lock-free may be stale against a concurrent slot advance;
  under the re-acquired queue mutex the ticket is frozen, so one re-read plus
  a conditional monotone re-stamp is a fixed point. TLA+:
  `ProducerMarkReclaim!WorkerRestamp`; Buggy axis `RestampDiscipline`.
- **Pin-ordering**: retained refs strictly contain the same chunk's marking
  window (program order on the committing thread); pins prevent
  `destroyPending`, the watermark gates reclaim after pins drop. TLA+:
  `PinDiscipline` axis.
- **Stamps-before-capture**: a low stamp is repairable (monotone max); an
  early capture is not — capture never precedes the marking of the same
  commit.

## 5. Verification mapping

| Contract | Evidence |
|---|---|
| R-BACK-43.4/43.6 mark/reclaim ordering | `ProducerMarkReclaim.tla` (+ 2 counterexample cfgs), shared predicates `canReclaimRecord`/`markStampUpper`, `dxmt9-producer-mark-reclaim-spec` |
| R-BACK-43.5 thread-affinity asserts | `assertRecorderThreadConfined` (reference shape); shared helper adoption tracked in gap.md |
| Queue-mutex contention observability | `DXMT9_PERF_QUEUE_MUTEX_SPLIT` per-site acquire/hold/segment rows |
| C++ memory-order obligation | OPEN — deterministic interleaving harness (R-VERIF-7.3 direction), gap.md |
