---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "GT2 replay owners: snapshot and Slot materialization ahead of planning"
type: experiment
status: diagnostic-attribution
updated: 2026-09-16
source: experiments/output/gt2-replay-owners-20260916-r624.noindex/campaign.json; experiments/output/gt2-replay-owners-20260916-r624.noindex/campaign-complete.json; experiments/output/gt2-replay-owners-20260916-r624.noindex/stack-analysis.json; experiments/output/gt2-replay-owners-20260916-r624.noindex/owner-audit.json; experiments/output/gt2-replay-owners-20260916-r624.noindex/audit_owners.py; experiments/output/gt2-replay-owners-20260916-r624.noindex/analyze_stacks.py; experiments/output/gt2-replay-plan-counters-20260916-r625.noindex/campaign.json; experiments/output/gt2-replay-plan-counters-20260916-r625.noindex/campaign-complete.json; experiments/output/gt2-replay-plan-counters-20260916-r625.noindex/load-time-gate.json
related: docs/perfomance/mini-replay-bisection/cpu-phase-attribution.10.md; docs/perfomance/mini-replay-bisection/gt2-qualified-policy-matrix.09.md; specs/experiments/harness/replay/fixed-input-experiment.md
---

# GT2 replay cost owners

## Verdict

The strongest remaining owner is **draw state/uniform snapshot construction**,
followed by **final Slot append**, not capacity planning. Direct exact's
source-driver samples attribute 38.1% to snapshots, 21.0% to append, 9.5% to
resource closure/mark/capture, 4.0% to planning and 3.6% to Slot admission.
Existing scoped counters support that ordering. This identifies candidates
in the unchanged instrumented implementation, not proven removable work.

Keep exact as the baseline. This investigation does not justify more headroom,
removing lifetime checks, changing defaults, or claiming an FPS improvement.
No runtime source, binary, renderer policy or pixel oracle was changed.

## Frozen input and qualification

- HEAD `9d20a99e`; all five CPU/provider/validator/loaded-library images match
  [r622/r623](cpu-phase-attribution.10.md). The audit rehashes them, the r405
  original and every staged input, including blob and identity components.
- Same 185 captured source boundaries, plan SHA-256
  `cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`,
  1,866 draws, 656 source-summary commands and 294 created/released objects.
  Every iteration completes/reclaims all 185 sources. Exact preserves 1,856
  Direct / ten compatibility draws, 182 leases and one SoA growth.
- r624: four semantic verification iterations, 32 ordinary retained samples,
  244 diagnostic retained samples and 24 warm-ups. Six diagnostic processes
  were sampled externally: four exact, two compatibility. r625 adds four
  verification, 112 diagnostic retained samples and fourteen warm-ups.
  **434 iterations total**, including all warm-ups and inert diagnostic runs.
- All eight fresh verification iterations match the prior ordered semantics.
  Diagnostic receipts remain `accepted=false`, `diagnostic=true`,
  `execution_valid=true`; they fail ordinary policy-measurement acceptance.
  The audit checks the exact requested diagnostic environment separately from
  the frozen semantic binding; it does not treat changed observer knobs as
  ranking-equivalent environments.
- Prior r622 exact-route GPU evidence is reauthenticated, **not rerun**.
  Original source/output RGBA remains
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
- CPU lane: x86_64 release/O3 through Rosetta, fresh resources each iteration,
  CPU-visible Metal resources but no encoding/GPU submission, controlled
  immediate completion. Host/thermal snapshots are retained, not proof of
  an idle host or fixed clock frequency. No samples are discarded.

The before/after ordinary exact medians are 70.673/69.642ms per whole
iteration. They bracket diagnostics; their difference is not an optimization.
Sampled exact replay phase means are 16.049–16.194ms, consistent with the
earlier ~16ms envelope. Setup/resource reconstruction remains outside it.

## Source-driver sample attribution

`/usr/bin/sample <owned-provider-pid> 3 1 -mayDie` runs after observing the
specific CLI child. Retained command/PID/header/exit evidence authenticates
all six samples. The analyzer subtracts each node's immediate child counts,
checks sample conservation, then assigns exclusive self samples by ancestry.

