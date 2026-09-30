---
domain: present-pacing
workload: STALKER CoP Day
title: "H416: Query ranges activate but remain slower in both run orders"
type: evidence
status: performance-gate-failed-default-off
updated: 2026-09-29
source: traces/query-ranges-h415/{compare_wild.py,wild-comparison.json,wild-screens/on-diagnostic-scene.png}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h412-{on-h416-diag1,off-h416-diag1,off-h416-forward1,on-h416-forward1,on-h416-reverse1,off-h416-reverse1}/{result.json,daybenchmark.result,dxmt9.log}; traces/query-ranges-h412/wild-{on-h416-diag1,off-h416-diag1,off-h416-forward1,on-h416-forward1,on-h416-reverse1,off-h416-reverse1}/{invocation.json,completion.json}
related: present-pacing-query-range-retained-release.415.md; ../../../specs/backend/query-scope-ranges.md
---

# H416: Activation succeeds; FPS promotion fails

Runtime commit: `bc668ac0` (`Retain query-range writer across serialized
occlusion release`). All three staged runtime build directories were rebuilt,
including the default Unix target's install-name fixup. No runtime changes
were made between these six runs.

## Controlled comparison

Sikarugir CX24.0.7, Day/high, 1280×720 windowed, two game threads and sixteen
sound targets. Metal occlusion, async query fences and CPU-ready tape are ON;
covered-Pending, query CB joining and END coalescing are OFF. Only
`DXMT9_QUERY_SCOPE_RANGES` differs within performance pairs. Performance runs
disable counters, frame sampling and validation; separate diagnostic runs
enable counters, query split observations and replay plan timing.

`compare_wild.py` checks identical requested-config SHA and all five staged
binary receipts across six runs, equal performance environments except the
policy, successful exits and absent performance counters in observer-OFF logs.
Config SHA-256:
`1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.

| Staged artifact | Bytes | SHA-256 receipt prefix |
|---|---:|---|
| i386 d3d9.dll | 1921024 | cd8c8b7fc0abf476 |
| i386 winemetal_dxmt9.dll | 140800 | f6b4f6fc50ac76ac |
| x86_64 winemetal_dxmt9.so | 10927600 | 367ef22787a2efbc |
| x86_64 d3d9.dll | 1785344 | fc0c7175fc88cd83 |
| x86_64 winemetal_dxmt9.dll | 145920 | 7e08a5d45558380f |

Game result-file average FPS, not sampled-frame FPS:

| Execution order | OFF | ON | ON change |
|---|---:|---:|---:|
| OFF → ON | 23.627712 | 21.348026 | −9.65% |
| ON → OFF | 23.446005 | 21.150637 | −9.79% |

All six runs pass, exit zero, avoid timeout and report zero admission warnings.
No builds, formal tests or external samplers ran alongside them. The runner's
automatic screenshot at sixty seconds is common to every run. Two samples
per policy reproduce direction, not a broad confidence interval or proof of
absence of all external interference. H414 measured a different image.

## Activation and cost evidence

Separate diagnostics normalize whole-run cumulative scopes by 2,551 OFF and
2,281 ON Presents. These populations differ and elapsed scopes overlap:
**do not sum them or interpret the differences as removable frame time**.

| Per Present | OFF | ON |
|---|---:|---:|
| Draws | 2646.91 | 2625.33 |
| Command-buffer creations | 21.03 | 6.51 |
| Render passes | 32.23 | 33.54 |
| Replay elapsed ms | 39.72 | 24.72 |
| EncodeChunk elapsed ms | 21.04 | 17.81 |
| Publication resource marking ms | 0.846 | 0.701 |
| Compatibility batch calls | 37.69 | 83.38 |
| Compatibility emitted DrawRuns | 352.18 | 426.25 |
| Compatibility append ms | 2.488 | 2.644 |
| Replay worker idle ms | 38.01 | 61.66 |
| Replay drain wait ms | 7.579 | 0.303 |
| Encode dequeue-to-CB-commit stage ms | 24.31 | 28.46 |
| Arena sources reaching Encode | 44.59 | 0 |
| Session heads appended | 51.13 | 0 |
| PSO prefetch ms | 1.167 | 1.633 |

| Query split event, whole-run count | OFF | ON |
|---|---:|---:|
| Deferred new END | 0 | 1434 |
| Deferred shared END | 0 | 26091 |
| Marker bound | 0 | 1434 |
| FLUSH publication | 0 | 1159 |
| Ordinary publication | 54994 | 5697 |
| Completion-bearing tail CB | 35785 | 4642 |

Tail-CB observations exclude initializer/sub-CBs and are not the command-buffer
creation counter. Both diagnostic runs have zero GPU command-buffer errors,
zero unbatched compatibility draws and zero queue-sequence waits. The latter
does not establish absence of other synchronization or GPU waits.

The ON diagnostic's outdoor scene screenshot shows buildings and trees without
obvious broad black/geometry corruption. This is a sampled visual check using
computer-use, not full-sequence wild exact comparison. H415's deterministic
query/pixel oracle remains separate evidence.

## Decision and next discriminating measurement

Keep ranges **default OFF**. H415 closes the serialized activation gap, and
this run confirms production activation, but fewer publications/CBs do not
translate into faster frames. No single cost owner yet explains the deficit.

1. In `src/dxmt9/dxmt9_command_queue.cpp`,
   `releaseCpuReadySessionBeforeOrderedControl` still posts, wakes and waits
   for ClosePass at every eligible BEGIN/END. Measure repeated acknowledgements
   of an unchanged published prefix separately from genuinely new work. Any
   reuse must preserve queue/epoch/action identity, reset/terminal handling,
   source order and real submission obligations; an ack is not GPU completion.
2. Isolate global Arena/session exclusion from range-local costs with the same
   input. Zero Arena/session activity and doubled compatibility batch calls
   identify a changed route, not its exclusive contribution to FPS.
3. Price range-consumer scanning in `src/dxmt9/dxmt9_draw_encoder_chunk.mm`,
   `selectQueryRange`, before considering monotone traversal.

Full concurrent reset/callback refinement, all-family fault injection,
callback-retained aggregate credit and cross-workload/wild exact qualification
remain open. No new performance claim or policy promotion follows from the
earlier native test success.
