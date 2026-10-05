---
domain: present-pacing
workload: STALKER CoP capture29
title: "Shared prepared input with fresh-device actual-Metal repetitions"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-inprocess-19/verification.md; experiments/output/stkcop-v4-inprocess-19/{l0-r1,l3-r1,l64-r1,l64-r2,l3-r2,l0-r2,l0-legacy-single}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-metal-timing.435.md; specs/experiments/harness/replay/spec.md
---

# Actual-Metal repetitions without repeated input preparation

The private V4 diagnostic prepares capture29 once per process and executes one
fixed policy with fresh factories/devices. Each iteration independently proves
original source/output bytes, EVENT readiness and lifecycle gates. A failed
iteration rejects the entire set; successful prefixes grant no authority.
The prepared owner and retained loader copy both remain alive: this is reuse,
not a zero-copy claim. WarmDevice admission remains closed.

L0, L3 and L64 each pass two warm-ups plus 16 measured executions on identical
executable, loaded images, input components and plan digest. All 54 executions
retain 2,799 compatibility draws, 80 batches, zero unbatched draws and zero GPU
errors. The legacy single-run invocation also passes on the same binary.

| Policy | Encode chunks | Replay wall median (ms) | EncodeChunk median (ms) | EncodeChunk min–max (ms) |
|---|---:|---:|---:|---:|
| L0 | 5 | 358.838 | 10.781 | 9.039–12.286 |
| L3 | 174 | 333.352 | 23.105 | 21.658–36.837 |
| L64 | 12 | 326.622 | 10.839 | 10.247–12.372 |

Warm-ups are excluded. These are elapsed, overlapping scopes, not thread CPU
or FPS. Replay wall excludes input preparation, factory creation and device
teardown; runtime EncodeChunk spans the complete fresh-device lifetime and
includes pass setup and PSO work. The opposing L3 movements prohibit treating
EncodeChunk as the whole replay critical path. L64 and L0 encode ranges overlap.
No exclusive GPU/wait/encoder-only attribution is available.

Preparation takes 4.24–4.28 seconds once per set. Every row repeats that same
one-time clock reading; summing it would invent work. This removes a practical
iteration bottleneck in the diagnostic, not a demonstrated live-game cost.

The reverse L64→L3→L0 matrix also passes all 54 executions, giving 108 total
on identical images/input. Reverse replay medians are respectively 326.887,
335.133 and 358.902 ms; encode medians are 10.927, 22.239 and 10.323 ms.
L0 replay differs by only 0.018% between sets, whereas its encode median differs
by about 4.25%. Thus the opposing L3 replay/encode movements survive order
reversal, but this does not isolate their cause.

Uncontrolled process/driver caches and background load still prevent policy
promotion. This is not randomized paired execution or warm-device A-B-A.
Warm labels do not establish warm-device state closure. Next is narrower cost
attribution; warm-device A-B-A, numeric occlusion and live qualification remain
separate gates. Production defaults are unchanged.

## Next attribution boundary: repeated V4 admission

Code inspection confirms `replayPreparedRenderTapeIdentity` in
`src/d3d9/device_c_render_tape_provider.cpp` rebuilds
`CaptureSourceReplayPlanV4` and admits a temporary `RenderTapeExecutionV4`
before the replay wall clock. `replayGeneralPlan` then admits a second execution
inside that clock. `RenderTapeExecutionV4::admit` in
`src/d3d9/device_c_render_tape_execution_v4.cpp` validates the General-plan
binding and constructs query/record/event facts. Shared input preparation does
not currently eliminate these repeated derived-plan passes.

This is confirmed duplicate work, not measured attribution for the policy
delta. Admission owns mutable execution state as well as immutable facts;
reusing an already executed owner across fresh devices would violate that
boundary. The next measurement must separate pre-effect plan/admission and
in-body admission from actual device execution before caching or removing
validation. Live renderer cost must not be inferred from diagnostic preflight.

An immediate lower-risk attribution step is exporting existing
`RenderTapeQueryReplayEvidence` clocks: `checkpointSetupWallNanos`,
`transcriptWallNanos` and `oracleWallNanos`. The diagnostic's `printQuery`
currently omits them. No new hot-path clock is needed to distinguish these
query scopes. They remain diagnostic elapsed measurements, not additive CPU
components. A later single-admission handoff must preserve evidence lifetime:
`admit` retains a pointer to result evidence while `replayGeneralPlan` currently
takes its result by value. Passing the outer admitted executor without an
explicit evidence-owner repair is not safe.
