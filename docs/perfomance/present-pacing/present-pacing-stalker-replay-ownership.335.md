---
domain: present-pacing
workload: STALKER CoP Day
title: "H335: receipt raw identity, publication and inflight backpressure"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-replay-owner-r674-day/{result.json,run-policy.json,actual.png,waits.csv,supply.csv,join.json}; experiments/output/app-d3d9-stkcop-bench-app-owner-r675-day/{result.json,run-policy.json,actual.png,supply.csv,supply-analysis.json,guest-window.json,dxmt9.log,thread-cpu.jsonl}; experiments/output/replay-owner-validation-h335/{dxmt9-r674-native-tests.log,dxmt9-r674-unix-tests.log,dxmt9-r674-tla.log,dxmt9-r674-final-tests.log}
related: present-pacing-identity-wait-trace.334.md; present-pacing-app-thread-attribution.313.md
---

# H335: receipt waits inherit publication backpressure

Base `6799396f` plus the observation-only source overlay extends the existing
CPU-supply trace rather than changing replay or query semantics. It binds
FIFO raw ordinals to wait-ledger incarnations, records writer-slot and
inflight-cap wait pairs, adds process identity, and hardens export ordering.
`join_replay_wait_trace.py` matches receipt prefixes, worker service spans
and publication sequences. Publication is caller-context attribution, not
exclusive ownership of all commands in a Slot. Disabled paths retain cached
gates; no wire/Slot fields or renderer defaults change.

## R674: late raw-to-publication linkage

Day/high 1280x720, Sikarugir cx24.0.7, perf profile, Tape/scoped-CB-join/
coalesce-END OFF, split/frame diagnostics ON. Wait window 2200–2300;
CPU-supply window 2190–2310. Both close without overflow or incomplete
writers. Supply retains 179,938 events. All 51 pending receipt targets join
to the same-process ledger/raw replay identity. Runner PASS, no failures,
timeout or capture error; screenshot shows the game. Staged image hashes
are retained in `result.json`. This intrusive run is not an FPS baseline.

Across the supply window, writer-slot CV waits are absent at the instrumented
site. There are 153 inflight-cap waits totalling 1344.921ms, max 25.546ms.
Every entry records queue identity 2 and inflight count 31. Exit counts are
30 (140 waits), 28 (6), 29 (5), 25 (1), and 26 (1). These are actual
`commitCurrentChunk` predicate waits, not slow initial mutex acquisition.

Query 54 accounts for 49 of the 51 pending receipts (771.081ms elapsed).
The corresponding target raw service spans total 1240.207ms wall,
205.134ms native CPU, and 946.505ms nested inflight-cap wait. These spans
begin before the app may call GetData; do not subtract/add them as one
elapsed budget or equate them with the shorter receipt interval.

The longest receipt provides a concrete identity chain:

`owner 1 / query 54 / raw 386843`
→ `worker TID 1154325`
→ `publication sequences 11997–12051 (55 publications)`
→ `inflight-cap wait under queue identity 2`.

At encoded Present 2291 the receipt waits 19.923417ms. Its target raw spans
28.384ms wall / 4.499ms CPU, including 21.659ms queue wait. Published Slots
have declared raw 0 in this compatibility lane; their attribution comes from
the same-thread replay interval, not a fabricated nonzero Slot raw field.
The join preserves other outstanding raw spans and missing targets explicitly.

The supported conclusion is publication backpressure on this replay path,
not proof that GPU arithmetic is saturated. Completion scheduling, encode
and GPU progress still determine how quickly the inflight cap clears.
Increasing the cap or removing END/receipt waits is not justified by this
observation alone. The next experiment must preserve query visibility and
resource completion while testing why this raw creates many publications.

## R675: pre-fragmentation app ownership

Same staged images as R674 (the complete `staged_build` receipts compare
equal), with wait trace OFF, supply window 1800–1900, guest-PC sampler
250Hz/full-self-PC and module map ON. Runner PASS with no failure, timeout
or capture error; 2525 encoded Presents. The screenshot shows the game.
The external CPU helper starts early and ends with process-not-found after
exit; this does not turn its final error into zero CPU.

The accepted supply interior is 1810–1890 (80 Present intervals):

| Measured owner/scope | Native CPU ms/Present | Wall ms/Present |
|---|---:|---:|
| App thread envelope | 50.435 | ~55.82 |
| Synchronous commit/import scope (subset of app) | 2.155 | 4.378 |
| Replay service scopes | 25.999 | 26.654 |
| Encode service scopes | 29.560 | 30.388 |

App envelope occupancy is 0.904 cores, covering 99.91% of the selected wall
window. Replay's input-wait scope is 28.563ms/Present; its queue is empty
54.73% of the measured wall interval. Thread work overlaps; the rows are
not additive. The import scope excludes other bridge entries such as Query,
Lock/Unlock, StateBlock and PE/Wine transitions. Its small CPU subset does
not assign the remainder to game or PE. Instrumentation perturbs this run;
these are not observer-OFF absolute product costs or a performance regression.

`summarize_guest_pc_window.py` selects complete cumulative sampler reports
and conserves their difference, preserving the unreported top-20 module tail
and rejecting changed top-module membership. The accepted endpoints are
PE Presents 1860→2100, anchored approximately to encoded frames 1858→2098.
There are 2935 stopped-PC samples, zero suspend/context/resume failures and
complete self-PC coverage. Their module distribution is:

| PC location | Samples | Share |
|---|---:|---:|
| Game engine `xr*` modules | 1388 | 47.29% |
| Wine system PE (`ntdll.dll`, `win32u.dll`) | 848 | 28.89% |
| dxmt9 PE (`d3d9.dll`) | 246 | 8.38% |
| Bridge / potentially saved Unix context | 351 | 11.96% |
| Support libraries | 67 | 2.28% |
| Unknown + unreported module tail | 35 | 1.19% |

Largest individual locations are `ntdll.dll` 26.47%, `xrRender_R2.dll`
14.51%, `xrCDB.dll` 11.07%, `xrCPU_Pipe.dll` 10.53%, and `xrParticles.dll`
6.95%. Do not multiply these percentages by app CPU: guest sampling includes
stopped/saved PCs during native execution and waits. The CPU and guest-PC
windows also differ. An attempted earlier 1400–1800 population comparison
is rejected because the exported top-module membership changes.

This narrows candidates without pretending to close the four-way exclusive
budget: inspect Wine-system/query/transition contexts and game renderer work,
not only PE record materialization. Exact synchronous CPU in the remaining
bridge families and reliable running-state Wine/guest attribution remain open.

## Verification

Canonical native, Unix and both PE builds pass. Native focused 4/4 and
x86 focused/install-name 3/3 pass; TLA regression passes in 199.37s.
The Python join tests cover same-process and ledger identity, publication
association, stale owner and duplicate binding rejection. Existing wait and
supply closure tests remain applicable. This is observer verification, not
a new rendering-policy or end-to-end liveness proof.
Final focused native 4/4 passes, including 17 Python analysis tests. New
counterexamples include an unclosed interior queue wait, incomplete guest-PC
coverage and a mismatched sampler target. No task-owned build/test overlaps
either wild run. Unrelated host activity (including `dasd`) is not excluded.
