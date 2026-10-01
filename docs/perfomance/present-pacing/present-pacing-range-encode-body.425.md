---
domain: present-pacing
workload: STALKER CoP Day
title: "H425: Large range-source bodies split between encodeDraw and DrawRun preparation"
type: evidence
status: large-source-cost-split-wait-reproduction-open
updated: 2026-10-01
source: traces/query-ranges-h425/{run.py,analyze.py,analysis.json}; traces/query-ranges-h425/on-active2/{invocation.json,completion.json,supply.csv,release.json}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h425-{on-active1,on-active2}/{result.json,dxmt9.log}; scripts/tools/analyze_publication_progress.py; tests/scripts/test_cpu_supply.py
related: present-pacing-range-reuse-activation.424.md; present-pacing-close-reuse-wild.423.md
---

# H425: Large-source Encode body attribution, not the same blocked workload

HEAD `fd70d2df`; no runtime edits or rebuilds. The five staged artifact receipts
and requested Day/high configuration match H424. Both runs exit zero and pass.
The first run ends at 1,977 encoded Presents before the requested 2000–2004
trace window; its empty trace is rejected, preserved and excluded from analysis.
The second run observes Presents 1850–1854 and ends at 1,955 Presents.
This time-driven workload does not guarantee a fixed frame count or scene at
an ordinal. The second run is not matched work against H424.

Command/draw split observers are enabled in addition to H424's counters and
bounded supply trace. They only instrument sources with at least 128 commands
and without selected parallel execution. No FPS or observer-OFF cost claim is
made. Trace is closed with zero overflow/incomplete rows; GPU error counter is
zero. The reused analyzer's 41 CPU-supply tests pass, including nested draw
coverage, monotone same-thread clocks and rejected malformed transcripts.

## Five complete large bodies

Sequences 4556, 4559, 4562, 4565 and 4568 each contain 371–384 DrawRun commands,
four nondraw commands and 1,516–1,547 encodeDraw calls. Body CPU ranges from
19.565 to 21.025 ms. The following partition totals 101.407 ms CPU:

| Exclusive body component | CPU total | Share |
|---|---:|---:|
| Inside encodeDraw | 55.268 ms | 54.5% |
| DrawRun outside encodeDraw | 40.359 ms | 39.8% |
| Nondraw command scopes | 0.074 ms | 0.1% |
| Per-record split-policy scopes | 1.422 ms | 1.4% |
| Dispatch residual outside those scopes | 4.284 ms | 4.2% |

DrawRun inclusive CPU is 95.627 ms; do not add it to the first two rows.
The DrawRun residual includes preparation, hazards, pass setup, binding-plan
work, loop work and observer overhead; it is not an isolated pass-cost timer.
`ChunkSplit` only surrounds `applyPerRecordSplitPolicy`, not all render-pass
or command-buffer transitions. `selectQueryRange` runs before command timing
and therefore belongs to dispatch residual, along with other work. Its scan
cannot be assigned that entire residual, and this is not a global FPS bound.

Nine other complete bodies are excluded by the observer gate. Four cost about
6.7–7.0 ms each despite having fewer than 128 commands; five are near-empty.
Consequently the table is a partition of large bodies only, not total Encode
CPU for the window. It also excludes planning outside ChunkBody.

## Release ownership did not reproduce here

This window has 533 complete releases, 259 reused acknowledgements, and total
release round-trip elapsed of 6.786 ms. No matched release post-to-action
interval overlaps the five large bodies. H424's long CPU-active prefix waits
therefore have **not** been reproduced with the finer observer in this earlier
window. Do not multiply H424's wait duration by the table's percentages or
interpret the smaller release total as a product improvement.

## Next owner and limits

There are two substantial owners in large bodies, not just encodeDraw: its
internal work and the 39.8% enclosing DrawRun residual. Per-record split policy
and dispatch/range selection are much smaller in this sample. Prioritize a
bounded decomposition of DrawRun preparation/pass/hazard/binding-plan work;
avoid another broad optimization before assigning that residual.

For the original blocking question, source selection should be keyed to the
actual long-release identity or a bounded late scene/time window, not a fixed
Present ordinal that may never occur. Extend coverage to the costly small
sources when attributing whole-window Encode cost. Runtime policy remains
default OFF; H423's negative FPS qualification is unchanged.
