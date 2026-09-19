---
domain: present-pacing
workload: STALKER CoP Day
title: "H326: reference tails and backend query polling costs"
type: evidence
status: measured
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-readme-wined3d-high-day-r3-20260831/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-readme-d9vk-mvk141-high-day-20260831/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-readme-d9vk-mvk141-high-day-20260831/dxmt9.log; experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-backend-poll-cost-r650-day/result.json; experiments/output/app-d3d9-stkcop-bench-backend-poll-cost-r650-day/xrEngine_dxmt9.log
related: present-pacing-stkcop-fps-windows.325.md; present-pacing-query-receipt-cost.324.md
---

# H326: common scene tails do not identify the dxmt9 cost owner

## Retained reference results

The existing WineD3D and D9VK-labelled README Day runs both record High,
`renderer_r2.5`, 1280x720 in their launcher logs. The latter actually loads
`DXVK v1.10.3-20250511-async (macOS)` and MoltenVK 1.4.1; its Wine manifest is
named `sikarugir-cx-24.0.7-wined3d`, which is not the renderer identity.
Both references are from 2026-08-31, not a matched current A/B. [H325](present-pacing-stkcop-fps-windows.325.md)
defines the reconstruction/rounding/scene-alignment limits of the offline FPS
analyzer used here.

| Reconstructed window | WineD3D | DXVK/MoltenVK reference | dxmt9 R648 |
|---|---:|---:|---:|
| Whole-run average FPS | 23.36 | 30.38 | 20.38 |
| First 0–60 seconds FPS | 30.37 | 48.48 | 28.58 |
| 60–70 seconds FPS | 23.50 | 29.50 | 20.70 |
| 70–80 seconds FPS | 17.60 | 20.50 | 14.30 |
| 80–90 seconds FPS | 14.30 | 10.90 | 12.00 |
| 90–100 seconds FPS | 12.00 | 5.60 | 7.60 |
| 100–110 seconds FPS | 17.50 | 13.50 | 11.80 |
| 110–120 seconds FPS | 13.60 | 14.60 | 12.90 |
| Last full 10 seconds FPS | 13.30 | 13.30 | 9.60 |
| Last 10 seconds frames >100ms | 26 | 21 | 36 |
| Last 10 seconds maximum frame ms | 332.3 | 1238.4 | 285.3 |

The last row group uses frame completions in `(total_duration - 10, total_duration]`,
not the final potentially partial ten-second bucket. Total reconstructed
durations differ: 120.387 / 132.742 / 125.556 seconds. Therefore equal elapsed
windows, or equal distance from the end, are not proven identical camera/source
segments. No host-state, cache or current-binary parity is implied.

The shared late slowdown is now artifact-supported, not only a user report.
It is insufficient to conclude GPU saturation, or that dxmt9 has no extra
overhead. WineD3D has a less severe reconstructed 90–100-second trough despite
lower overall FPS than the DXVK reference. Focus on explicit dxmt9 ownership
costs instead of attributing the entire tail to one provider defect.

## Backend observer contract

`DXMT9_QUERY_BACKEND_OBSERVER=counts` records backend poll and request-result
populations. `=1` also samples one in 64 calls. It is separate from the receipt
observer, which remains OFF for R650. Disabled templates remove phase/TLS
operations; the public poll and request seams retain cached mode checks.

The existing queue mutex and pending-completion mutex are timed on acquisition.
Classification includes ordinary/initializer progress reads. Submission decisions
are observed under their original lock: rejected, already submitted, existing
requested prefix, or new target. Already-submitted takes diagnostic precedence
when both reuse predicates hold, without changing their existing OR condition.
Calls to requestQuerySubmission outside an observed poll are not counted.
No extra GPU poll, runtime mutex acquisition, wake, submit or completion wait
is introduced. Final aggregation uses a diagnostic mutex after runtime locks
are released; its overhead is excluded from sampled intervals but perturbs the
diagnostic run. Reports are fixed-size and capped at 64 periodic plus final.

Phases are disjoint elapsed intervals, not thread CPU: queue acquisition,
progress/classification, deferred marker, request setup, pending mutex
acquisition, request decision, wake, and exit/unlock remainder. New-request wake
includes leaving the pending mutex scope. Queue residency after acquisition is
not universally continuous lock hold because deferred-marker publication can
release/reacquire it. Sample maxima are not full-run maxima; periodic sampling
can miss or alias stalls. Do not scale sampled sums into a claimed frame cost.

