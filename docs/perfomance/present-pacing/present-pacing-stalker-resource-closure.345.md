---
domain: present-pacing
workload: STALKER CoP Day
title: "H345: Pass read-union overflow, not incomplete individual draws"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-resource-closure-h345-r693-day/{result.json,actual.png}; experiments/output/stalker-resource-closure-h345-r693/{supply.csv,waits.csv,progress.json}
related: present-pacing-stalker-parallel-observer.344.md
---

# H345: Resource-closure rejection attribution

Base `d2c30dba` plus resource-detail attribution. The serial-only H344 observer
now distinguishes `raw_read_set_incomplete`, `raw_write_set_incomplete`, and
`pass_read_union_overflow`. Existing reason IDs and the coarse
`resource_set_incomplete` count are retained. Details are overlapping per-pass
bits, not per-draw occurrences or additive rejection totals. No capacity,
canonicalization, hazard, execution, lifetime or partition policy changes.

## R693 qualification

Day/high 1280x720, Sikarugir cx24.0.7, perf. Same explicit policy as R691:
Metal occlusion, async query fences, covered Pending and publication dedup ON;
Tape, query CB join, END coalescing and command/draw split OFF; IdentitySerial.
Supply window 2450–2470, wait window 2452–2468, parallel observer ON.

Runner PASS, no failures/capture error, 2776 encoded Presents, zero resolved
parallel work and zero reported GPU command-buffer errors. Captured image
shows the game scene, not a desktop fallback; this is not pixel-exact evidence.
PID 51262; unix provider SHA-256 prefix `1c7256704bd1b0c3`, matching the build.
Both traces close with zero overflow/incomplete writers; wait unknown identities
zero, CV entries nine. Two partial edge sources are excluded. This single run
is not a matched semantic replay of R691 and is not an FPS A/B result.

## Result

| Rejection | Rejected passes |
|---|---:|
| ResourceSetIncomplete (coarse) | 61 |
| Pass read-union overflow (detail) | 61 |
| Incomplete individual raw reads | 0 |
| Incomplete individual raw writes | 0 |

All resource-set failures in this qualified window are attributable to the
pass-wide union, not an individual draw's resource footprint. At that failure
site the incoming reads are complete/canonical; the destination starts complete
and can become incomplete only by overflowing `ExactResourceSet`'s 12 identities.
Repeated appends after overflow keep the same rejection bit. This does not
reclassify the historical R691 population or establish maximum required capacity.

| Source group | Sources | Inclusive encode CPU ms | Structural passes | Draws |
|---|---:|---:|---:|---:|
| Query-scoped | 1148 | 197.447 | 0 | 0 |
| Unscoped with candidates | 20 | 367.306 | 40 | 37013 |
| Unscoped without candidates | 42 | 349.003 | 0 | 0 |

All 1210 complete sources have qualified observations. Observer CPU totals
16.603ms. Union overflow occurs in 41 sources: 20 also contain successful
passes; 21 contain none and account for 344.723ms inclusive source CPU. Those
times include all source work and observer cost, not just rejected passes;
they must not be presented as recoverable parallel CPU or predicted FPS gain.
Query-state and small-work rejection counts are 1148 and 348 respectively.

## Next implementation boundary

`ExactResourceSet` is intentionally a small copied draw-entry/continuation
summary. Reusing that representation for a whole pass bounds the union at the
same 12 identities, despite each draw fitting independently. The next design
candidate is a separate bounded canonical pass-closure representation, owned
by the sealed-pass planner and bound to its certificate/revalidation path.
Do not globally enlarge every copied source summary or merely clear overflow.

Before promotion, establish required cardinalities, preservation of alias/hazard
checks, deterministic overflow fallback, and certificate equivalence; then run
CPU and GPU exact oracles. Child imbalance, executor binding/economics, and
query-scoped exclusions remain independent gates. Wider closure alone is not
evidence that any rejected pass becomes executable or profitable.

## Validation

Native parallel-pass, CPU-supply analyzer, encode-session lifecycle and
framegraph tests pass (4/4); Python tests pass (27/27); unix provider build and
install-name audit pass, both PE builds are current. Native counterexamples
distinguish incomplete per-draw reads, complete per-draw sets overflowing the
pass union, repeated failure counted once, and repeated same-set acceptance.
Current attachment writes contain at most four color targets plus depth, so
raw-write overflow is unreachable with capacity 12; its diagnostic is defensive,
not a fabricated runtime counterexample.

This patch changes rejection classification only. It does not introduce a new
stateful refinement or GPU oracle; H344's TLA regression remains prior evidence,
not a claim of a new TLA run. Parallel execution remains disabled.
