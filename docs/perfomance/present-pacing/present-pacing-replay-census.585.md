---
domain: present-pacing
workload: STALKER H502/r5 fixed original V4 input
title: "STALKER Replay census: compatibility reserve exceeds append; GT2 uniform attribution does not transfer"
type: evidence
status: fixed-input-census-qualified-not-cpu-saving
updated: 2026-10-16
source: traces/stkcop-replay-census-h585/{contract.md,build.json,run.py,report.json,verify.py,verification.json}; traces/stkcop-replay-census-h585/{off0,parent0,split0,split1,parent1,off1}/{census.json,source.bgrx,output.bgrx,shutdown-counters.json}
related: present-pacing-sparse-cpu.584.md; present-pacing-compat-append-phases.387.md; present-pacing-uniform-lookup-owners.388.md
---

# Price the actual STALKER route, not GT2's uniform fraction

H585 completes six fixed original-input executions without replacements.
Four counted executions agree on all non-time route counters. Compatibility
accounts for 2,828 of 3,982 draws (71.02%); its reserve scope is larger than its
append body in both parent-only runs. Uniform lookup plus append accounts for
43–46% of split-observed append, not H387 GT2's approximately 73%.

This is an operation census and observer-inclusive elapsed attribution, not
native CPU, an optimization result, or a live FPS/removable-work budget.
It does not replace H584's near-peer live Replay/Encode result.

## Controls and measurement scope

One H572 C image pair, default policy, no Clear fusion or private per-source
self observer; only the H506-derived runner is rebuilt. The H570 shader-count
repair remains. No Wine staging or game launch occurs. H585 reads existing
public Replay/append snapshots in cold armed/closed callbacks. V4 validation,
callback forwarding/association, original source boundaries and cleanup remain.

The interval excludes pre-arm setup and includes settlement before closed.
Counters are process-wide, not exclusive caller/thread CPU. Process-shutdown
counters are retained separately; they include work outside this interval.
The frozen order is off0,parent0,split0,split1,parent1,off1. OFF disables product
perf and append clocks; parent enables perf and append mode1; split uses mode2.
OFF zero counters mean disabled observation, not absent work.

All six runs preserve seven source cuts, nine query issues, five polls,
four results, five readiness checks, and 670 created/released wrappers.
Both source and output BGRX exactly match the original SHA-256:
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Restoration and unchanged-normal-state checks pass. GPU error counters are zero
in all counted runs. This is the retained capture oracle, not whole-game visual
equivalence. Independent `verify.py` authenticates input/build/image/log hashes,
readbacks, raw census JSON, shutdown-log extraction and scope arithmetic.

## Actual work shape

| Counter | Every counted run |
|---|---:|
| Direct committed leases / draws | 199 / 1,154 |
| Compatibility batches / draws | 84 / 2,828 |
| Compatibility unbatched draws | 0 |
| Direct continuation capacity rejections | 51 |
| Populated continuation fallback / fallback draws | 51 / 2,824 |
| Capacity rotations | 0 |
| Compatibility append entries / new states | 2,343 / 394 |
| SoA growth after reserve | 2 |
| Encode chunks | 7 |

The 259 raw dispositions comprise 250 Direct candidates, seven state-only and
two ordered-control inputs. Of the 250 candidates, 199 commit and 51 fall back
before effects. The four compatibility draws outside the fallback counter are
not silently reclassified as Direct failures. Count equality does not identify
which capacity dimension rejected each source or how many bytes were allocated.

This is not the old draw-per-queue-acquisition regression: ordinary batching
is active, and no unbatched compatibility draws occur. Do not restore unsafe
mid-source rotation or enlarge headroom merely because fallback is frequent.

## Nested elapsed scopes

All values below are **ms per complete captured interval**, not per live frame.

| Scope | parent0 | parent1 | split0 | split1 |
|---|---:|---:|---:|---:|
| Compatibility batch | 18.012 | 18.276 | 19.048 | 18.346 |
| Within batch: reserve | 4.816 | 4.781 | 5.016 | 4.621 |
| Within batch: append | 3.827 | 3.956 | 4.505 | 4.255 |
| Batch minus reserve and append | 9.370 | 9.540 | 9.526 | 9.470 |
| Within append: uniform lookup | disabled | disabled | 1.299 | 1.305 |
| Within append: uniform append | disabled | disabled | 0.636 | 0.650 |
| Within append: state | disabled | disabled | 1.104 | 0.952 |
| Within append: final SoA writes | disabled | disabled | 0.951 | 0.829 |
| Within append: preflight / local reserve | disabled | disabled | 0.169 / 0.088 | 0.170 / 0.088 |

`dxmt9_append_diag.cpp` admits only Batch-rooted scopes. BatchReserve and
Append are disjoint children in `CommandQueue::submitDrawRunBatch`;
Preflight/UniformLookup/LocalReserve/UniformAppend/State/Soa partition the
inner append body. Preflight is entered twice per append (4,686 calls).
State is entered only for 394 new runs, not the 1,949 compatible extensions.
The remaining 0.260ms inside split Append includes observer/scope boundary
work; it is not another measured renderer phase. Parent and child durations
must not be added as separate costs.

The approximately 9.4–9.5ms batch residual is a separate investigation target.
It includes entry preparation, compatibility checks, mutex acquisition,
binding snapshots, segment admission, marking, argument construction, query
range handling, commit checks and observer overhead. It is neither proven
lock wait nor entirely V4 qualification. Split-parent differences are not a
certified observer-overhead estimate from this six-run census.

## Concrete reserve audit and next discriminant

`src/dxmt9/dxmt9_backend_types.hpp` / `reserveDrawRunBatchStorage` reserves
command/run/state/parameter/payload vectors, four uniform record vectors, two
constant byte arenas, and three lookup families. Its caller supplies one
conservative uniform allowance per batch entry, before interning; it does not
count exact unique fixed/vertex/pixel components. Constant byte reservation
uses that allowance times full stage-constant size, even though stored spans
may be sparse. This is a safe upper bound, not evidence of equal copied bytes.

`chunkSlotReserveAtLeast` already grows geometrically and returns immediately
when capacity suffices. `reserveDrawUniformPayloadLookup` and
`reserveDrawUniformStageLookup` rehash only on bucket growth or inconsistent
table sizes. Empty-table provisioning is already implemented. Therefore
"every batch reallocates/rehashes" and "add geometric growth" are not valid
conclusions or new fixes.

The next bounded measurement should record per-reservation dimension
size/capacity before/after and actual growth/rehash work, with reserve CPU
separated from the batch residual. This distinguishes conservative overreserve,
real relocation and reusable no-growth work before choosing a storage change.
Any candidate must preserve generation-qualified destination ownership,
pre-effect fallback, all capacity dimensions and exact original pixels/query/
lifetime, then demonstrate whole-stage CPU reduction on the same input.

No production code, policy or default changes follow from H585. The largest
default-thread/removable-CPU goal remains open; this result removes an invalid
GT2-to-STALKER attribution shortcut and identifies a bounded reserve target.
