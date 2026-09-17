---
domain: present-pacing
workload: STALKER CoP Day / D3D9 query conformance
title: "Default asynchronous query completion and bounded END submission locality"
type: evidence
status: accepted-verdict
updated: 2026-09-17
source: experiments/output/query-fences-r637-20260917.noindex/{off1,on1,on2,off2}.json; experiments/output/query-fences-r638-20260917.noindex/{off1,default1,default2,off2,default-x64,default-x86,analysis,diagnostic,diagnostic-analysis,diagnostic-retry,diagnostic-retry-analysis}.json; build/meson-logs/query-fences-r638-{settled-native,resource-regression,range-regression}-summary.log; build/meson-logs/query-fences-r637-locality-tla.log
related: present-pacing-query-publication-fences.315.md; overview.md
---

# Default asynchronous completion; END still publishes eagerly

`DXMT9_ASYNC_QUERY_FENCES` now defaults ON. Explicit `0` or an empty value
retains the synchronous reference path. END captures an actual queue/initializer
prefix; pending GetData(FLUSH) requests submission and returns `S_FALSE`.
Only successful completion of both prefixes permits `S_OK`. Readback, resource
waits, reset and teardown retain their blocking contracts.

This does **not** make the whole public GetData entry wait-free: PE settlement,
bridge replay drain and queue locks remain. Nor does it upgrade core occlusion
or timestamp data into real Metal visibility/counter results.

## Safety and locality changes

The fixed `query::UploadTracker` now owns the initializer's retained FIFO,
admission, contiguous successful-completion prefix and sticky terminal failure.
Production and fault tests call the same reaper with different status providers.
Event signaling alone never authorizes completion. Failed entries and their
unobserved suffix cannot be overwritten to admit new uploads.

The queue distinguishes three facts:

1. END has **published** an ordinary source prefix.
2. A Metal command buffer containing that prefix has actually been **submitted**.
3. The ordinary and initializer prefixes have **successfully completed**.

The stored submission request remains sticky, but actual submission now
discharges its splitting pressure. Waiting for an old query no longer forces
unrelated suffixes to submit merely because that request remains stored.
Repeated polls of already requested/submitted prefixes also avoid duplicate
encode/finish CV notifications. Neither change advances completion/reclamation.

This is a bounded END-related locality improvement, **not deferred END
coalescing**. The ordinary writer is still eagerly published at END. Sharing
several ENDs in a larger writer requires an admitted marker-to-publication
identity and its own refinement evidence; predicting the next sequence remains
forbidden. No standalone FPS gain is attributed to the locality change.

## Evidence gates

| Layer | Result and claim boundary |
|---|---|
| Four builds | Full native, x86_64 Unix, PE x64 and PE x86 builds pass; canonical Unix build performs install-name fixup before staging |
| Focused native + audits | 12/12 pass on settled builds, including queue/session/early-prefix, actual publication/completion, active-Arena rejection and install-name audit |
| Resource/upload regressions | Five additional cases pass: resource lifetime, upload/arena observer ON/OFF and range-upload lock-scope OFF/ON |
| Upload faults | All 64 FIFO error positions; 65th admission rejection without eviction; three wraps; stale/duplicate ordinal, missing event, ordinal exhaustion and sticky allocation-failure state |
| Actual initializer-only GPU | Independent SharedEvent parks the GPU; FLUSH/non-FLUSH polls stay Pending without Present/readback; release permits completion, then all 256 bytes match on each of three uploads (768 total). A newer pending upload does not extend an older receipt. Metal API validation enabled. |
| Bounded TLA | 61 normal + 4 progress + 141 expected-counterexample configurations pass. `QueryPublicationFence` adds actual-submission locality and premature-retirement mutation. `QueryInitializerFence` checks the ordinary/event/successful-upload product and capacity; its 3,651 distinct states reject three dependency/capacity mutations. |
| Final default-mode Wine x64 | Four cases pass, no skips; 200ms delayed completion, issue pending counts 148/195; exact sampled clear/draw pixels and unchanged pending output bytes |
| Final default-mode Wine x86 | Four cases pass, no skips; pending counts 141/135; same pixel oracle and public HRESULT/data-size contracts |

Both Wine lanes enable CPU-ready Tape/direct replay and leave the query-mode
variable **unset**. The fixture issues UP and then buffered VB draws, releases
the VB before query completion, and performs readback only after `S_OK`.
The delayed pending assertion also applies to the unset default.

Fault-provider tests exercise production state transitions, not deliberate
hardware/driver faults. TLA excludes Metal bytes, C++ atomic memory ordering
and unbounded queue histories. GPU tests cover bounded actual execution;
they are not a whole-game exact-frame oracle.

## Clean STALKER comparison

Both campaigns use Sikarugir cx24.0.7, Day/High, `renderer_r2.5`, 1280×720
windowed, perf profile/Warn, offloaded replay ON, CPU-ready Tape OFF. Segmented
transport, inline constant delta, precise uniform invalidation, lock-scope
refinement and opaque-depth index optimization are OFF. Query observer,
perf counters, frame sampling and completion-delay injection are OFF.

