---
domain: present-pacing
workload: STALKER CoP Day
title: "H344: Serial-only structural parallel candidates and source CPU"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-parallel-observer-h344-r691-day/{result.json,actual.png}; experiments/output/stalker-parallel-observer-h344-r691/{supply.csv,waits.csv,progress.json}; experiments/output/stalker-parallel-observer-h344-r690/{supply.csv,waits.csv,progress.json}; experiments/output/parallel-observer-validation-h344/{dxmt9-h344-tests-final.log,dxmt9-h344-unix-tests.log,dxmt9-h344-tla.log}
related: present-pacing-stalker-capture-origin.343.md
---

# H344: Measure candidates without executing them

Base `67cb3ad4` plus the default-OFF
`DXMT9_CPU_SUPPLY_PARALLEL_OBSERVER` overlay. The observer requires the bounded
CPU-supply window and IdentitySerial mode. It invokes the existing pure
sealed-pass producer with real query/capture/initializer facts in separate
diagnostic scratch. No snapshots flow to the executable batch, no worker
executes them, and production parallel counters are unchanged. This does not
repair Tape capture or widen any renderer policy.

Pool/sequence/TID events join through the existing framegraph pool binding.
The analyzer requires complete nested endpoints, monotone clocks, typed
counts and rejection reasons. Missing observations are not zero eligibility.
Structural acceptance precedes runtime binding/certificate/economics checks.
Whole-source CPU includes ineligible draws, prefetch and observer overhead;
it is only an upper bound on the cost reachable through eligible work.

## Qualification and exploratory windows

Day/high 1280x720, Sikarugir cx24.0.7, perf. Tape, command/draw split observers,
CB join and END coalescing OFF. Partition mode explicitly identity.
R690/R691 enable Metal occlusion, async fences, covered Pending and publication
dedup. R689 accidentally omits the opt-in query settings and is retained only
as an exploratory different-policy sample, never a control for R690/R691.

R690 observes 1700–1710 Presents: 20 complete sources, none query-scoped,
3 candidate-bearing sources / 3 structural passes / 451 draws. This is not
the late-query workload, prompting a separate late window. Fixed Present
numbers do not guarantee scene or semantic-boundary equivalence across runs.

R691 observes supply 2450–2470 and waits 2452–2468. Its trace closes with
zero overflow/incomplete writers and 1470 complete ordinary sources, all
with qualified candidate observation; two incomplete edge sources are excluded.
The wait trace has zero unknown identities and zero CV entries. Thus this is
late-source CPU/eligibility evidence, not a reproduction of query-54 blocking
or an assertion that queue backpressure has disappeared.

## R691 source-qualified result

| Source group | Sources | Inclusive encode CPU ms | Observer CPU ms | Structural passes | Structural draws |
|---|---:|---:|---:|---:|---:|
| Query-scoped | 1126 | 204.917 | 10.036 | 0 | 0 |
| Unscoped with candidates | 20 | 233.533 | 1.474 | 40 | 24975 |
| Unscoped without candidates | 324 | 447.466 | 7.361 | 0 | 0 |
| Total | 1470 | 885.916 | 18.871 | 40 | 24975 |

Candidate-bearing sources contain 26.36% of measured source encode CPU.
This is an inclusive upper bound, **not 26.36% parallelizable CPU or FPS gain**.
Even these sources contain rejected passes; observer timing is not a calibrated
subtraction. CPU scope sums are not wall-clock critical-path budgets.

The producer sees 1874 candidates/sealed passes, with 40 structural successes
and 274 proposed children. Rejections include query state 1126, insufficient
two-child work 627, and incomplete resource set 81. Rejection categories can
overlap and must not be summed as disjoint passes.

Among candidate-bearing sources, 16/20 have at least one proposed pass whose
maximum-minus-minimum child draw count exceeds the default imbalance bound
64 (observed per-source maxima 62–97). This is not a count of all rejected
passes; the observation stores the maximum per source. Structural success
does not imply that the production economics gate would accept all 40 passes.

Largest source: queue owner 140370671669216, sequence 19093, 176 commands,
15.473ms inclusive encode CPU. It has 15 structural pass candidates, 2
successes / 1241 draws / 14 proposed children; maximum child imbalance 84.
Observer CPU is 0.076ms. The remaining candidate rejections are small work
12 and incomplete resource set 1; these do not price the actual two passes.

## Observer-OFF control (R692)

R692 uses the same explicit policy and Present windows as R691, with only the
parallel observer disabled. Both runners pass without reported failures or
capture errors. All five staged artifact hashes match across R689–R692;
the unix provider SHA-256 prefix is `975ff7f1d867ebc6`.
R691/R692 encode 2682/2691 Presents respectively. Their captured images show
the game scene rather than a desktop fallback; these are not pixel-exact oracles.

R692 has 1356 complete ordinary sources (two incomplete edges excluded),
788.161ms inclusive encode CPU, and no qualified parallel observations.
Disabled observation means **unmeasured**, not zero candidate work.
Both trace sinks close with zero overflow/incomplete writers and no unknown
wait identities. Unlike R691's zero CV entries, R692 records six CV entries.
Thus source populations and wait behavior differ: the 885.916 versus 788.161ms
totals cannot be subtracted to estimate observer overhead or parallel savings.
These single fixed-Present-window runs establish neither FPS improvement nor
regression. Parallel execution remains disabled in both.

Control artifacts: `experiments/output/app-d3d9-stkcop-bench-parallel-observer-h344-r692-day/`
(`result.json`, `actual.png`) and
`experiments/output/stalker-parallel-observer-h344-r692/`
(`supply.csv`, `waits.csv`, `progress.json`).

## Next candidate

Within unscoped sources with no structural success:

- 21 sources reject only for incomplete resources: 228.875ms source CPU.
- 20 sources mix incomplete resources and small-work rejections: 77.752ms.
- 262 sources reject only for small work: 136.563ms.
- 21 sources have no rejection/candidate: 4.276ms.

These are costs of the containing sources, not time spent checking resources.
`produceSealedParallelPassSnapshots` merges several failure sites into
ResourceSetIncomplete: individual read/write-set incompleteness and pass-wide
union failure. `ExactResourceSet` has capacity 12, but this observation does
not prove capacity overflow caused every rejection. Next split those causes
and identify the affected pass/resource closure before changing representation.
In parallel-policy design, inspect whole-command child imbalance without
loosening the bound or violating ordered draw boundaries.

This evidence supports targeted closure/partition investigation before broad
Volume/Query replay expansion solely for a presumed parallel speedup.

## Validation

All four canonical builds pass. Native focused checks 5/5, x86 trace/install-name
checks 2/2, Python analysis 26/26 and TLA regression 199.82s pass. A first native
clock-sensitive check failed during concurrent build/test activity; the final
native checks pass serially after TLA/build completion. The native Xcode build
requires explicit current SDKROOT for standard-library header discovery in
this shell. Source guards pin separate diagnostic scratch and rejection enum
mapping; they are not a new proof of parallel execution semantics.
