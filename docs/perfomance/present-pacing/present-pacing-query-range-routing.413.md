---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H413: Query-range routing removes warning flood, not the wild gap"
type: evidence
status: single-pair-candidate-slower-default-off
updated: 2026-09-29
source: experiments/output/app-d3d9-stkcop-bench-query-ranges-h412-{off-route1,on-route1}/{result.json,daybenchmark.result,xrEngine_dxmt9.log,stkcop-requested.ltx}; traces/query-ranges-h412/{wild.py,route-tests-final.log,route-x64-tests.log,route-tla.log}; traces/query-ranges-h412/wild-{off-route1,on-route1}/{invocation.json,completion.json}
related: present-pacing-query-range-integration.412.md; ../../../specs/backend/query-scope-ranges.md
---

# H413: Expected compatibility routing is not an admission failure

Based on `ce9933ae` plus the query-range routing change. The first wild ON
run emitted 117,691 `cpu_ready_arena_admission` warnings (27,348,332 bytes):
the process-wide range policy deliberately excludes Arena, but Replay built
its capacity plan, attempted admission and logged that exclusion as failure.
The earlier OFF run also overlapped a two-second CPU sample with scene entry;
those runs are not a clean performance comparison.

## Change and safety boundary

`queryRangeCompatibilityShortcut` accepts only validated, non-capture ordinary
Draw/DrawIndexed/APPLY_STATE raws containing a draw. They cancel speculative
next-source intent and enter the same resource-marking/compatibility transaction
before Arena capacity planning/admission. No record is skipped or reordered.
State-only, capture (including partial identities), UP and control families keep
the full planner. The policy is process-immutable, unlike active query scope;
no unsynchronized read of mutable scope was added. Expected
`OcclusionScopeUnsupported` no longer emits a warning. Other failure logging
remains unchanged.

This is a pre-effect routing predicate, not a new queue/lifetime transition.
The native opcode truth table checks policy/capture gating and every opcode
0..255, including a forbidden suffix following an allowed draw. Native query
and planner tests pass 39/39; x86_64 tests plus install-name audit pass 40/40.
Existing TLA safety/progress/counterexample suite passes; this does not close
the candidate's composed temporal-refinement gap.

## Clean observer-OFF pair

Sequential OFF then ON, same five staged binary receipts and requested config
SHA256 `1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.
Unix provider receipt: `1fd47ba21a58c1c9`, 10,927,040 bytes. Sikarugir CX24.0.7,
Day/high, 1280x720 windowed, two game threads, 16 sound targets. Actual Metal
occlusion and async fences ON in both; covered-Pending, CB join and coalescing
OFF. Only query-range policy differs. Perf/frame observers, CPU sampling and
external GPU capture OFF. All builds and TLA work finished before launching.

| Query ranges | Engine average FPS | Admission warnings | Exit / timeout |
|---|---:|---:|---|
| OFF | 24.118372 | 0 | 0 / false |
| ON | 21.300428 | 0 | 0 / false |

Both harness results PASS. ON is 11.69% lower in this one pair; removing the
warning flood did not demonstrate a speedup. This is not a matched-source
causal decomposition or a reversed/repeated statistical comparison. The small
change against the older contaminated ON score cannot price this optimization.
Remaining plausible costs include the policy-wide Arena/Direct exclusion,
compatibility Slot construction and range lookup; none is attributed by this
observer-OFF run.

Computer Use confirmed a focused real game window. The additional OFF capture
shows the late building scene; the automatic ON capture is a loading screen,
so this pair supplies no new ON in-scene visual or pixel-exact qualification.
The earlier ON wild run had an in-scene screenshot, and deterministic Metal
oracles still pass, but neither substitutes for full-scene current-build exact
comparison. Default remains OFF; no README promotion.

Next: reverse/repeat for reproducibility, then a separate diagnostic pair for
publication, compatibility draws, planning and encode costs. Do not blame the
entire difference on log I/O now that the candidate warning count is zero.