The denominator below is **1,017 main-thread samples under
`replayPrevalidatedResolvedCommandChunkBySources`, excluding
`SourceSettlementContext::invoke` and its harness publication/completion**.
It is neither whole-process time nor all of the ~16ms replay envelope. Import/
resolve/settlement work outside that ancestry is not silently assigned to a
production owner. Unknown Rosetta descendants stay with their nearest named
owner; inlining and short sample windows limit precision.

| Exclusive owner | Samples | Share |
|---|---:|---:|
| Draw state/uniform snapshot | 387 | 38.05% |
| Direct final Slot append | 214 | 21.04% |
| Resource closure/mark/capture | 97 | 9.54% |
| Source publication/lifecycle, excluding grouped closure | 73 | 7.18% |
| Planning | 41 | 4.03% |
| Direct Slot admission | 37 | 3.64% |
| State undo journal | 6 | 0.59% |
| Compatibility append | 3 | 0.29% |
| Other source-driver work | 159 | 15.63% |

Compatibility's 517 source-driver samples similarly put snapshots first
(35.0%), compatibility append next (26.3%) and planning much lower (2.3%).
These percentages do not establish a Direct/compatibility speed ratio.

Named clock stacks occur in **178/1,017 exact samples (17.5%)**, overlapping
the owner rows. This is observer work inside the measured path, not an extra
additive cost or a proven 17.5% recoverable gain. Do not multiply the shares
by 16ms to manufacture owner timings.

## Counter cross-check and actual mechanisms

Numbers below divide process cumulative counters by **all eighteen
iterations, including two warm-ups**. They are not retained-sample medians.
Despite `_cpu_ms` names, these production scopes use `steady_clock` elapsed
time; they are not the harness's process-CPU clock. Parent/child scopes
overlap and must not be summed into an exclusive CPU decomposition.

### 1. Snapshot/materialization

Ordinary r624 brackets report 5.795–5.849ms for snapshot hit plus miss scopes.
Within batch misses: uniform build 1.966–1.977ms, hot-state build
1.890–1.894ms, shader layout 0.479–0.527ms. Render-state flattening alone is
1.174–1.178ms **inside** hot-state build. Uniform/hash child timers overlap
uniform-build parents; they are not additional milliseconds.

`src/d3d9/core_draw.cpp`, `submitDirectReplayDrawFromCurrentState`, requests
`cachedBaseDrawStateForSubmissionBatch` each draw, then borrows cached hot
state, shader layout and full uniforms for `DirectReplayDrawInput`. It does
not rebuild everything on every draw: there are 514 batch misses, comprising
422 full uniform builds, 88 nonconstant reuses and four full payload reuses.
Other refresh paths bring uniform-build calls to 883 per iteration.
`refreshFlatDrawStateRecordFromState` rebuilds prioritized flat render state
unless its category-generation reuse proof holds.

The largest owner is therefore repeated state/uniform projection and its
cache/observer work. It is not evidence that queue reservation itself is the
main cost. Snapshot generation dependencies remain correctness authority;
the old GT1 stale-binding failures forbid treating pointer/hash equality as
a replacement for generation-qualified reuse.

### 2. Final Slot append and repeated uniform decomposition

`src/dxmt9/dxmt9_source_payload.cpp`,
`TransactionalChunkSlotAssembler::tryAppendDirectDraw`, compares run hot
state/layout, accounts for capacity and appends new hot/layout data only when
the run changes. It still calls `ChunkSlot::appendDrawUniformPayload` for
every Direct draw. The borrowed append capability does not reacquire the queue
lock for each draw.

Per iteration the Slot counters report 1,862 uniform payload-record appends,
184 fixed components, 870 VS components and 259 PS components; reported
append bytes total 2,732,736. The records are already component-deduplicated,
not 1,862 full independent uniform copies. Named child scopes are:

| Slot uniform scope | Elapsed ms/iteration, ordinary brackets |
|---|---:|
| Fixed-component lookup | 0.850–0.853 |
| VS-component lookup | 0.296–0.297 |
| PS-component lookup | 0.073–0.074 |
| Fixed/VS/PS component append, combined | 0.256–0.266 |
| Payload-record reserve/copy/lookup-link, combined | 0.190–0.191 |

These selected scopes total about 1.68ms; they are not all Direct append
work. Run compatibility, parameter/payload append and bookkeeping also exist.

