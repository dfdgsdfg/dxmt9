---
domain: present-pacing
workload: STALKER CoP; PE to Unix command ingress
title: "H393: Ownership handoff already exists; provisional marking still couples ingress to the queue"
type: source-audit
status: candidate-proof-open
updated: 2026-09-27
source: src/d3d9/device_c_chunk_replay.cpp; src/d3d9/device_c_replay_offload.cpp; src/dxmt9/dxmt9_command_queue.cpp; src/dxmt9/dxmt9_resource_pool.hpp; specs/verification/tla/ProducerMarkReclaim.tla
related: present-pacing-stalker-cpu-recheck.392.md; present-pacing-mark-ticket-coverage.386.md
---

# H393: Separate ingress ownership from execution sequence coverage

Source audit at `fd2fafa5`; no runtime change, new benchmark, or safety
qualification. [H392](present-pacing-stalker-cpu-recheck.392.md) motivates the
investigation but its observer-inclusive waits are not removable-cost estimates.

## Existing ownership handoff

`device_c_replay_offload.cpp` / `prepareOffloadChunk` creates Unix-owned wire
storage and resolves/retains wrappers before accepting work. Segmented import
has a corresponding owned preparation path. `device_c_chunk_replay.cpp` /
`dxmt9c_device_commit_chunk_impl` transfers release authority on queue adoption: both Accepted
and EffectUnknown mean the queue owns the chunk; RejectedPreEffect does not.
EffectUnknown poisons the ledger rather than permitting an ordinary retry.

The worker publishes replay completion before clearing backing snapshots and
releasing retained wrappers. This is not GPU completion. Actual execution-use
stamps protect subsequent GPU use. Ordinary asynchronous command acceptance
does not require an Encode result to return to PE. Present pacing, synchronous
readback, query visibility, mutation fences and bounded-queue backpressure
remain separate reasons an application call can wait.

| Responsibility | Current mechanism | Boundary that permits release |
|---|---|---|
| PE transport bytes and chunk-local retention | Unix owned import and retained wrappers | Successful handoff, not GPU completion; not permission to destroy a still-live PE COM object |
| Unix object existence during queued replay | Raw retained wrappers | Replay consumption/failure cleanup |
| Versioned buffer backing identity | `ChunkBufferBindingSnapshot` residency | Replay consumption, followed by execution-use protection |
| Mutable non-versioned data visibility | Ordered mutation/drain and map slow path | Relevant prior consumers have finished; a pin alone is insufficient |
| GPU resource reuse/reclaim | Final sequence stamps and completion waterline | Actual execution completion |

## What the measured queue acquisition does

`CommandQueue::markChunkResourcesAndCaptureBufferBindings` performs:

1. Acquire-load a provisional mark ticket and bulk-mark resources outside the
   queue mutex, using arena locks.
2. Acquire the queue mutex, re-read the ticket, and re-stamp if it advanced.
3. Unlock, then capture buffer bindings under arena protection.

Backing capture is **already outside the queue mutex**. Describing this
acquisition as a backing-capture lock would misidentify the remaining work.
The capture-only method does not acquire the queue mutex at all.

`persistResolvedResourcesAndCaptureBindings` currently selects capture-only
when `supportsCpuReadyArenaReplay()` is true and combined marking/capture
otherwise. The `resourcesMarkedBeforeReplay` flag connects this decision to
`markLegacyResources`. Compatibility separators, missing layout, inline replay
and provider ranges have deferred-mark call sites; strict execution has its
own reserved-ticket publication. Their existence is useful precedent, not an
all-family proof that every ingress can skip marking.

## Candidate and outstanding proof

The candidate is **capture/retain-only ingress independent of execution
policy**, followed by Replay-owned final sequence coverage. It does not require
moving COM lifetime or Metal objects across the ABI, nor removing resource
lifetime protection. Prefer existing raw ownership and residency over a second
duplicated retention representation.

Before changing the production branch:

- Prove queue adoption publishes each resource's replay dependency before any
  later mutation can observe acceptance; cover rejection, EffectUnknown,
  inline replay, stop, poison and reset.
- For versioned backing, preserve capture identity and residency across rename.
  For non-versioned buffer/texture/surface writes and readback, prove the
  mutation fence reaches prior replay **and** the resulting execution sequence.
  Object existence alone does not protect old contents.
- Prove every direct, compatibility, draw-free and ordered-control consumer
  establishes final-use coverage before raw pins/residency can be released,
  including source splits and partial-effect failures.
- Reconcile the model with production: `ProducerMarkReclaim!BeginMark`
  increments `nextSeqId` and reserves `commitSeqId`; production
  `seqIdForMark` is only an acquire load, followed by the frozen-ticket repair.
  The producer reservation abstraction does not license deleting that repair.
- Add deliberate stale-ticket, early-pin-release, backing-reuse and
  mutation-before-consumption counterexamples, with native production-path
  bindings. Then qualify identical-input GPU output and an interference-free
  wild run before promotion.

Measure producer mark-ticket acquisition removal separately from any added
Replay marking and arena contention. H392's late 5.73 ms/Present acquisition
scope includes observation and overlap; it is neither a predicted FPS gain nor
proof that all this time disappears.

Decision: do not remove the final queue acquisition in isolation. The useful
next implementation unit is the ingress-to-Replay lifetime/visibility contract
and its model-code tests, not another backing-capture lock optimization.
