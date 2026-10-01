---
domain: present-pacing
workload: STALKER CoP Day
title: "H426: DrawRun residual concentrates in pass entry and per-draw loop work"
type: evidence
status: diagnostic-partition-complete-no-promotion
updated: 2026-10-01
source: traces/query-ranges-h426/{analyze.py,analysis.json,build.log,pe64.log,pe32.log,tests.log,python.log,tla.log}; traces/query-ranges-h425/run.py; traces/query-ranges-h425/on-h426-active1/{invocation.json,completion.json,supply.csv}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h425-on-h426-active1/{result.json,dxmt9.log}; src/dxmt9/dxmt9_draw_encoder_chunk.mm; scripts/tools/analyze_publication_progress.py; tests/scripts/test_cpu_supply.py
related: present-pacing-range-encode-body.425.md; present-pacing-range-reuse-activation.424.md
---

# H426: Pass-entry work is the largest DrawRun preparation bucket

Base `7d5e16d2` plus diagnostic-only changes. Five append-only `DrawRunPhase`
milestones partition selected serial DrawRuns under the existing opt-in
`DXMT9_CPU_SUPPLY_DRAW_SPLIT` gate. No query, resource, command, submission or
completion policy changes. Disabled execution has no new observer storage,
clock calls or output; the added sites test the existing cached diagnostic gate.

The analyzer validates sequential milestone values, parent kind, same-thread
monotone clocks and nested draw coverage. It subtracts encodeDraw only from
the final loop remainder. Historical traces remain readable but unqualified
for this new split; invalid/skipped commands remain unclassified. Partial,
duplicate, reordered and cross-thread milestones fail analysis.

## Validation and runtime receipts

Xcode 27.0 / Metal 32023.921 / Apple M1 toolchain checks pass. Canonical Unix
and both PE builds complete. Five focused x86_64 tests pass, including supply
analysis/trace, serial partition, the segmented offloaded range-on query/pixel
oracle and install-name audit. Python analysis tests pass 42/42. The unchanged
TLA suite passes 227 configurations; that is regression evidence, not a new
formal theorem for diagnostics.

The Day/high diagnostic run uses the same requested configuration as H425;
the analyzer checks its SHA and each of the five loaded receipts against the
current build artifacts. Rebuilding changes the Unix provider and d3d9 PE
receipts, so this is explicitly **not** a same-image comparison against H425.
The loaded Unix receipt is `19ac4abc19bd90be`. The run passes, exits zero and
reports zero GPU command-buffer errors. Presents 1850–1854 form a closed trace
with zero overflow/incomplete rows. No build, model checker or profiler runs
alongside this workload.

## Exclusive partition of five large source bodies

Sequences 4596, 4599, 4602, 4605 and 4608 contain 354–376 DrawRun commands and
1,259–1,559 encodeDraw calls each. Their body CPU totals 108.693 ms.

| Component | CPU total | Scope |
|---|---:|---|
| encodeDraw | 55.146 ms | Existing nested draw endpoints |
| Pass entry | 22.466 ms | Exact hazard/entry decisions, clear handling, any pass close/open or mid-chunk commit, continuation and read tracking |
| Per-draw loop residual | 17.018 ms | Work after command preparation excluding encodeDraw: overrides, uniform/binding transitions, per-draw setup/post-state and partition-loop overhead |
| Uniform/setup | 3.232 ms | Command materialization and initial state/handle setup |
| Flush-blit/hazard setup | 1.721 ms | Blit flush, attachment key and prepared/fallback read-hazard construction |
| UP setup | 1.672 ms | UP prescan and optional upload path |
| Binding-plan setup | 1.429 ms | Merge-policy setup and command-level CommandBindingPlan construction |
| Other body work | 7.009 ms | Non-draw commands, per-record split policy and dispatch residual |

The six new preparation/loop buckets sum to 47.538 ms, exactly the inclusive
DrawRun CPU minus encodeDraw. This is conservation within one diagnostic run,
not a comparison with H425's 40.359 ms. Inputs and observer overhead differ.
Small intervals include high-frequency native CPU clock overhead; bucket names
do not claim algorithm-exclusive cost. In particular hazard decisions are also
inside pass entry, and UP setup time does not prove an upload occurred.

Nine complete small bodies are still excluded by the 128-command observer
threshold; four cost 6.267–6.772 ms each. Thus this is not all-window Encode CPU.
The window has 545 release requests (265 reused), 11.387 ms total round-trip
elapsed. H424's long blocked-prefix workload is not independently requalified
here, and source ordinals across runs must not be equated.

## Next bounded candidate

Prioritize pass entry over command-level binding-plan construction or UP
preparation. Distinguish ContinueActive from actual pass transitions, then
separate startRenderPass preparation from encoder/driver calls and read tracking.
The existing bucket is too broad to justify eliding any hazard/store/query
boundary. The second owner is per-draw setup/post-state, not another uniform
arena redesign. Keep range default OFF; no FPS or overhead-free product-cost
improvement is claimed. Broader small-source and wait-linked coverage remains
open before assigning a whole-frame improvement ceiling.
