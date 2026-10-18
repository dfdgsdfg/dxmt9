---
domain: present-pacing
workload: STALKER CoP Day high 1280x720
title: "H609 — sparse live Encode draw-loop attribution"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-owner-h609/verification.json; traces/stkcop-encode-owner-h609/preflight-verified.json; traces/stkcop-encode-owner-h609/N0/attribution.json; traces/stkcop-encode-owner-h609/N1/attribution.json
related: docs/perfomance/present-pacing/present-pacing-pair-live.608.md; docs/perfomance/present-pacing/present-pacing-run-milestones.543.md
---

# H609 — sparse live Encode draw-loop attribution

The whole draw-subrange loop is the largest disjoint CPU owner **inside both
selected late-episode source populations**. This is not yet a population-wide
CPU/frame attribution or a removable-time certificate. No product code changed.

## Method and scope

Use H602 generation-qualified default archives, without H607's pair candidate.
Replace only `dxmt9_queue.cpp.o` and `dxmt9_draw_encoder_chunk.mm.o` in a private
runtime archive. The ordinary queue's unlocked `encodeFn` invocation owns each
root. It excludes dequeue wait and post-call queue bookkeeping. Identity carries
controller, queue lifetime domain, sequence, source index/generation, storage
page/count/generation, process birth, and executing thread.

Select roots before execution by a fixed sequence hash (1/64), with bounded
8192-row storage. Count all roots. CPU endpoints exist only on selected roots;
children record nested wall intervals, converted to conservative CPU bounds
using root CPU and the complementary wall interval. Endpoint acquisition delay
and 2us/read uncertainty remain in the bounds. There are no per-draw clocks.
The Loop scope surrounds the entire `encodeDrawSubrange` loop, including
`encodeDraw` and in-loop state/command work, not GPU execution alone.

Fixed live population: N0,N1, same binary, Day/high/1280x720,
`-max-threads 2`, `snd_targets=16`, Clear OFF, product counters OFF. H602 requests
whole-thread endpoints at spatial knots 1 and 6. Both runs reach all seven
active knots and exit naturally with game status0. Catalogue missing_capture is
the sole prospective waiver. Post-tail screenshots are coherent, not exact
full-scene pixel oracles. Fourteen files per run are restored by hash.

## Whole-thread CPU

| Run | Completed frames | Replay CPU ms/frame | Encode CPU ms/frame | Diagnostic late FPS |
|---|---:|---:|---:|---:|
| N0 | 121 | 29.278–29.395 | 28.846–29.075 | 17.538–17.544 |
| N1 | 122 | 30.225–30.363 | 30.280–30.471 | 18.293–18.298 |

These are observer-ON diagnostic values, not a new README baseline or an H608
performance contrast. Replay leads N0; the N1 worker bounds overlap. Neither
worker is established as the permanent critical-path bottleneck. Never sum
their CPU times as frame latency.

## Selected source population

13/12 roots lie strictly between the matching Encode CPU-read brackets; there
are no straddlers. The following values are **total milliseconds across those
selected roots**, not per frame and not scaled by64.

| Disjoint CPU bound | N0: 13 roots | N1: 12 roots |
|---|---:|---:|
| Entire selected root population | 72.010–72.703 | 30.046–30.185 |
| Whole draw-subrange loop | 42.756–47.873 | 18.544–19.643 |
| Pass decisions/start/split | 8.988–13.647 | 3.691–4.789 |
| Outside encodeChunk within encodeFn | 5.540–10.692 | 3.112–4.299 |
| Chunk remainder | 0.360–2.345 | 0.630–1.407 |
| Uniform/setup | 0–1.437 | 0.104–0.585 |
| DrawRun remainder | 0–0.663 | 0.012–0.284 |
| Hazard/blit | 0–0.281 | 0–0.128 |
| UP preparation | 0–0.137 | 0–0.058 |
| Binding setup | 0–0.087 | 0–0.039 |

Loop lower bounds exceed every other individual upper bound in both runs.
Its bounded sample CPU share is 58.81–66.48% / 61.43–65.38%. N0 includes1791
DrawRuns; N1 includes761. Different sequence populations explain why their raw
totals are not an A/B comparison.

Only about2.05–2.08% / 0.808–0.817% of whole-window Encode CPU is directly
covered by these sampled roots. The remaining CPU is **unattributed by this
observer**, not zero and not necessarily outside encodeFn. Multiplying the
samples by64, applying their shares to all frames, or declaring a removable
~60% Encode budget would be unsupported. Observer cost is included and is not
independently subtracted.

## Evidence and correction

- ARM64/x86_64:12 native cases cover disabled/all/sparse modes, unwinding,
  concurrent distinct domains, invalid identity, nested root, CPU failure,
  stack depth, and bounded-storage overflow.22 corrupt-trace tests reject
  invalid records. Existing process-exit identity tests pass.
- Original source/output SHA remains
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`
  with observer OFF and ON. Query transcript,7 sources,670 wrapper lifetimes,
  and restoration match. ON exercises9 ordinary roots,757 DrawRuns and761
  subrange loops with every root selected.
- The first preflight incorrectly expected the original harness to bypass the
  ordinary worker. It stopped on `assert h[14] > 0` despite zero orphan scopes.
  The failed script/report remain. `preflight_complete.py` independently checks
  the same original results, corrects that expectation, and runs the remaining
  queue controls. Neither original run was repeated or discarded.
- Separate ordinary-queue OFF/ON controls pass Metal validation and two
  readbacks, with draws skipped. These establish routing, not candidate pixels.
- Live root census12161/11943; selected179/176; no faults, drops, orphans,
  incomplete roots, or unmatched selected-thread identity. Independent raw
  verification checks355 rows, conservation, source reversal and28 restoration
  hashes. No Wine process remains.

## Decision

Continue inside the whole draw loop rather than returning to small uniform,
binding-setup or pair-preparation candidates. Separate dxmt9 state/binding
reconstruction from Metal command emission on the already qualified input;
keep expensive per-draw timing out of the live baseline. Any removal candidate
still needs identical effects and a same-input CPU reduction, then bounded live
transfer. The global largest CPU operation and its removable amount remain open.
