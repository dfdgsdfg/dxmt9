---
domain: present-pacing
workload: STALKER CoP Day
title: "H336: 54 occlusion scopes, 55 publications and completion credit"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-publication-progress-r676-day/{result.json,run-policy.json,actual.png,waits.csv,supply.csv,progress.json}; experiments/output/publication-progress-validation-h336/{dxmt9-r676-native-tests.log,dxmt9-r676-native-isolated-tests.log,dxmt9-r676-unix-tests.log,dxmt9-r676-tla.log,dxmt9-r676-analysis-tests.log}
related: present-pacing-stalker-replay-ownership.335.md; present-pacing-query-scope-cb-composition.332.md
---

# H336: scope boundaries fill the publication cap behind a large head

Base `014c79b1` commits H335. An observation-only overlay adds actual
publication reason/command count, query Issue object/flags, scope membership
transitions, completion-worker dequeue, completion observation and credit
retirement to the existing bounded CPU-supply trace. Runtime predicates,
limits, query semantics and defaults are unchanged. The scope value packs
the published bit at bit 63, old count at bits 32–62 and new count at 0–31.
These fields remain in diagnostic storage, not wire or Slot contracts.

## R676 qualification

Day/high 1280x720, Sikarugir cx24.0.7, perf profile. Tape, scoped CB join,
coalesce-END and PE sampler OFF; split/frame observations ON. Wait window
2200–2300 and supply window 2190–2310. Both close without overflow or
incomplete writers; supply contains 209,847 events. Runner PASS, no timeout,
failures or capture error; screenshot shows the game, not a continuous pixel
proof. There are 2635 encoded Presents. Staged image hashes are in result.json.
This intrusive diagnostic is not an observer-OFF FPS comparison.

## Why one replay call publishes 55 sources

The longest matched query-54 receipt in this run is owner 1/raw 381409,
encoded Present 2265. Its worker TID is 1256087. Replay wall is 27.112ms,
native CPU 4.704ms and nested cap wait 19.426ms; app receipt elapsed is
20.353292ms. These overlapping scopes must not be added.

The raw replay calls Issue BEGIN/END once each for query objects 1–54:
108 query operations. It produces ordinary sequences 9899–9953, all reason
9 (`SemanticBoundary`), not `QueryEnd`, `QueryPoll` or capacity rotation.

| Boundary | Count | Published command extent |
|---|---:|---|
| First scope 0→1 | 1 | Existing writer: 284 commands |
| Remaining scope 0→1 | 53 | Empty writer: no publication |
| Scope 1→0 | 54 | One command in each published source |

Thus 55 = one populated prelude + 54 query-scope closures. A command is a
backend command (potentially a DrawRun), not necessarily one D3D draw.
The first source includes the previously accumulated writer: caller-context
attribution does not claim all its commands originated in this raw.
`CommandQueue::setOcclusionScope` owns the boundary; it publishes a populated
writer before replacing membership so earlier work keeps its old scope.

## What releases inflight=31

After publishing sequence 9929, the next publication (9930) waits at
inflight=31. During that 19.426ms wait the trace records exactly one credit
retirement: sequence 9899, the 284-command head source. Inflight becomes 30.
The sequence is fully covered by all ordinary progress endpoints:

| Head-source stage | Elapsed |
|---|---:|
| Publication → encode start | 0.059ms |
| Encode start → end | 14.352ms (14.342ms native CPU) |
| Encode end → actual commit receipt | 0.164ms |
| Commit → completion-worker dequeue | 0.046ms |
| Dequeue → observed completion | 5.989ms |
| Observed completion → credit retirement | 0.045ms |

Credit retirement occurs 19.390ms after cap-wait entry; the wait returns
approximately 0.036ms later. This is an observed retirement in the wait
interval, not an OS notification-identity claim.

All 55 target publications have complete 1:1 ordinary stage coverage:

| Per-source stage | Median | Maximum |
|---|---:|---:|
| Publication → encode start | 14.890ms | 18.420ms |
| Encode | 0.142ms | 14.352ms |
| Encode end → commit | 0.019ms | 0.164ms |
| Commit → completion dequeue | 4.972ms | 5.944ms |
| Completion dequeue → observed | 0.004ms | 5.989ms |
| Observed → credit retirement | 0.094ms | 0.344ms |

Small sources wait behind the large head in encode and completion FIFO.
Completion dequeue-to-observed includes `waitUntilCompleted`, driver and
scheduler delay; it is not a GPU execution timestamp. Likewise commit-to-
dequeue is a completion-worker queue delay, not pure GPU time. Source stage
intervals overlap across sequences, so their sums are not a frame budget.

## Decision boundary

This reproduces head-of-line backpressure driven by a large encode head plus
many tiny scope publications. Submit-call or post-completion retirement cost
is not the dominant component in this observed head. It does not prove every
late hitch has this owner, or that GPU arithmetic is irrelevant.

The next structural candidate is to distinguish the required visibility/pass
scope boundary from source publication while preserving per-command scope,
END receipts, FLUSH submission and exactly-once completion. H332 CB-only
composition does not itself remove the per-source publication credit limit;
do not interpret this run as evidence to enable it or simply increase the cap.
The head's 14.342ms encode CPU is a separate measured optimization target.

## Verification

All four canonical builds pass. Native focused 4/4 passes in isolation;
x86 CPU-clock/install-name checks pass 2/2; TLA regression passes in 196.95s.
The first native CPU-clock fixture failed during concurrent TLA at 16.48ms
CPU / 40.001ms wall (its minimum is 20ms / 50%). Preserve that failed log;
the unchanged isolated repeat passes, not a silently relaxed clock threshold.
The 18 Python analysis tests cover missing/duplicate progress, query selection,
scope/command metadata and the earlier identity/closure counterexamples.
No task-owned builds or tests overlap the wild run; unrelated host activity
is not excluded. These are diagnostic gates, not a new rendering-policy proof.
