---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Current-image app thread: guest PC attribution and unresolved Unix CPU"
type: evidence
status: partial-attribution
updated: 2026-09-16
source: experiments/output/app-thread-r634-20260916.noindex/campaign.json; experiments/output/app-thread-r634-20260916.noindex/analysis.json; experiments/output/app-thread-r634-20260916.noindex/audit.json; experiments/output/app-thread-r634-20260916.noindex/timeprof/failure-audit.json; build/meson-logs/app-thread-r634-final.txt
related: present-pacing-cpu-supply-critical-path.312.md; present-pacing-app-thread-waits.305.md; overview.md
---

# Current-image application-thread attribution

The current 05/06 GT2 **stopped Win32-PC population** is predominantly in the
game executable: 70.28% / 81.06%. The dxmt9 `d3d9.dll` population is
10.26% / 5.83%. This narrows the location of application-thread activity, but
does **not** close a game/Wine/PE/synchronous-Unix exclusive CPU-time budget.
Bridge PCs can represent a saved guest context during Unix execution or wait.

Native stack sampling identifies commit/import and buffer Lock/Unlock as
recurring synchronous-provider contexts. Its Rosetta/Wine unwinds are not
reliable enough for exclusive CPU attribution. A Time Profiler follow-up
failed to produce an exportable trace and is explicitly rejected. Runtime
code, defaults and README FPS remain unchanged.

## Images, recipe and qualification

Measured source HEAD is `b74a2023`; all five images match the frozen images
used by [H312](present-pacing-cpu-supply-critical-path.312.md). There is no
new runtime overlay or rebuild. `campaign.json` preserves full SHA256s,
commands, environment overrides, host-process snapshots and complete frame
rows. `audit.json` rechecks unchanged build images, staged-image receipts,
actual Wine-root images and stable result/log hashes after the campaign.

| Image | SHA256 |
|---|---|
| PE32 `d3d9.dll` | `64bd5838c2be3404b8fd6112d7a28e8fe815da3f12f9441ecffdba0a55a31136` |
| Unix provider | `7940d1667e32efb2c8698768e90eaf9c0eb1c482af2b975e5b2d59e34263b030` |

Sikarugir cx24.0.7, perf profile, explicit Info logging and frame sampling;
05 is 1024x768 and 06 is 1280x720. Offload is ON. CPU-ready Tape, segmented
transport, inline constant delta, lock-scope refinement, precise uniform
invalidation and opaque-depth optimization are OFF, matching H312. The new
CPU-supply trace is OFF. PE module-map logging is ON; only the PE diagnostic
runs enable the 250 Hz full-self-PC sampler. The 05 focus keepalive is
disabled to avoid late log writes. No task-owned build/test/other benchmark
overlaps the runs; unrelated host activity is not proved absent.

Completed order is 05 native, 05 PE, 06 native, 06 PE. Run directories are
`experiments/output/app-d3d9-3dmark{05,06}-app-thread-r634-{05,06}-gt2-{native,pe}`
with matching product numbers. All four official results pass. Eight checked
rejection/GPU/pipeline counters are zero at covered endpoints. The 06 scene
finishes before the expected result-GUI timeout.

| Run | Scene frames | Scene wall (s) | Uncovered full-health tail frames |
|---|---:|---:|---:|
| 05 native | 1613 | 64.739 | 0 |
| 05 PE | 1853 | 64.648 | 0 |
| 06 native | 1058 | 64.077 | 22 |
| 06 PE | 1142 | 64.071 | 18 |

Four saved screenshots show the forest/firefly scene rather than a menu or
black screen. They are spot observations, not full-scene pixel/temporal
equivalence. Diagnostic FPS is not promoted or used for README comparisons.

## Guest-PC partition: location, not CPU time

The sampler suspends the target Win32 thread, reads its context and resumes
it. This is not a CPU-running-only profiler. All reports have zero
suspend/context/resume failures, no target mismatch, complete full-self-PC
coverage and conserved module totals. Startup-inclusive cumulative totals are
not used: the analysis subtracts two interior Present reports.

| Population | 05 GT2 | 06 GT2 |
|---|---:|---:|
| Interior PE Present report interval | 120→1740 | 1080→2040 |
| Sample count | 12,033 | 11,811 |
| Game executable | 70.28% | 81.06% |
| dxmt9 PE: `d3d9.dll` | 10.26% | 5.83% |
| Bridge / potentially saved Unix context: `winemetal_dxmt9.dll` | 11.18% | 7.30% |
| Wine system PE DLLs | 5.17% | 2.07% |
| Support libraries | 2.90% | 3.62% |
| Unknown | 0.21% | 0.13% |

Wine-system here means `ntdll.dll`, `win32u.dll`, `kernel32.dll` and
`user32.dll`, **not all Wine-native execution**. Support DLLs are reported
separately, without guessing their caller or implementation provenance:
MSVCR/MSVCP/MFC71, UCRT/MSVCRT, libc++, and 06's `d3dx9_28.dll`. The latter
alone is 2.09% of 06's population. CRT work cannot be assigned to game or PE
without reliable calling context.

These are interior scene selections with different Present populations, not
same-time/same-source samples across runs. Native and PE samplers ran in
separate processes. **Do not add native provider-context percentages to the
bridge row; do not multiply this table by H312's app CPU ms/P.** A saved
bridge PC, actual Unix work, Wine transition overhead and elapsed waiting
cannot be separated by this guest-context observation alone. The table also
does not establish an Amdahl/FPS ceiling for PE optimization.

