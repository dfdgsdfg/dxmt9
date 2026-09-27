---
domain: present-pacing
workload: deferred ingress native ownership and backing validation
title: "H395: Parked-worker handoff and backing-to-sequence protection"
type: validation
status: native-composition-partial-promotion-open
updated: 2026-09-27
source: tests/native/backend/cpu_ready_production_routing_spec.cpp; tests/native/backend/dynamic_rename_ring_spec.cpp; build-x86_64-builtin/meson-logs/ingress-h395.txt; src/dxmt9/dxmt9_command_queue.cpp; src/d3d9/device_c_bridge_resources.cpp
related: present-pacing-ingress-deferred-marks.394.md
---

# H395: Acceptance does not wait for Replay to establish ownership

Candidate implementation committed as `d37935e5`. This follow-up adds native
composition tests; runtime policy and the default-off gate are unchanged.

## Actual worker handoff

`deferredIngressAdoptionAndFailureKeepRawOwnership` parks the real offload
worker before dispatch through its existing test seam. Two raw entries are
accepted with CPU-ready planning disabled. Before either executes, both
wrappers have one additional Unix reference and both resource-scoped replay
drain targets are pending, while use stamps remain zero. The temporary caller
wire vectors have already been destroyed when Replay resumes.

For both ordinary Direct and Compatibility, exercise:

| Schedule | Checked outcome |
|---|---|
| Both raws succeed | Real `replayRawChunk` runs twice; use marks exist while raw refs remain held; both extra refs settle |
| First callback fails before dispatch | Neither source has replay effects; successor is discarded, not retried; refs settle |
| First callback fails after real dispatch | First source has use coverage before release; successor remains unstamped; worker fail-stops and refs settle |
| Queue refuses before adoption | Import retention unwinds locally, no replay target or use stamp is invented |

The six accepted-work schedules include worker stop/join settlement; the two
refusal schedules require no worker. Condition-variable ordering controls the
interleaving; the five-second deadline is a hang guard, not a timing assertion.
The injected after-dispatch error tests the worker cleanup boundary, not every
internal partial-effect failure site or a real device Reset.

## Physical backing handoff

The rename tests now execute both combined-mark and capture-only admission:

- Captured G0 remains the resolver's binding after the live buffer moves to G1.
- Capture-only leaves the logical use stamp zero; raw residency alone prevents
  DISCARD from reusing G0 even when prior sequence 1 is complete.
- Final-use stamping explicitly targets captured G0 at sequence 7, not live G1.
- After raw residency release, completion 6 still forbids G0 reuse; completion
  7 permits it. The simulated completion watermark is monotone (1, 6, 7).
- Missing mandatory versioned backing rejects in both admission modes.

These drive production Pool/resolver methods using synthetic handles. They do
not allocate GPU resources or validate actual rendered bytes/pixels.

## Source audit: final-use owners

`dxmt9_command_queue.cpp` retains independent use coverage after ingress:

- Direct span commit marks the raw closure with `ticket.seqId`, then visits
  newly appended payload commands and captured bindings.
- Compatibility draw batches mark binding snapshots/resources at their
  execution sequence; final Slot publication also walks its resource uses.
- Ordinary Clear, SurfaceCopy, StretchRect, ColorFill, DepthResolve and
  GenerateMipmaps append and mark through the queue-locked submit methods.
  Their active Arena/Direct branches delegate to their source transaction.
- Readback retains its synchronous path and resource marks. Query completion
  retains its separate submission/marker receipt protocol.
- Buffer Lock retains the scoped replay fence before backend visibility checks;
  texture/surface content operations and Reset retain bridge drains.

This is an owner map, not exhaustive all-family execution proof. Shader/decl
ownership, query/state-only behavior, texture/surface backing bytes, reset and
all partial-effect interleavings must not be inferred from the buffer fixtures.

## Result and next boundary

x86_64 baseline routing, candidate routing and extended rename tests pass
(3/3). `git diff --check` passes. Existing TLA regression evidence remains H394;
no TLA model or runtime protocol is changed by this follow-up.

Remaining: all-family final-use/failure coverage and GPU exact qualification,
then an interference-free STALKER OFF/ON comparison. Keep the candidate OFF by
default. No FPS or removed-CPU-cost claim follows from these tests.
