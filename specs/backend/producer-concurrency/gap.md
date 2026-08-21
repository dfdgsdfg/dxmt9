---
type: "Gap Tracker"
title: "Producer Concurrency Gaps"
description: "Adoption and evidence gaps for the bridge synchronicity classification and thread-ownership contracts."
tags: [specs, backend, producer-concurrency, gap]
---

# Producer Concurrency — Gaps

Domain-owned tracker for `requirements.md` (R-BACK-43.x). Root rollup:
[../../gap.md](../../gap.md).

| Area | Status | Evidence / missing |
|---|---|---|
| Bridge entry classification table (R-BACK-43.1) | classification complete + mechanically audited | `scripts/check/audit_bridge_entry_classification.py` / `dxmt9-bridge-entry-classification-audit` covers 161 symbols and the five bridge forwarder files. |
| `record-only` de-synchronization (R-BACK-43.2) | G2 resolved: no getter migration | The 2026-08-21 source audit found no drained entry with a justified record-only migration. Reopen only if a profiler identifies one. |
| Ownership declarations at field level (R-BACK-43.4) | implemented for the audited set + mechanically audited | Rename mutation and commit-time capture are documented as `arena-protected` under HandleArena unique/`inspect` locking; binding publication remains the immutable commit-time snapshot. `scripts/check/audit_thread_ownership_declarations.py` / `dxmt9-thread-ownership-audit` checks token declarations and declaration-site drift. The audit is lexical and does not replace the HandleArena lock proof. |
| Shared thread-affinity assert helper (R-BACK-43.5) | implemented for thread-confined state | The shared token/assert helper serves the PE recorder and writing-slot guard. The Pool rename/capture transaction is not a first-thread-affine adopter: its guard is HandleArena `update`/`inspect`. `D3DCREATE_MULTITHREADED` producer ordering remains serialized by the recorder mutex. Induced-failure declaration-audit controls are the negative-control evidence; there is no longer a Pool-specific inverted-assert claim. |
| T2b capture off the queue mutex | implemented; cross-workload **operational** evidence restored, counter profile still GT2-only | Capture runs under HandleArena's own shared lock and retains the same retainer-pin, generation/sequence, and snapshot publication obligations. `ProducerMarkReclaim` covers `NoCaptureAfterFree` and the capture counterexample. [Append decomposition .32](../../../docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md) preserves the GT2 mechanism report; [.33](../../../docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.33.md) withdrew the missing-artifact GT1/GT3 claim. Restored 2026-08-21 by the surviving sweep at `8ddfe5fa` (which contains T2b/c, `c9511f31`): `experiments/output/app-d3d9-3dmark05-final-gt1b`, `-final-gt2`, `-final-gt3`, and `experiments/output/app-d3d9-sfiv-benchmark-final-sfiv` — all four `status=pass` with `gpu_command_buffer_errors=0`, recorded in [wild FPS refresh .02](../../../docs/perfomance/baselines/baselines-wild-fps-refresh.02.md). That covers "the change does not break other workloads"; it does NOT carry `DXMT9_PERF_QUEUE_MUTEX_SPLIT` profiles, so the per-site wait/hold reduction remains measured on GT2 only. |
| T2c map DISCARD fast path (`completedSeqId_` → owner-published atomic) | implemented; cross-workload **operational** evidence restored, counter profile still GT2-only | The acquire/release publication and HandleArena-protected map transaction are covered by native/model evidence. `.32` preserves the GT2 mechanism report. The same four `final-*` sweep runs cited in the T2b row above are the restored cross-workload pass/GPU-error evidence at a T2c-containing build; the map-path acquire counters remain GT2-only. |
| T2d reserve-copy-commit slot append | deferred; prior cross-workload verdict is not locally reproducible | The architectural option and bounded half-appended-slot model requirement remain. `.33` withdraws the missing-artifact multi-workload gate, so the prior “no waiting victim anywhere” conclusion is not a current accepted proof. Reopen from a surviving producer-wait profile or when encode becomes the pacer. |
| C++ memory-order harness (R-BACK-43.6 residual) | implemented, with scope statement | `dxmt9-producer-interleaving-spec` drives production queue/pool symbols and translates the model traces. The scripted lane proves protocol order, not arbitrary memory order. TSan is configured by `tests/meson.build` with `TSAN_OPTIONS=halt_on_error=1:abort_on_error=1`; the production release/acquire pair and weakened-order negative control are covered, while the harness remains a deterministic evidence layer rather than a proof of all compiler/CPU behaviours. |
| Constant-promotion complexity review (R-BACK-43.7) | process rule active; known instance closed in code | The append/retainer/chunk dedup structures use open-addressed tables. `appendHandle` uses `RecordLocalDedupTable` for its normal O(1) path, preserving the identity/pointer integrity check and retaining the original bounded scan after table overflow. `dxmt9-pe-chunk-record-value-spec` pins same-record hits, cross-record misses, rollback, overflow, and the identity-collision failure; `.33` intentionally makes no performance claim for it. |
| Raw `findBuffer` live views | closed: every residual read proven or converted (2026-08-22) | `HandleArena::find` takes a shared lock only while resolving the generation-qualified slot, then returns a pointer-stable view. The 2026-08-22 field-by-field audit below covers every `Pool::findBuffer` call site outside `dxmt9_resource_pool.cpp` (plus the sibling `findTexture` live-view sites reached the same way). No site relies on the arena mutex being held across a read. See the audit table for the per-field classification and the sequencing argument (`hasVersionedBacking()` + T2b's `MissingRequired` chunk-commit gate + `CommandQueue::mapBuffer`'s FAST/SLOW lane split) that makes the residual production reads `immutable-after-init` or `sequence-ordered` rather than racing the rename ring. |

