---
domain: present-pacing
workload: STALKER CoP Day fixed optimizer inputs
title: "H365: Failed pass-coalescing search dominates the planning spike"
type: evidence
status: partial-attribution
updated: 2026-09-25
source: experiments/output/app-d3d9-stkcop-bench-optimizer-fixture-h365/{result.json,dxmt9.log}; traces/app-d3d9-stkcop-optimizer-h365/qualified/{result.json,0.csv,1.csv,2.csv,3.csv,0.stderr,1.stderr,2.stderr,3.stderr}; traces/app-d3d9-stkcop-optimizer-h365/inputs/optimizer-96859-140227595350704-{5877,5897,5899,5901}.fginput
related: present-pacing-encode-residual.364.md
---

# H365: The spike survives return diagnostics OFF

Base `72fce02e` plus a cold input observer and device-free probe. Production
optimizer decisions, pass order and attachment policies are unchanged. The
[fixture contract](../../../specs/backend/encode-scheduling/optimizer-fixture.md)
preserves every FrameGraph value, unlike the display-oriented DAG JSON.

## Qualification

Capture uses the catalogue STALKER Day/high/1280x720 Sikarugir lane, perf
profile and only `DXMT9_FRAMEGRAPH_OPTIMIZER_FIXTURE_DIR` for added observation.
Runner PASS, zero GPU command-buffer errors. Four input exports log
`complete=1`, after exact codec round trips. Capture changes scheduling/cache
state and is not an FPS sample. No Render Tape or GPU readback replay is claimed.
The observed sources are from process 96859, backend 140227595350704; these
identities are run-local, not globally meaningful sequence numbers.

The captured Unix image SHA prefix is `fb8d0725e86bb336`; unchanged PE32/PE64
prefixes are `4d30258d6e7d0c3e` / `cae268981dc6cc85`. The offline probe is
x86_64 on the same Apple M1/Rosetta environment. The final receipt records
SHA-256 of both the executable and the **actually resolved optimizer dylib**,
plus every input, checked unchanged across all trials. The initial `measured/`
screening block lacks the loaded-dylib hash and is not the primary evidence;
`qualified/` is the final remeasurement.

Each input uses two warmups per policy and sixteen paired measured rounds,
alternating OFF→ON and ON→OFF. Whole optimizer and split production stages
start from separate fresh copies of the same original graph. Parsing/copying,
file output and equality checks are outside timers. All 256 measured optimizer
executions (and 32 warmups) match the full OFF-reference graph and merge count,
including ordered command/draw refs, dependencies, resource accesses, flags and
load/store actions. This is optimizer-output equivalence, not whole-backend or
GPU-pixel equivalence. No build, test or game run overlaps offline measurement.

## Whole-optimizer native CPU medians

| Source | Commands | Passes / edges | Diagnostics OFF | Diagnostics ON | Extra diagnostic CPU |
|---|---:|---:|---:|---:|---:|
| 5877 | 431 | 98 / 175 | 7.775ms | 8.624ms | 0.849ms |
| 5897 | 529 | 134 / 229 | 27.602ms | 30.834ms | 3.233ms |
| 5899 | 785 | 133 / 223 | 26.157ms | 29.337ms | 3.180ms |
| 5901 | 787 | 133 / 223 | 26.129ms | 29.516ms | 3.388ms |

The diagnostic adds roughly 11–13% over OFF here; removing it would not remove
the spike. Unlike the earlier wild elapsed counters, these are qualified native
thread CPU measurements. They remain per-input optimizer budgets, not FPS gains.

## Phase ownership with diagnostics OFF

| Source | Initial lifetime | Pass coalescing | Load/store |
|---|---:|---:|---:|
| 5877 | 0.001ms | 7.758ms | 0.007ms |
| 5897 | 0.001ms | 27.595ms | 0.010ms |
| 5899 | 0.002ms | 25.979ms | 0.064ms |
| 5901 | 0.002ms | 26.015ms | 0.063ms |

Coalescing includes its production post-merge lifetime refresh. Tiny phases
are near the clock overhead/resolution floor; zero/sub-microsecond readings
are not proof of free work. Separate whole/split medians need not sum exactly.
**Pass coalescing overwhelmingly owns the cost, even without diagnostics.**

Every input produces **zero merges** in every trial. ON-only return
classification explains the rejected candidate population:

| Source | Return candidates | Blocked cycle | Second pass contains non-draw |
|---|---:|---:|---:|
| 5877 | 1,493 | 752 | 741 |
| 5897 | 3,203 | 1,607 | 1,596 |
| 5899 | 3,199 | 1,603 | 1,596 |
| 5901 | 3,199 | 1,603 | 1,596 |

Non-render-intervener and missing-invariant buckets are zero. OFF counter zeros
mean disabled observation, not absent candidate work. Since no merge occurs,
these spikes are **failed search**, not successful graph rebuilding. The input
graph stays structurally unchanged through candidate search.

## Next optimization target, not yet implemented

`fg_optimizer/passcoalesce.cpp::coalesceOnce` scans attachment-matching pairs;
`classifyMove` repeatedly asks `reaches`, which allocates traversal scratch and
scans all edges for each visited node. This is a concrete reuse opportunity
when hundreds/thousands of candidates query the same unchanged graph. The
current measurement does not separately price these inner operations.

Prefer graph-qualified adjacency/reachability reuse and precomputed draw-only /
barrier summaries, with invalidation after a successful merge. Preserve the
first-safe-pair order and exact replay/action output; keep these fixtures as
counterexample-capable differential inputs. Do not skip a candidate solely
because this workload happened to produce zero merges. Also consider making
return diagnostics an explicit observer rather than an implicit perf-counter
cost, but that removes only the smaller measured fraction here.

No optimizer change or parallel-Encode promotion is made. A future algorithm
change still owes formal/native refinement and applicable GPU verification.

## Tools and checks

`dxmt9-framegraph-optimizer-probe INPUT.fginput` invokes the actual production
passes. `scripts/tools/measure_framegraph_optimizer.py` preserves raw CSV/stderr,
rejects incomplete/failed trials, authenticates images/inputs, and records CPU
and wall medians. A new output directory is required; evidence is never replaced.

Native and x86_64 full builds pass. Final native fixture/optimizer/linearizer/
backend tests pass 4/4; x86 fixture/install-name checks pass 2/2. The fixture
test covers every value family, nontrivial A→B→A candidate observation,
roundtrip, truncation/trailing data, enum/range rejection and OFF/ON equality.
Its first non-vacuity assertion exposed a same-target fixture that merged
adjacent passes before reaching a return candidate; the corrected test uses
distinct middle attachments. No rendering/lifetime transition changes, so
this diagnostic-only change introduces no new TLA model or GPU promotion gate.