### Current PE symbol hints

`05-interior-selfpc.csv` / `06-interior-selfpc.csv` preserve all 1,235 / 688
dxmt9-PE samples. `05-pe-symbols.json` / `06-pe-symbols.json` join their
64-byte buckets to the matching PE32 image, with image and joiner hashes in
`audit.json`. Buckets crossing symbol starts stay unassigned: 300 / 168
samples, about 24% in each product. Nearest-symbol padding and inlining still
limit attribution; lexical family totals are not inclusive helper costs.

Recurring non-crossing labels include `ValidatedConstWrite<16u>::applyNonEmpty`
(69 / 34 samples), `SetVertexShaderConstantF` (47 / 23),
`PeSemanticBatchOwner::accumulateRetentionDelta` (37 / 30),
`writeSparsePlanPayloadQualified` (38 / 14) and `buildSparseStatePlan`
(36 / 14). These identify candidate code, not separately measured ms or
evidence that a particular helper rewrite will move FPS.

## Native view: synchronous entry candidates, not an exclusive budget

`/usr/bin/sample PID 20 1 -mayDie` was attached after approximately ten
seconds of scene work. The parser selects only the exact native TID recorded
by `unix_commit_chunk_entry`, with PID/start-time identity from the calibrated
native CPU observer. It conserves tree sample counts separately from inclusive
stack contexts.

| Native observation | 05 GT2 | 06 GT2 |
|---|---:|---:|
| PID / native app TID | 52700 / 77561522 | 61160 / 77579415 |
| App-thread stack sample occurrences | 14,088 | 14,785 |
| Opaque innermost leaf | 100% | 100% |
| Provider somewhere on stack | 1,693 (12.02%) | 1,096 (7.41%) |
| Repeated unwind PC more than eight times | 1,795 (12.74%) | 1,151 (7.78%) |

All leaves resolve to unknown code or Rosetta JIT/runtime, while some unwind
chains repeat Wine dispatcher PCs hundreds of times. A nearest visible PE
return frame is not the currently executing PC. These native stacks cannot
produce trustworthy exclusive game/Wine/PE/Unix weights. No CPU-weighted
running/waiting partition is claimed.

Within the uncertain provider contexts, the nearest named bridge entries are:

| Entry label | 05 occurrences | 06 occurrences |
|---|---:|---:|
| `dxmt9c_device_commit_chunk_impl` | 777 | 605 |
| `dxmt9c_device_commit_chunk` outer label | 63 | 49 |
| `dxmt9c_buffer_unlock` | 539 | 271 |
| `dxmt9c_buffer_lock` | 287 | 151 |
| Other provider entries | 27 | 20 |

These are context counts, not entry call counts, CPU ms or savings. They
support investigating commit/import and Lock/Unlock boundaries before another
small PE-only rewrite, subject to direct timing confirmation.

The separate partial native CPU envelopes cover 94.85% / 82.21% of scene
frame intervals and report 0.803 / 0.853 cores, unlike H312's 0.94–0.96.
Intrusive stack capture, endpoint differences and run variability prevent a
clean overhead estimate. Do not replace H312's baseline with these values or
call their lower FPS a runtime regression.

### Rejected Time Profiler follow-up

The `metal-gpu-debug` xctrace environment/TOC procedure was used with **Time
Profiler only**, without GPU capture. Xcode, template availability and native
CPU calibration passed. The unchanged-image 05 follow-up attached to PID
79999 / native TID 77615567 for a requested 20 seconds.

Recording reached its time limit but did not return a successful save receipt.
The driver raised `TimeoutExpired` while waiting 30 seconds after benchmark
runner exit. The retained trace contains only `Trace1.run/RunIssues.storedata`;
TOC export fails with exit 10, `Document Missing Template Error`. No profiler
process remains. `timeprof/failure-audit.json` preserves the command-related
artifacts and export failure. The planned 06 profiler run did not start.
Neither this trace nor its benchmark FPS enters the attribution verdict.

## Remaining measurement and decision

The supported conclusion is **app-thread supply constraint plus a game-heavy
guest-PC population**, not “PE is finished,” “the game owns exactly 70–81%
of CPU,” or “Unix is only the bridge percentage.”

H312 already measures actual CPU inside `dxmt9c_device_commit_chunk_impl`
on these same images: 0.816–0.911 ms/P (05), 1.084–1.200 ms/P (06), in its
own qualified windows. That is a **subset** of synchronous Unix, excluding
other direct entry points and PE/Wine crossing. It must not be treated as the
full synchronous cost or combined with this run's PC shares.

The next boundary measurement should extend calibrated native-TID CPU plus
elapsed accounting to synchronous buffer Lock/Unlock and the remaining
app-thread bridge entries, with non-overlapping outer scopes, explicit nested
scope handling and an observer-OFF control. Account for provider-called
allocator/driver work by causal scope, separate waiting from CPU, and preserve
an unknown residual. Four-way exclusive CPU closure still needs a qualified
Wine/guest execution attribution method; changing an unresolved PC label is
not a substitute. No scheduling, lock, ownership or representation change is
justified by this diagnostic alone.

Validation: four existing sampler/symbolication/native-clock/observer test
targets pass before collection and again in `app-thread-r634-final.txt`.
The final audit checks image
identity, stable evidence hashes, run health, exact module/sample conservation
and rejection of the unusable profiler trace. This is evidence qualification,
not a rendering-policy promotion.