### `findBuffer`/sibling live-view audit (2026-08-22)

Enumerates every call site reached through `Pool::findBuffer` (and the two
`findTexture` sites with the identical shape) outside `dxmt9_resource_pool.cpp`
itself. "Production" means the read feeds the Metal command stream / draw
data; "diagnostic" means the read is reachable only when an off-by-default
env knob (`traceEncode`, `forceTrace`, `DXMT_FORCE_EXPAND_INDEXED`,
`DXMT9_DUMP_INDEXED_GEOMETRY_DIR`, `DXMT9_PERF_ENCODER_BREAKDOWN`) is set.

| Site | Fields read | Classification | Action |
|---|---|---|---|
| `dxmt9_render_pass_encoder.mm` `beginRenderPass` `considerBuffer`/`considerTexture` (buffer site, texture sibling) | `isHeapBacked`, `heap` | immutable-after-init — both fields are written exactly once, in `Pool::createBuffer`/`Pool::createTexture`, before the record is inserted into the arena; no other write site exists (grepped every assignment) | none required; documented in place |
| `dxmt9_blit_encoders.cpp` `considerTexture` | `isHeapBacked`, `heap` | immutable-after-init, same as above (sibling `TextureRecord`) | none required; documented in place |
| `dxmt9_draw_encoder_draw.mm` stream-0 bind, condition (`buffer->buffer` truthiness) | `buffer` (`WMT::Reference<WMT::Buffer>`) | was UNPROVEN: `.buffer` is `arena-protected`, rewritten by rename-ring rotation (`rotateBufferBacking`, under `bufferArena_.update()`'s unique lock) for records with `hasVersionedBacking()==true`; the old condition (`buffer->buffer` OR'd with a valid-snapshot check, buffer-truthiness evaluated first) evaluated the racy operand even when the snapshot already made the condition true | converted: reordered so the valid-snapshot check is evaluated first (short-circuiting away the live read whenever a valid commit-time snapshot exists). `Pool::captureChunkBufferBindings` (T2b) rejects the whole chunk (`MissingRequired`) before any draw reaches encode if a `hasVersionedBacking()` record lacks a valid snapshot, so `buffer->buffer` is now read only for non-versioned records |
| same site, non-debug fallback (`buffer->shadow`, `buffer->contents`, `buffer->desc.size`) | `shadow`, `contents`, `desc.size` | sequence-ordered: reached only when the snapshot produced no bytes, i.e. (by the same `MissingRequired` argument) a non-versioned record. `shadow`/`contents` mutate only under `bufferArena_.update()` from `uploadBufferData`/`uploadBufferDataRange`/`finalizeBufferMap`; `CommandQueue::mapBuffer` takes the SLOW lane (`waitSeq != 0`) for exactly this class of record and drains a sequence wait before calling into those mutators, ordering the mutation after any concurrent encode read. `desc.size` is immutable-after-init | documented in place; no code change |
| same site, `traceEncode` trace block (`buffer->buffer.handle`, `buffer->shadow.size()`, `buffer->contents`) | as listed | diagnostic-only (`traceEncode`) | documented in place (already gated) |
| `dxmt9_draw_encoder_draw.mm` FFP trace #1 (`forceTrace && !ffLayout` geometry dump), index bytes | `indexRecord->shadow`, `->buffer`, `->contents`, `->desc.size` | diagnostic-only | documented in place (already gated) |
| `dxmt9_draw_encoder_draw.mm` FFP trace #2 (`debug::fixedFunctionTraceTextureHandle()` gated), index bytes | same fields | diagnostic-only | documented in place (already gated) |
| `dxmt9_draw_encoder_draw.mm` extra-stream bind, condition | `buffer` | was UNPROVEN, same shape as stream-0 | converted: same reorder fix |
| same site, `liveMetalHandle`/`shadowBytes`/`hasContents` locals | `buffer->buffer.handle`, `buffer->shadow.size()`, `buffer->contents` | was diagnostic-only but read unconditionally (not gated by `traceEncode`) even though only used inside the `traceEncode` block | converted: moved the three reads inside `if (traceEncode)` |
| same site, stream-IB-staging condition (`extraRecord->buffer`) | `buffer` | sequence-ordered by the same `MissingRequired` argument (`!extraSnapshot` implies non-versioned record for a record that participates in T2b capture) | documented in place; no code change |
| `dxmt9_draw_encoder_draw.mm` `forceExpandIndexed` block, index bytes + `resolveStreamBytes` lambda | `indexRecord`/`buffer` `shadow`, `contents`, `desc.size`, `buffer` | diagnostic-only (`DXMT_FORCE_EXPAND_INDEXED` / `DXMT9_PROBE_FORCE_EXPAND_INDEXED*`, default off, correctness-invalid classifier per `environment_variables_encoder.rules.md`) | documented in place (already gated) |
| `dxmt9_draw_encoder_draw.mm` index-buffer bind, condition | `buffer` | was UNPROVEN, same shape as stream-0/extra-stream | converted: same reorder fix |
| same site, `needIndexBytesForDiagnostics` branch | `shadow`, `contents`, `desc.size` | diagnostic-only (`encoderBreakdownActive` or `indexedDiagnosticsEnabled`) | documented in place (already gated) |
| same site, `!buffer->shadow.empty()` CPU-only fallback (no live Metal backing) | `shadow` | sequence-ordered — same `mapBuffer` FAST/SLOW-lane drain argument as the stream-0 fallback; this branch is reached for SystemMem/Scratch-pool index buffers, which are never `hasVersionedBacking()` | documented in place; no code change |
| `dxmt9_draw_encoder_draw.mm` `dumpIndexedGeometryEligible` block, `resolveDumpStreamBytes` lambda | `shadow`, `contents`, `desc.size` | diagnostic-only (`DXMT9_DUMP_INDEXED_GEOMETRY_DIR`, unset by default per `environment_variables_capture.rules.md`) | documented in place (already gated) |

