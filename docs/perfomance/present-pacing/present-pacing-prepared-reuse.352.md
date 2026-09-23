---
domain: present-pacing
workload: STALKER CoP Day
title: "H352: Reuse private prepared-plan storage; clean wild retry completed"
type: evidence
status: partial-attribution
updated: 2026-09-22
source: experiments/output/prepared-reuse-h352/{native-final.log,x86-tests.log,tla.log,h350-backcompat.json,h351-backcompat.json,stalker-on-runner.log,stalker-r2-runner.log,stalker-r2.csv,stalker-r2.json,stalker-r3-runner.log,stalker-r3.csv,stalker-r3.json}; experiments/output/app-d3d9-stkcop-bench-prepared-reuse-h352-r2-day/{result.json,actual.png}; experiments/output/app-d3d9-stkcop-bench-prepared-reuse-h352-r3-day/{result.json,actual.png,daybenchmark.result}
related: present-pacing-prepared-screen.351.md
---

# H352: Reusable diagnostic storage

Parent `6e762d18` plus workspace reuse. `PreparedEncodePlanWorkspace` moves
one private allocation into a builder, then consumes the sealed owner or
invalidated builder after synchronous views expire. It caches bytes, not source
metadata or resource lifetime. Acquire resets key, counts and resolution flag;
append overwrites all counted entries. No full-array clear is required on reuse.

The ordinary shadow owns one cold thread-local workspace only behind its
existing observer gate and pre-allocation screen. One idle 149,576-byte region
can remain until thread exit. No Slot fields, PE wire changes, async handoff,
production policy changes or increased identity bounds are introduced.

## Deterministic verification

- 125 three-step histories compare reused and fresh builders across canonical,
  unresolved, partial-capacity, incomplete and invalid-key outcomes. Failed
  prefixes cannot append/seal; recycling consumes both successful and failed
  owners. Allocation failure, stale keys and owners outliving the workspace
  are separately covered.
- The real native queue OFF/ON fixture preserves publication order/counts and
  skip reasons. Its 18 candidates allocate once and reuse 17 times.
- Native plan/shadow/supply tests: 3/3 pass. x86_64 plan/shadow/install-name
  tests: 3/3 pass. TLA regression passes in 237.11s. Unix and both PE canonical
  builds pass. This is synchronous storage refinement, not proof of future
  async publication or GPU consumption.
- H350 and H351 historical traces remain readable. New traces separate new
  allocation bytes from private storage capacity; missing historical capacity
  is unavailable, not an inferred reuse count.

## Actual-input mechanism evidence only

Day/high, renderer_r2.5, 1280x720, Sikarugir cx24.0.7 perf, identity partition;
same observer configuration and encoded Present window 2450–2470 as H351.
Unix artifact SHA-256 prefix `93e1a378410d393f`; five staged hashes are retained
in the result. No builds/tests ran alongside the benchmark.

The first attempt blocked before Wine execution in the Codex-bundled `rg`
launch. Only those attempt processes were stopped. The retry used the existing
Nix `rg` via PATH without changing security settings or approving the launch
dialog. Its screenshot captured the macOS `rg` confirmation dialog, not the
game; the runner reports no matching onscreen STALKER window. It ultimately
timed out (`returncode=-15`, elapsed 460.927s including capture wait).
The game process is no longer running. **This is a failed wild run, not visual,
FPS, progress or promotion qualification.** The dialog is observed interference;
its presence alone does not prove the entire timeout's cause.

The bounded trace nevertheless closes completely with zero overflow/incomplete
writers. These counts establish only the observed allocation mechanism:

| Item | Result |
|---|---:|
| Qualified sources | 1,220 |
| Query-scope / no-parallel-work skips | 1,157 / 21 |
| Preparation attempts | 42 |
| Structural successes / capacity rejections | 21 / 21 |
| New allocations / reuses | 1 / 41 |
| Cumulative new allocation | 149,576 bytes |
| Private capacity per attempt | 149,576 bytes |
| Capacity-rejected attempts using reused storage | 20 |
| Total diagnostic CPU | 20.582 ms |
| Candidate diagnostic CPU | 9.073 ms |
| Acquire/initialization CPU | 0.111 ms |
| Projection / seal CPU | 7.640 / 0.344 ms |

The first attempted source is a capacity rejection; later sources reuse its
invalidated private storage. This is non-vacuous failure-path reuse evidence.
Zero reported GPU command-buffer errors does not override the failed wild run.

H351 had 42 allocations (6,282,192 bytes), but its input population and runtime
conditions differ. Total diagnostic CPU here increased from H351's 11.282 ms;
do not call the allocation reduction a total CPU or FPS improvement. The
allocation phase includes reset and clocks; retained capacity is not RSS. The
total excludes a per-source free because storage is now retained until thread
exit. No native benchmark result or FPS comparison is accepted for this run.

## Clean retry (2026-09-22)

After the user cleared the dialog, `r3` ran commit `1dbf6bd6` with the same
configuration and all five staged artifact hashes identical to `r2`. No builds
or tests ran alongside it. Runner PASS, return code 0, no timeout or capture
error, elapsed 228.948s, and zero reported GPU command-buffer errors. The
150s screenshot is an actual STALKER scene with no obvious black/repeated
geometry. A single screenshot is not a full-run flicker or pixel-exact oracle.

The 2450–2470 trace closes with zero overflow/incomplete writers:

| Item | Clean retry |
|---|---:|
| Qualified sources | 1,249 |
| Query-scope / no-parallel-work skips | 1,141 / 21 |
| Preparation attempts | 87 |
| Structural successes / capacity rejections | 66 / 21 |
| New allocations / reuses | 1 / 86 |
| Cumulative new allocation | 149,576 bytes |
| Total / candidate diagnostic CPU | 11.828 / 5.377 ms |
| Acquire / projection / seal CPU | 0.131 / 4.069 / 0.228 ms |
| Native benchmark average | 20.982122 FPS |

Normal completion and an in-scene capture close the prior single-run integration
blocker. Reuse remains non-vacuous on real input. The candidate population is
87 rather than H351/r2's 42; neither the timing nor FPS is a fixed-input A/B
improvement claim. Async consumption and pixel-exact equivalence remain open.

## Next gate

Measure exact rejected-source identity cardinality before
choosing right-sized storage or raising a bound. Repeated projection/lookup,
joint Slot-plan ownership and independent Encode differential remain open;
this diagnostic optimization does not itself remove production Encode work.
