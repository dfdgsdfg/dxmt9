---
domain: present-pacing
workload: STALKER H502/r5 fixed original V4 input
title: "Compatibility batch partition: outer append owns CPU, not initial queue wait"
type: evidence
status: measured-envelope-qualified-append-scope-corrected
updated: 2026-10-16
source: traces/stkcop-batch-residual-h589/{contract.md,observer.hpp,native.cpp,native.json,parser-tests.json,prepare_build.py,build.json,run.py,report.json,verify.py,verification.json}; traces/stkcop-batch-residual-h589/{on0,on1}-batch.bin; traces/stkcop-batch-residual-h589/{off0,on0,on1,off1}/{census.json,source.bgrx,output.bgrx}
related: present-pacing-reserve-recurrence.588.md; present-pacing-replay-census.585.md; present-pacing-compat-append-phases.387.md
---

# Attribute the outer append before changing queue policy

**Scope correction:** [H590](present-pacing-append-nesting.590.md) finds that
the phase named Append includes post-callback capacity settlement and Tape
runtime observation. [H591](present-pacing-append-tail.591.md) separates them:
approximately6ms belongs to the verifier; actual append is about3–4ms CPU.
The historical numbers below remain valid for the broad envelope, not for
the append body alone or removable default-live rendering work.

H589 partitions ordinary compatibility batches in the original STALKER capture.
The outer append phase consumes **9.554–10.332ms / 9.260–10.041ms caller CPU**
in two observed executions. Initial queue acquisition spans only
0.119–0.209ms / 0.070–0.159ms wall, including CPU-read endpoint envelopes.
This fixed input does not reproduce a large initial mutex wait. The useful
next target is append work, not a lock-free rewrite or scratch cleanup.

All numbers here are **per complete captured interval**, not ms/live frame.
They are observer-inclusive bounds, not measured removable CPU or FPS gains.
H588 separately establishes that cold reserve growth does not recur as a
major cost in its warmed live tail; H589 does not repeat that transfer test.

## Frozen experiment and controls

Use H585/H586's H572 C baseline archive, replacing only the command-queue
object with a private phase-observed build. No product source, capacity,
ownership, command order, query behavior, lifetime or default policy changes.
The rendering transition model is unchanged; this is an observer, not a new
policy requiring a new TLA refinement. The observer is non-fatal and mmap-backed;
missing storage, read failures, overflow and incomplete batches invalidate
evidence without skipping or aborting rendering. ARM64/Rosetta native controls
cover absent/invalid storage, bounded overflow, false return, exception,
foreign queue identity and `_exit` durability. Parser positive and 14 negative
fixtures pass. Private build logs have no warnings; install-name audit passes.

Four fixed fresh-process executions run in order OFF0, ON0, ON1, OFF1, with
no retries or population extension. Product perf and inner append diagnostics
are OFF in all four; their zero counters are not zero-work evidence. OFF does
not produce private rows. All four preserve seven original source boundaries,
nine query issues, five polls, four results, five readiness checks, 670 wrapper
create/releases, and normal-state restoration. Both source/output BGRX match
SHA-256 `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
No Wine staging or game launch occurs.

Both ONs contain 84 successful batches, 2,828 draws, 2,343 input entries and
924 phase rows, with identical input/phase shapes and one qualified caller
thread/queue per process. Independent `verify.py` authenticates input/build/
image/log/readback hashes, decodes raw rows without importing the parser,
recomputes each bound, and proves phase CPU deltas telescope to batch totals.

## Disjoint phase ownership

Each phase boundary reads caller-self libproc CPU between two mach timestamps.
The ending sample is the next phase's beginning sample. Per-phase lower bounds
subtract both read extents and a 4us allowance; upper bounds add 4us. The root
batch uses only its first/last endpoint uncertainty, rather than adding every
phase's allowance. No Rosetta POSIX thread-CPU clock is used. Storage is
initialized/prefaulted before the first measured batch sample. Row bookkeeping
is charged to the next phase; observer CPU and scheduling perturbation are not
subtracted. Each phase has 84 observations.

| Phase, caller CPU bounds in ms | ON0 | ON1 |
|---|---:|---:|
| Whole batch, enclosing | 18.176–19.035 | 16.655–17.531 |
| Scratch/setup | 0–0.483 | 0–0.502 |
| Immutable entry/pair preparation | 1.252–1.896 | 1.267–1.906 |
| Initial queue acquisition | 0.001–0.411 | 0–0.393 |
| Binding capture | 0.065–0.578 | 0.072–0.595 |
| Slot readiness | 0–0.408 | 0.001–0.405 |
| Segment plan | 0.001–0.433 | 0.001–0.428 |
| Reserve | 5.302–5.820 | 4.186–4.702 |
| Resource marking | 0.573–1.219 | 0.467–1.111 |
| Append plus mutation-helper postprocessing | **9.554–10.332** | **9.260–10.041** |
| Commit checks | 0–0.427 | 0–0.410 |
| Epilogue/unlock/scratch cleanup | 0.001–0.388 | 0.002–0.398 |

Small-phase bounds are allowance-limited, not proof that each costs hundreds
of microseconds. Setup's body wall is 0.021/0.018ms; epilogue's is
0.018/0.021ms. Root wall envelopes are 19.786–19.973ms and 17.190–17.393ms.
Reserve still owns cold allocation CPU, consistent with H586, but H588 limits
its relevance to warmed live performance. Do not add root and child costs.

## What outer append does and does not establish

In `src/dxmt9/dxmt9_command_queue.cpp`, `submitDrawRunBatch`, outer append
includes command-append bookkeeping, destination lookup, query-range routing,
state argument construction, `appendDrawRun` or compatible extension, returned
temporary cleanup, emitted-run counters and current-backbuffer updates.
It also extends beyond the mutation callback to capacity settlement and
`observeTapeRuntimeNewCommands`; this omitted boundary is corrected by H590/H591.
`noteCurrentSlotCommandAppendStartedUnlocked` reads a clock only for an empty
Slot with no prior timestamp; it is not an unconditional per-entry timestamp.

`src/dxmt9/dxmt9_backend_types.hpp` takes `CanonicalDrawState` by value for new
runs and by const reference for compatible extensions. `ShaderBytecodeBytes`
copies shared ownership, not the shader byte vector itself. This is not evidence
of a deep shader copy per draw. `appendQueryRangeDraw` has its own transactional
preparation/reserve wrapper; its actual branch count is not measured here.

**Do not subtract H585's 3.83–3.96ms inner Append from these outer CPU bounds.**
H585 used another image/observer configuration and elapsed clocks. Its
whole-process append counter was about4.24ms, also smaller than H589's outer
phase. The difference remains an explicit attribution/perturbation question,
not proof that argument materialization costs approximately6ms. The same
source hash and original pixel/work controls establish provenance and semantics,
not unchanged compiler layout, cache behavior or observer overhead.

The next discriminant is same-image nested outer/inner observation plus
aggregate new-run/extension/query-range route counts, keeping a parent-only
control and caller CPU. This can separate Slot interning/state/payload work
from dispatch/argument/query preparation without a clock per draw. Only then
choose a removal, verify exact original semantics, and test whole-stage CPU
and warmed live transfer. No optimization or default promotion is justified
by the present local cost alone; the global CPU-owner/removal goal stays open.