Sequencing argument used throughout: `CommandQueue::mapBuffer` (T2c) selects a
FAST lane (no sequence wait) only for NOOVERWRITE, MANAGED, dynamic-rename
DISCARD, or an unmarked buffer — exactly the `hasVersionedBacking()` /
rename-ring cases the commit-time `ChunkBufferBindingSnapshot` exists to
cover — and a SLOW lane (`waitForSequence`) for everything else, which drains
before `finalizeBufferMap`/`uploadBufferData*` can mutate `shadow`/`contents`/
`.buffer`. So for every record class reached by a raw `findBuffer` fallback
read, either (a) `hasVersionedBacking()` is true and a valid snapshot is
mandatory (T2b `MissingRequired` gate), or (b) `hasVersionedBacking()` is
false and the mutating Lock drains a sequence wait first. Per-draw binding-
snapshot *completeness* for a given stream (whether the producer always
attaches a `DrawBindingSnapshot` when a stream's record needs one) is T2b's
own promotion scope, not re-derived here.
| Restamp-fire observability | implemented | `mark_ticket_restamp_checks` / `mark_ticket_restamp_fires` measure the frozen-ticket window. No promotion claim is derived from a zero wild count. |
| Producer commit acquire not fully removed | open | T2b leaves the producer ticket acquire and frozen-ticket re-read in place. Removing the final acquire requires either a real reservation proof or a model/harness-backed sufficiency argument. |
