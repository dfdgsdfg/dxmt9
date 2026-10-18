---
domain: present-pacing
workload: STALKER H502/r5 actual compatibility new-run inputs, warmed private construction
title: "Actual-input owned-run construction separates a roughly half-microsecond local contrast"
type: evidence
status: local-cpu-contrast-measured-live-budget-unconfirmed
updated: 2026-10-16
source: traces/stkcop-owned-run-local-h600/{contract.md,probe.hpp,prepare.py,derivation.json,fixture.hpp,native.cpp,build.py,build.json,run.py,runs.json,codegen.py,codegen.json,analyze.py,analysis.json,verify.py,verify-initial.py,verifier-correction.md,verification.json}; traces/stkcop-owned-run-local-h600/{O0,P0,P1,O1}.log; traces/stkcop-owned-run-local-h600/{P0,P1}.csv
related: present-pacing-owned-run-live.599.md; present-pacing-owned-run-construction.597.md; present-pacing-owned-run-warm-replay.598.md; present-pacing-replay-cpu-owner.596.md
---

# Constructor contrast measured; the live frame budget remains separate

H600 compares construction on **actual captured STALKER inputs**, not synthetic
state timing. Both process orders show approximately 0.36–0.66us/run mean
contrast after endpoint allowances. Reference construction is approximately
1.21–1.37us/run and the H597 candidate is 0.71–0.86us/run in this warmed,
reserved scratch workload. These are constructor-loop CPU values, not live
Replay savings or an FPS estimate.

The frozen conservative screen nevertheless fails: the mean treatment lower
bounds do not exceed the largest individual sham bound. Preserve that result.
Do not replace the predeclared screen with an aggregate/sham-mean comparison
after seeing the data, or extend this population to obtain a preferred verdict.

## Actual-input and ownership boundary

The private image instruments the new-run branch of
`src/d3d9/core_draw.cpp`, `Device::submitCompatibilityReplayDrawBatched`.
The real output always uses the existing reference construction. A fixed hash
of new-run ordinal selects 67 of 2,343 entries in each original capture replay.
Each selected call borrows its current state/layout/debug/uniform/binding only
for the synchronous observer; both scratch paths construct owned runs.
No scratch result is published, encoded, or retained across observer calls.

Each private vector reserves 64 runs. The reference and candidate each build
64 outputs, compare all semantic fields, then clear. Reservation, output
comparison, destruction and logging lie outside the timed batch. Capacity and
base addresses are checked unchanged. Default initialization, layout ownership,
loop dispatch and helper calls inside construction remain timed. This is a
hot no-growth workload, not the production cache-residency distribution.

For each input, O/O sham and O/N treatment each receive two warmup pairs and
sixteen measured pairs. First-arm order alternates. P1 reverses both first-arm
and sham/treatment order relative to P0. Both processes select the same ordinals
and shader/mask metadata. This metadata match is not a full cross-process byte
identity assertion; both use the same hash-bound original capture and effect
oracle, and each pair itself receives the exact same immutable references.

Unlike [H598](present-pacing-owned-run-warm-replay.598.md), the renderer executes
the original capture only once per process. Only scratch construction repeats.
Fixed control order is O0,P0,P1,O1, with no replacements or extensions.

## CPU results

Caller-self `proc_pidinfo` CPU reads are bracketed by monotonic wall reads.
The batch lower subtracts both read durations and 4us; the upper adds4us.
Bounds are divided by64. Zero lower bounds remain valid, and no outliers are
removed. Each process has 1,072 pairs per comparison mode, or 68,608 measured
constructions per arm per mode. Each input has equal pair count.

| Process | Treatment reference CPU, us/run | Candidate CPU, us/run | Reference-first mean reduction | Candidate-first mean reduction |
|---|---:|---:|---:|---:|
| P0 | 1.213–1.357 | 0.707–0.851 | 0.361–0.650 | 0.362–0.650 |
| P1 | 1.223–1.367 | 0.713–0.859 | 0.363–0.653 | 0.365–0.656 |

The O/O order-group mean contrast bounds span approximately −0.145 to+0.151
us/run. Both treatment order-group means have positive lower bounds. However,
the **maximum individual** O/O bound is 1.567us/run in P0 and 0.484us/run in P1.
Both exceed the treatment mean lower bounds, so neither process clears the
frozen sham screen. The screen compares aggregate treatment with worst-case
individual variation; its failure is not evidence that the local mean contrast
is zero. Any different acceptance design must be declared before new data.

## Effect and generated-code verification

- ARM64 and x86_64 fixtures use the actual helper and a hash-bound extraction
  of the production run declaration. Both orderings, semantic values, retained
  references, stable reserved addresses and injected reserve failure pass.
- Four original controls pass exact source/output pixel SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
  seven source mappings, query transcript, 670 wrapper lifetimes and normal
  state restoration. Product counters remain OFF.
- Optimized x86_64 assembly retains three 8,112-byte hot copies in the isolated
  reference, one in the candidate, and seven candidate zero-fill calls. This
  preserves the key H597 mechanism; it does not make the isolated call site
  instruction-for-instruction identical to production. The same changed core
  object is linked into the dynamic provider and static capture executable.
- Eleven malformed-trace tests reject missing completion/rows, duplicates,
  wrong owner/thread/order/range, bad times and wrong run size.
- An independent verifier imports no analyzer. It reproduces all 8,576 raw
  CPU rows, selection/count/order checks and comparison arithmetic, verifies
  the image/input hashes and effect controls, and confirms no remaining runtime.

The initial verifier falsely counted a preceding memcpy's size from the
disassembly context. The preserved initial script and correction note explain
the fix: use the last size-register assignment before the current call.
Timing data, binaries and comparison criteria did not change.

## Implication for the original objective

The constructor mechanism now has an actual-input, order-reversed local price,
consistent in direction with [H599](present-pacing-owned-run-live.599.md).
It is not the entire compatibility preparation cost. Do not multiply it by
H596's sampled construction counts or a cold-capture route ratio to assert
default-live removable ms/frame. Different storage/cache residency and live
frequency remain transfer conditions.

Further broad FPS repetitions for this constructor are not the priority.
[H596](present-pacing-replay-cpu-owner.596.md) still leaves cache-miss processing
and final queue batch as overlapping larger CPU candidates. Resolve those
owners with operation-local CPU evidence, retaining the already measured
constructor contrast separately. The largest default-live single-thread owner
and a qualified live removal amount remain open; no product change, promotion
or commit follows from H600.
