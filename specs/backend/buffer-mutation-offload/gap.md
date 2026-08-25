---
type: gap
title: Managed Buffer Mutation Offload — Gap
description: Implementation and evidence status for R-BACK-44.x (designed, not implemented).
tags: [backend, buffers, producer-concurrency, offload]
---

# Managed Buffer Mutation Offload — Gap

Status of R-BACK-44.1..44.8. The topic is designed only; nothing is
implemented. Sizing evidence:
`docs/perfomance/present-pacing/present-pacing-bridge-crossing-decomposition.237.md`
(Managed re-uploads `1.19ms/present` on the saturated GT2 producer thread;
`.38`'s `>=0.5ms/Present` mutation-stream design gate is met).

A 2026-08-25 external design review (codex `gpt-5.6-sol`/high; findings
re-verified against source) returned "implement with changes": four
blockers resolved by design amendments (V1 scope narrowed to plain Managed
writable locks; reserve/rotate/commit admission transaction; encode-side
snapshot-sourcing promoted to a prerequisite; task backing lease) and five
majors folded into the requirements. Rows below reflect the amended
design.

| Area | Status | Evidence / missing |
|---|---|---|
| **PREREQUISITE: encode-side versioned byte readers snapshot-sourced (R-BACK-44.4a)** | missing — pre-existing latent race, fix independent of this mode | The encode index staging call lacks the `!indexSnapshot` guard the vertex path has (`StreamIbStagingCache::findOrStage` call in `dxmt9_draw_encoder_draw.mm`), and diagnostic index readers read `indexRecord->shadow` live; the R-BACK-2.51(d) drain waits on `lastReplayedSeq` only, which does not cover encode-time reads, so today's synchronous Managed unlock can already race an encode-time staging read under the engine-default index-cache opt-in. Fix + audit of all versioned live-byte readers must land before offload mode. |
| Queue reservation API (reserve/commit/release) in `ReplayOffloadQueue` | missing | R-BACK-44.2's transaction; staged bytes charge `DXMT9_OFFLOAD_QUEUE_BYTES` bounds and release on every reject/stop/teardown path. |
| Task backing lease (R-BACK-44.2a) | missing | Concrete ring-entry retention (handle, pointer, generation, replay residency) honored by destroy/GC. |
| R-BACK-2.51(d) amendment (fourth admission form (iv)) | missing | Must land in `specs/backend/requirements.md` in the same change that implements the mode. |
| Formal model `BufferMutationOffload.tla` + production cfg (R-BACK-44.3/44.4) | missing | Visibility and snapshot-revision invariants stated in `spec.md` §6; model not written. Must compose with, not weaken, `BufferBackingVersioning.tla` (R-BACK-5.11). |
| Counterexample configs (R-BACK-44.6 via R-BACK-43.6) | missing | FIFO-position removal and deferred-rotation configs must fail with the named invariant; wire into `verify_tla.sh` `counterexample_models`. |
| Shared pure predicates + native spec | missing | Predicates header (admission, FIFO position, direct-reader fence) plus a `dxmt9-buffer-mutation-offload-spec` following the `dxmt9_mark_reclaim_predicates.hpp` binding pattern. |
| FIFO task variant in `ReplayOffloadQueue` | missing | Queue carries only `RawCommandChunk` today; needs the two-alternative element and worker dispatch (`src/d3d9/device_c_replay_offload.hpp`). |
| Synchronous-half split in `Buffer::unlock` / `Pool` | missing | Stage + logical rotate + enqueue; ledger publication for mutation tasks; env resolver `DXMT9_MANAGED_MUTATION_OFFLOAD`. |
| Bridge synchronicity reclassification (`dxmt9c_buffer_unlock`) | pending design acceptance | Today `visibility-wait` in `specs/backend/producer-concurrency/spec.md` §3; offload mode drops the pre-mutation drain for the Managed case. Requires the R-BACK-43.2 procedure. |
| Direct-reader fence coverage (R-BACK-44.5) | missing | Extend `ReplayDrainTarget` publication to mutation tasks; audit that shared-buffer export waits; enumerate any other live-byte direct readers. |
| Mechanism counters | missing | Enqueued/applied tasks, staged bytes, worker apply CPU; producer-side delta visible in existing `d3d9_buffer_unlock_*` family. |
| Wild promotion evidence (R-BACK-44.8) | missing | Conformance, GT1/GT3/SFIV visual anchors, GT2 matched A/B with producer-wall/locality/error gates. Default stays off until green. |
