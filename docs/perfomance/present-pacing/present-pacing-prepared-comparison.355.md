---
domain: present-pacing
workload: STALKER CoP Day and synthetic prepared-plan fixtures
title: "H355: Indexed builder requalification and equal-input linear comparison"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/prepared-comparison-h355/{aa-native-clock.csv,ab-native-clock.csv,ba-native-clock.csv,native-clock-hashes.txt,native-clock-tests.log,x86-native-clock-tests.log,stalker-on.csv,stalker-on.json,stalker-runner.log}; experiments/output/app-d3d9-stkcop-bench-prepared-comparison-h355-day/{result.json,actual.png,daybenchmark.result}
related: present-pacing-prepared-index.354.md
---

# H355: Wild integration and equal-input construction cost

Runtime commit `3bf9b37d` (`Index bounded prepared-plan identity construction`).
The added benchmark is test-only. No alternate production builder or runtime
policy was introduced. Prepared plans remain diagnostic shadows, not Encode input.

## Wild requalification

Day/high, renderer_r2.5, 1280x720, Sikarugir cx24.0.7, perf profile;
identity partition policy, prepared observer ON for encoded Presents 2450–2470,
capture and other split observers OFF. Query scope CB join and END coalescing
remain OFF. No build or microbenchmark overlapped the wild run.
Computer-use restored and verified the game window during loading. The runner's
150-second capture subsequently targets that STALKER window, not Orca.
The captured scene is visibly rendered; this is a screenshot smoke check,
not a whole-run pixel-equivalence oracle.
The user subsequently confirmed visually normal operation. Together with the
native builder equivalence and boundary tests, this closes the visual smoke
gate for this diagnostic-only change. Additional GPU exact evidence is required
when the prepared plan starts controlling Encode execution, not as a blocker
to retaining the current shadow builder.

Runner PASS, return code 0, no timeout or capture error, elapsed 224.751 seconds,
zero GPU command-buffer errors. Native benchmark average **21.617464 FPS**,
reported min/max 4.983206 / 49.413414. The five staged artifacts match H354;
Unix hash prefix `3e2fd9a2bea450ee`. H354's 8.464481 FPS remains excluded because
its capture/window conditions failed. H353's 21.545542 FPS is context only:
these are different live inputs and not a controlled speedup comparison.

The trace is closed, with zero overflow/incomplete writers:

| Observation | Value |
|---|---:|
| Qualified sources | 1191 |
| Query / no-work skips | 1128 / 21 |
| Preparation successes / failures | 42 / 0 |
| Wide sources / identities | 21 / 415–459 |
| Unresolved rows, wide / all successful sources | 0 / 126 |
| Allocation / reuse | 1 / 41 |
| Immutable region bytes | 151624 |
| Acquire / append / seal CPU, cumulative | 0.088 / 4.751 / 1.186 ms |
| Total shadow CPU, including skipped sources | 12.971 ms |

Different source cardinality and scheduling prohibit subtracting H354 timings.
The smaller successful plans may still require Encode-side resolution;
structural preparation success does not certify parallel execution eligibility.

## Same-input builder comparison

`tests/native/backend/prepared_encode_benchmark.cpp` supplies each builder the
same immutable in-memory fixture. The generated linear reference replaces only
interning and removes index scratch; it retains current 512-identity limits,
storage, workspace reuse, sealing and independent validation. It is not the
historical 256-entry binary. Generation fails if expected source markers drift.

Two synthetic shapes: 64 commands / 64 identities and 270 commands / 420
identities. Each command has eight reads and one aliasing write; keys contain
full generation bits. Both are DrawRun-only, canonical, single-range inputs.
These are **not captured STALKER command streams**. Equality checks cover dense
identity ordering, access indices, command fields, ranges and resolution flags.

Same x86_64 executable under Rosetta, two warmups per policy, 32 paired samples,
alternating AB/BA ordering and a separate reversed-start run. A/A uses independent
indexed workspaces. Printing happens after timing. Total encloses acquire/reset,
append, seal, checksum and recycle, including instrumentation; fresh allocation,
source extraction, alias resolution, queue synchronization and GPU work are absent.

Median CPU time per construction (microseconds):

| Fixture / pair | Indexed | Comparator | Reduction |
|---|---:|---:|---:|
| 64 identities, A/A | 8.0 | 8.5 indexed | noise/control |
| 420 identities, A/A | 61.0 | 61.0 indexed | noise/control |
| 64 identities, A/B | 8.5 | 16.0 linear | 46.9% |
| 64 identities, reversed start | 9.0 | 16.0 linear | 43.8% |
| 420 identities, A/B | 61.0 | 231.0 linear | 73.6% |
| 420 identities, reversed start | 60.5 | 232.0 linear | 73.9% |

For the wide A/B fixture, append costs 20 vs 191 microseconds; seal costs 39 vs
38 microseconds. Indexed seal is now the largest measured builder phase. Its
independent quadratic duplicate validation is a next candidate, not removed here.
Wall medians corroborate the wide result: 61.000 vs 230.542 microseconds;
reversed start 60.541 vs 231.958. A/A wall medians are 60.791 vs 60.750.
CPU samples are microsecond-quantized; tiny phase differences are not meaningful.

### Clock exclusion

Initial `aa.csv`, `ab.csv`, `ba.csv` and `summary.json` in the artifact directory
are excluded: POSIX `CLOCK_THREAD_CPUTIME_ID` returned approximately wall/41.67
on this Rosetta host. Their derived CPU units are invalid. Accepted files have
the `-native-clock` suffix and use `proc_pidinfo(PROC_PIDTHREADID64INFO)` user plus
system nanoseconds, matching the production CPU-supply observer. Clock lookup
failure aborts rather than silently substituting zero. Accepted CPU/wall totals
are consistent; timer overhead remains included symmetrically.

## Verification and remaining gate

Native and x86_64 plan + equal-output benchmark tests each pass 2/2 after the
clock fix. Runtime lifecycle code is unchanged from H354's TLA-tested commit.
The test target runs semantic checks only; timing has no pass/fail threshold.

Reproduce with `meson test -C build-x86_64-builtin
dxmt9-prepared-encode-benchmark-spec`, then run
`build-x86_64-builtin/tests/native/backend/dxmt9-prepared-encode-benchmark
--benchmark` (optionally `--aa` or `--reverse`) in an otherwise idle interval.

This closes single-run wild integration and a controlled synthetic interning
comparison. Actual captured STALKER equal-input comparison, GPU exactness,
consumer proof integration and end-to-end FPS benefit remain open. The indexed
construction result must not be promoted to a 74% pipeline or FPS improvement.
