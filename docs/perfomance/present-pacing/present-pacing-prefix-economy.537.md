---
domain: present-pacing
workload: Same H502/r5 STALKER late input, H506 production-counter diagnostic
title: "Prepare-prefix candidate clears the fixed same-input CPU screen"
type: evidence
status: offline-screen-passed-live-transfer-and-mechanism-witness-open
updated: 2026-10-14
source: traces/stkcop-prefix-economy-h537/{contract.md,build_pair.py,build-receipt.json,base-build.log,private-build.log,run_pair.py,rows.jsonl,summary.json,verify_raw.py,verification.json,check_execution.py,execution-witness.json,execution-witness.log,build_witness.py,witness-build.json,counter-witness-blocked.md,counter-witness.log}; traces/stkcop-prefix-economy-h537/{image-A,image-B}.log
related: present-pacing-prefix-prepare.536.md; present-pacing-removal-scope.535.md; present-pacing-self-counter-boundary.513.md; present-pacing-validation-live.509.md
---

# Same-input CPU reduction, not a default frame budget

The [H536](present-pacing-prefix-prepare.536.md) transaction-local suffix prepare
candidate passes its frozen offline comparison: **3.379 ms paired-median planned
Replay CPU reduction per captured interval**, above the **1.347 ms maximum A/A
difference**, with all **8/8** pairs positive. All 30 executions retain exact
original source/output pixels and the recorded source/query obligations.

This is not a live ms/frame, FPS gain, exclusive validation cost, or proof of
the largest default dxmt9 CPU owner. It is a private V4 diagnostic with setup
and replay policies fixed to H506. Background host CPU load remained present.

## Matched build and measurement boundary

- Rebuild current release/x86_64 sources through Meson including the unix
  install-name fixup; preserve pre-build images and verify unchanged build options.
- Copy archives and relink private A/B images. The only arm difference is the
  queue object's `DXMT9_EXPERIMENT_PREFIX_PREPARE=0/1` definition. Both use the
  same provider archive with the per-validation TLS probe disabled. The runner
  executable is byte-identical; observer-OFF startup logs verify loaded images.
- Authenticate the unchanged H502/r5 bundle and unchanged H506 runner.
  Seven source mappings, 670 created/released objects, nine query issues, five
  polls, four defined results and five readiness obligations match each time.
  Both 3,686,400-byte pixel outputs match the original SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- Query semantics are the captured production-counter profile, **not** a true
  GPU occlusion-result oracle. `gpu_oracle=0` remains explicit.
- Two observer-OFF image checks, two warmups per arm, four A/A pairs, then eight
  alternating A/B–B/A pairs. No retries, outlier removal or sample extension.
- Every timed execution contains all 259 raw scopes and zero per-validation
  events. H513's self-query distinction applies; this does not rehabilitate
  H512's stale external-counter endpoints. The common V4 qualification and
  boundary observers remain, and their cost is not subtracted.

The Metal skill's environment preflight passed on Apple M1, Xcode/xctrace 27.0
and Metal compiler 32023.921. In-process pixel readback is used; no gputrace,
shader-validation overhead or GUI capture is added to the timing population.
No Wine installation files were staged. Canonical build outputs were rebuilt
with the **default full-prepare** path; experimental images live only in H537.

## Fixed comparison

| Scope | A median | B median | Paired median saving | Max absolute A/A | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Planned Replay CPU | 61.0855 ms | 57.7065 ms | **3.379 ms** | 1.347 ms | 8/8 |
| Entire replay interval excluding readback, secondary | 128.928 ms | 125.492 ms | 3.406 ms | 1.282 ms | 7/8 |

The primary reduction is about 5.5% of A's captured planned-Replay median.
These scopes nest and must not be added. Secondary interval variation includes
other mutations/definitions/control and is not a separate saving.

Independent CSV subtraction reproduces all **7,252** timed raw scopes and the
primary gate. An exploratory join against H507's same-input prefix membership
partitions mean paired saving into 2.95825 ms in 197 prior-prefix raws and
0.307 ms in 62 other raws. The two means conserve the 3.26525 ms mean total.
This supports the proposed mechanism but is historical population membership,
not a fresh runtime count or an exclusive causal cost decomposition.

## Supplemental execution witness remains missing

After the fixed matrix, an unpriced LLDB run stalled at Rosetta launch before
any breakpoint hit; the bounded debugger was stopped. A separate count-only
image then also stalled at process creation with no target output. Neither is
passing evidence, neither is a timing sample, and neither replaces H536 tests.
The retained note documents the debugger cleanup and the still-`U` target
PID 51619, including its exact path. SIGKILL did not remove it at observation;
host Rosetta services were not restarted. The cause is unproven.

The private probe-suppressed build has one expected unused-function warning
for the disconnected `observeProviderSlotValidation`; production source is
unchanged. A witness-build script's `ranlib -c` classification error was fixed
before linking; it did not affect the A/B binaries or samples.

## Consequence and next gate

There is now finite same-input evidence of a useful **net candidate CPU effect**;
there is still no canonical/default removal amount. Keep the runtime default
unchanged. Resolve the supplemental launch state and establish fresh execution
counts, then use current-thread self reports for a matched-scene live transfer.
Do not reuse H509's external endpoint accounting, convert this captured-interval
saving directly to frames, or treat Replay/Encode diagnostic ranking as global
default ownership. Common-observer qualification remains an independent gate.