## R650 diagnostic result

Base `25ebcac3` plus this backend observer. Sikarugir cx24.0.7, Day High r2.5,
1280x720, perf profile, Info logging, health counters ON and validation OFF.
`DXMT9_QUERY_BACKEND_OBSERVER=1`; receipt/legacy query/frame observers OFF;
actual occlusion and async fences ON; END coalescing OFF. The canonical
`run_experiment.py run app-d3d9-stkcop-bench` invocation uses
`--wine-id sikarugir-cx-24.0.7 --timeout 300 --capture-delay-sec 120
--output-suffix backend-poll-cost-r650-day`. All five staged size/hash prefixes
match canonical outputs (Unix `4b80080814efe3e8`); ABI handshake succeeds.
Builds, tests and TLA finish before launch; no session-scheduled compute
overlaps runtime and spot process checks find none. Other user/OS activity is
not excluded. No window restore is performed.

Normal exit, returncode 0, runner PASS, no timeout or failure markers, 2,424
benchmark rows, 2,513 encoded Presents. All eight standard health counters
are present and zero. **Capture fails to find the app window**; `actual.png`
is a fallback desktop image, not STALKER evidence. This run adds no visual
qualification. Its **18.029873 FPS** is diagnostic/focus-unqualified, not a
performance comparison or non-regression result.

| Complete backend census | Count |
|---|---:|
| Polls / FLUSH polls | 541,168 / 541,168 |
| Complete / Pending | 25,369 / 515,799 |
| Invalid / Failed | 0 / 0 |
| Pending request: existing not-yet-submitted target | 262,162 |
| Pending request: target already submitted | 252,660 |
| Pending request: new target / wake path | 977 |
| Rejected request / deferred-marker publication | 0 / 0 |

Request decisions sum to Pending; no-request calls equal Complete. **99.81%**
of pending requests reuse an existing request or submitted prefix. Only 0.19%
take the new-target notification path. The existing sticky-request deduplication
already works; repeated GetData does not mean repeated encoder wakeups.

| Phase | Sum over 8,456 samples, ms | Sampled share | Sample maximum, us |
|---|---:|---:|---:|
| Queue mutex acquisition | 51.655807 | 87.73% | 883.375 |
| Progress/classification | 1.600150 | 2.72% | 21.041 |
| Marker branch/check | 0.717444 | 1.22% | 0.292 |
| Request setup | 1.300991 | 2.21% | 12.208 |
| Pending-completion mutex acquisition | 1.011375 | 1.72% | 25.000 |
| Request decision | 0.744731 | 1.26% | 33.750 |
| New-request unlock/notify | 0.017498 | 0.03% | 4.791 |
| Exit/unlock remainder | 1.834769 | 3.12% | 25.584 |

Zero invalid clocks; disjoint sampled total 58.882765 ms. The marker phase
still measures the normal branch/interval boundary when no marker is present;
its nonzero timing does not imply any deferred work occurred. With zero marker
paths in this run, post-acquisition intervals approximate queue-mutex residence,
including a small unlock/return remainder; do not sum them with the enclosing
receipt backend timer from another run.

**Next owner is queue acquisition, not wake deduplication.** This identifies
where sampled backend calls spend time, not why the holder is delayed or how
much FPS is recoverable. The largest sampled acquisition is under 1ms; this is
not evidence that queue acquisition explains 100–600ms frame stalls. Rare waits
can be missed and there is no source/frame join. Next: bounded queue-lock-owner
attribution and a model-backed read-mostly completion/submission receipt fast
path, preserving terminal/reset identity and FLUSH's persistent submission
obligation. Do not remove waits or notifications based solely on these totals.

## Verification results

Native sampled-mode tests pass 7/7, including production bridge, actual Metal
snapshot, initializer-only completion and deferred-END oracles. Counts-only
and observer-OFF initializer/deferred tests pass 3/3 each. x86_64 tests pass 4/4,
including those completion oracles and install-name audit. Fake-clock tests
pin disjoint nested-mutex intervals, maxima, invalid-clock exclusion, result
and request populations, and exactly-once/nested TLS restoration. Full native,
Unix and both PE builds pass. Full TLA verification passes without a policy or
model change. Logs: `build/meson-logs/query-backend-observer-{tests,counts-tests,off-tests,x86-tests,tla}.log`.
