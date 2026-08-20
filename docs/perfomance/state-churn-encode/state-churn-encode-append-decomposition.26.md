---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 26
title: Post-Harvest Triage — Drain Is Solved, Mark Owns Commit, Locks Are Unix-Side Work
date: 2026-08-20
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-drain-sites-r3; experiments/output/app-d3d9-3dmark05-commit-phase-r1; experiments/output/app-d3d9-3dmark05-sampler-postgate
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.25.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.24.md
---

# Post-Harvest Triage — Drain Is Solved, Mark Owns Commit, Locks Are Unix-Side Work

Three GT2 diagnostics against the post-[.25] head (plus the
`notePeDeviceCallAfterPresent` gate, `1987f689`), sizing what remains of the
bridge ledger. All three are instrumented runs, not perf samples.

**1. The drain fence is no longer a story** (`drain-sites-r3`,
`DXMT9_PERF_DRAIN_FENCE_SITES=1`). Total blocked-drain cost is
**0.105 ms/present**, all of it `dxmt9c_buffer_lock` on **plain MANAGED**
locks (949 blocks / 176 ms over ~1,663 presents); DISCARD (15.7k) and
NOOVERWRITE (53k) bypass without blocking, and `dxmt9c_surface_lock_rect`
never appears as a blocked site at all. Two consequences: the [.207]-era
"MANAGED, DISCARD-skewed" picture is obsolete post-offload, and
`surface_lock_rect`'s 1.33 ms/call is **not** drain wait — it is CPU inside
the unix implementation (candidates from source: the DISCARD-path
`bytes.assign(slicePitch×depth, 0)` zero-fill, and the wow64 pointer-shadow
alloc/copy that every 32-bit lock takes). Attribution between those two is
the open follow-up.

**2. `mark` owns the commit_chunk sync half** (`commit-phase-r1`,
`DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT=1`, 27,698 calls / 1,761 presents).
Parent `offload_commit_app_cpu_ms` = 1.42 ms/present (90.5 µs/call); phases
sum to 88.5 µs/call (perturbation small):

| phase | µs/call | ms/present | share |
|---|---|---|---|
| mark | 56.5 | **0.888** | 62% |
| prepare | 23.7 | 0.373 | 26% |
| present_wait | 6.3 | 0.099 | 7% |
| enqueue | 1.9 | 0.031 | 2% |
| import | 0.1 | 0.001 | 0% |

[.05]'s "fixed ~22-23 µs/call" predates the cadence promotion; at 256-record
chunks the per-call cost tripled while calls fell, netting the same
~1.4 ms/present — and it is now attributable: the next increment is inside
`mark` (resource marking scales with records/handles; whether any of it can
move to the offload worker is the question to answer before touching it).

**3. Post-gate sampler** (`sampler-postgate`, info-level — logging artifacts
included). Module split: game **63.3%**, winemetal **14.8%** (was 16.8% in
[.23] — the [.25] harvest, direction consistent), d3d9 10.1%.
`notePeDeviceCallAfterPresent` fell 16.3% → 4.5% of top-32 self-PC
(~0.07 ms/present residual — the early return still zero-fills the
96-byte-plus `PePresentCallSample{}`; a call-site branch could recover the
rest, diminishing returns). New named rows at the top of `d3d9.dll` self:
`CommandChunkBuilder::referencesObject` 11.2% and
`D3D9PePendingCommandRetainer::retainWireObject` 4.1% of top-32 — the
linear arena scans, together ~0.25 ms/present. This is the trigger condition
recorded in `c7b3b141` ("if a wild run shows the scan eating the crossing
harvest"): it is not eating the harvest, but it is now a named, sized
candidate for the O(1) ptr→entry index.

**Corrected framing for the lock lane.** `factory_adapter_count` proved a
bare PE→unix crossing costs ~0.3 µs. `buffer_lock`/`unlock` at 52/73 µs and
`surface_lock_rect` at 1,334 µs are therefore ~99% unix-side execution, not
"round-trip price" ([.24]'s wording overstated the wire). The lock lane
(2.65 ms) responds to making the unix side cheaper (upload/copy staging,
zero-fill elision), not to crossing elimination.

**Value-ordered next increments:** ① `mark`-phase decomposition and
offload-feasibility (0.888 ms); ② `surface_lock_rect` CPU attribution —
zero-fill vs wow64 shadow (0.58 ms, likely a small fix if it is the
DISCARD zero-fill, which D3D9 semantics do not require); ③ retainer/builder
O(1) index (~0.25 ms); ④ `PePresentCallSample` call-site branch
(~0.07 ms, optional).