A concrete duplicate representation remains:
`core_draw.cpp` already caches `uniformFixedPayload`, but Direct input carries
only full uniforms. In `src/dxmt9/dxmt9_backend_types.hpp`,
`findDrawUniformFixedPayload(const DrawUniformPayload&)` reconstructs the
fixed projection, then checks the last component and linearly searches fixed
records with hash **and content** comparison. A miss reconstructs that
projection again during fixed append. The 0.85ms lookup is a candidate scope,
not a promise that passing the cached projection removes its entire cost.

### 3. Resource closure is real, but ingress locking is not dominant here

Three mark-split processes agree on ingress dedup 0.138–0.139ms, core
capture/mark 0.139–0.141ms and snapshot sorting 0.024–0.025ms per iteration.
They visit 4,493 handles, including 2,822 buffers. Queue-acquisition wait is
0.0022–0.0024ms, **included in core time**, not an additional charge.
`prepare_slot_resource_mark_cpu_ms` adds a separately scoped ~0.587ms
publication scan, but these figures do not cover all resource work.

`src/d3d9/device_c_chunk_replay.cpp`,
`persistResolvedResourcesAndCaptureBindings`, owns ingress closure and buffer
backing capture. `src/dxmt9/dxmt9_command_queue.cpp`,
`commitDirectChunkSlotReplay`, also marks raw closure under the exact span seq,
visits only newly appended commands and constructs `ArenaResourceRetainDigest`.
Those commit activities appear in stacks without a complete separate timer.
Do not report ingress plus publication as the complete closure cost or delete
later-span marking: lifetime pinning and final GPU-use watermarks have different
authority. This workers-disabled lane cannot dismiss live queue contention.

### 4. Planning is smaller; enabling child timers changes the answer

r625 total-only observations are **0.8995/0.9101ms per iteration across all
instrumented planner calls**. There are 371 calls and 5,603 records per
iteration, matching two passes over the 185-source/2,801-record stream plus
bootstrap. Harness `admitCaptureSourcePlan` plans ranges before replay, and
the production source driver plans them again. This aggregate is not all
production replay cost, and it must not be assigned to the ~19ms admission
phase either. The sample ancestry isolates production planning at ~4%.

Work-only mode reports 5,589 capacity-delta computes and 11,178 applications,
357,696 dimension checks and the same number of adds, with no application
failures. Planner elapsed increases to 1.123ms. Adding per-record child
timers raises it to 2.056ms; diagnostic replay rises from 15.73–15.76ms in
off controls to 16.81ms. Use total-only for coarse cost and work-only for
counts; the timed subdivision is materially observer-sensitive.

The first r624 total-only attempts (runs 09/11) were **inert**, not zero-cost
planning. `kReplayEmissionPlanSplitEnabled` in
`src/d3d9/device_c_cpu_ready_plan.cpp` initializes at library load; native
main sets `DXMT_PERF_COUNTERS=1` later. r625 explicitly exports that variable
before launch in every control and diagnostic run. Counters become nonzero
only in the requested planner modes. The old receipts/logs remain preserved;
their zero planner values are excluded from timing conclusions. No runner
or runtime gate was patched for this investigation.

## Next bounded experiment and residuals

The first implementation candidate should reuse the cached fixed-uniform
projection at the borrowed Direct append boundary, then evaluate whether a
**Slot-owner/generation-qualified component reuse witness** can avoid repeated
lookup. Slot replacement, rollback, source changes and A→B→A must invalidate
or revalidate that witness; a hash, address or old component index alone is
not authority. This is smaller than redesigning planning or weakening closure.

The larger follow-up is snapshot category-generation reuse and redundant flat
state/uniform reconstruction. Any behavior change needs pure transition/native
stale-generation and rollback counterexamples, applicable refinement evidence,
same-source CPU semantics and exact GPU qualification before ordinary A/B.
First retain an observer-off cost check: nested clock/atomic instrumentation
is itself visible in this image. No optimization or performance gain has yet
been demonstrated by this owner investigation.

Still open: a complete exclusive split of wrapper import/resolve/settlement
versus production work, precise commit-closure time, uninstrumented service
cost, warm-resource reset fidelity, asynchronous residency/pacing and repeated
wild A/B on broader inputs. This is current bounded cost attribution, not
independent attribution of the historical v0.1.1 performance regression.