The order is OFF → ON → ON → OFF; the final campaign uses the **unset default**
for ON. Each campaign verifies the same five staged images, source and driver
throughout all runs, archives appdata/benchmark outputs and records host-start
processes. No task-owned build/test/profiler overlaps a performance run.

The first campaign uses the original opt-in image from `4248c3d0`; the final
campaign uses the uncommitted H316 safety/locality/default changes over that
HEAD. They are separate image populations, not a cross-image controlled
ablation of locality or tracker cost.

| Campaign/order | OFF first | ON first | ON second | OFF last |
|---|---:|---:|---:|---:|
| R637 average FPS | 13.162791 | 19.477272 | 18.413807 | 10.608938 |
| R637 frame rows | 1,791 | 2,588 | 2,507 | 1,597 |
| R638 final-image average FPS | 8.511958 | 14.048413 | 13.322946 | 8.186486 |
| R638 frame rows | 1,536 | 2,192 | 2,171 | 1,531 |

R638 uses the unset default for both ON runs. All eight runs complete with
zero runner failures, no capture error and actual game-window identification.
`analysis.json` verifies cross-run image/source/driver identity, flags, staged
hashes, result artifacts and presence of the separate scene captures.

The R637 OFF repeats have substantial variance: this supports the observed
direction in both orderings, not one universal improvement percentage.
Both R638 modes are slower than their R637 counterparts. The separate image
populations, time separation and uncontrolled macOS background activity do not
isolate the cause of that absolute decline. The promotion evidence is the
same-final-image OFF/default-ON comparison, not an assertion of zero cost for
the intervening safety/locality changes.
README benchmark tables are intentionally unchanged.

## Separate locality diagnostic

The observer/counter-enabled final-image diagnostic hits the runner timeout
after 391.45 seconds (90-second capture delay plus a 300-second process wait).
It is preserved as a **failed run**, not a completed benchmark. Sampled images
show advancing scenes through the final building, but do not prove completion.

Its last complete periodic prefix covers 1,980 encoded Presents, 16,553 CBs
(8.360/P) and 5,877 published sources (2.968/P), with zero sequence waits and
zero in the eight selected error/rejection counters. The bounded query summary
has its own earlier endpoint; it must not be joined with this prefix as a
single complete census. None of these values qualify the unobserved tail.
They also do not establish lower CB density than H315's different full-run
population. A same-image retry changes only the diagnostic timeout to 600
seconds; failed evidence remains separate.

The retry completes normally after 408.28 seconds and verifies the same five
images as the clean R638 runs. Its diagnostic average is 6.576049 FPS, excluded
from the clean performance comparison. Final cumulative counters are:

| Counter | Completed diagnostic |
|---|---:|
| Encoded Presents | 2,107 |
| Command buffers | 17,614 (8.360/P) |
| Published legacy sources | 6,220 (2.952/P) |
| Queue sequence waits / elapsed | 0 / 0 ms |
| Explicit `submit_flush` | 0 |
| Eight selected error/rejection counters | All zero |

The bounded event sample includes 73 authoritative END receipts with matching
queue incarnations and published ordinary/initializer prefixes. Last query
summaries conserve calls = OK + Pending + Error and report zero errors; their
event population is not a complete trace. Occlusion pending polls remain
visible. GPU command-buffer time and replay/encode elapsed scopes overlap
and are not summed into a CPU budget.

This confirms the completed run no longer enters the old queue sequence wait
path. It does **not** establish an independent FPS or CB-density improvement
from submission-pressure retirement; H315 is a different image/population.

## Visual qualification and preserved failures

Actual game-window scene captures are retained separately as `*-scene.png`
in each campaign directory. Inspected scenes show coherent terrain, buildings
and trees without the previously reported black/repeating pattern. This is
sampled visual evidence, not every-frame or pixel-exact game equivalence.
The runner's early `actual.png` can depict loading; it is not substituted for
the scene captures. The H315 missing-onscreen-window failure did not reproduce.

Intermediate validation failures are preserved, not removed:

- The first selective `--no-rebuild` rerun used two stale native test binaries
  after a queue-header layout change, causing invalid-mutex/SIGABRT failures.
  Rebuilding all affected executables removed them.
- An added Arena test incorrectly expected a single-source abort to roll back.
  That path intentionally fail-stops. The corrected fixture separately checks
  terminal single-source rejection and reversible pre-effect batch rollback.
- One install-name audit overlapped the Unix rebuild before its final fixup.
  The settled canonical build and subsequent 12-test run pass. No measured run
  or Wine staging uses that intermediate provider.

## Remaining work

- Real Metal occlusion/timestamp result providers, independently of fencing.
- Deliberate real driver-fault injection/recovery and broader workloads.
- Full-frame game oracle; sampled visible correctness is a narrower claim.
- Deferred END marker/coalescing with explicit publication identity.
- Isolated measurement of the locality change; combined default-policy FPS
  must not be attributed exclusively to fewer wakes or suffix splits.
