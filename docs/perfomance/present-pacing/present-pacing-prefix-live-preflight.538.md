---
domain: present-pacing
workload: STALKER late-scene prefix-prepare live transfer preflight
title: "Post-reboot launch recovered; mechanism and matched image oracles pass"
type: evidence
status: offline-preflight-passed-live-not-executed
updated: 2026-10-15
source: traces/stkcop-prefix-live-h538/{contract.md,build_pair.py,build-receipt.json,build.log,verify.py,verification.json,prepare_adapter.py,adapter-build.json,run.py,preflight.py,launch_guard.py,analyze_run.py,compare.py,test_adapter.py,adapter-tests.json,resume-exec-policy.log,resume-exec-policy.log.json,resume-sample-launch.log.json,resume-sample-oahd.log,resume-sample-oahd.log.json,post-reboot-launch.log.json,preflight.json,preflight-witness.log,preflight-A.log,preflight-B.log,preflight-witness-process.json,preflight-A-process.json,preflight-B-process.json,inputs.json,clocks.json,post-reboot-host-readiness.txt}; traces/stkcop-prefix-economy-h537/{launch_control.cpp,bounded_exec.py,launch-control.log.json,system-x86-control.log.json,watchdog-negative.log.json,counter-witness-blocked.md}
related: present-pacing-prefix-economy.537.md; present-pacing-source-self-thin.516.md; present-pacing-boundary-comparison.534.md
---

# Offline preflight passed, not live evidence

Follow-up: [H540](present-pacing-prefix-live-result.540.md) subsequently runs
the fixed population on October 15. All six games complete, with explicit
deferred-restoration recovery; the amended live economy screen is negative.
The preparation chronology below remains historical, not a current launch gate.

After the user-reported reboot, the standalone x86_64 control and all three
offline probes exit normally without timeout or residual child. The previous
launch blocker is no longer present; its cause remains unproved. H537's missing
runtime mechanism witness and H538's matched image checks now pass. The fixed
six-run live CPU comparison has not started. Historical failures below are
retained, not current blockers.

## Post-reboot execution, 2026-10-14 20:52 KST

Evidence: `traces/stkcop-prefix-live-h538/post-reboot-launch.log.json`,
`preflight.json`, `preflight-{witness,A,B}.log`, and the corresponding
`preflight-{witness,A,B}-process.json` receipts. The preflight authenticates the
frozen H502/r5 input and private images; independent verification rechecks all
34 receipt hashes and recomputes the witness totals from the actual log.

- 199 accepted suffix prepares skip 75,300 prior-prefix commands and validate
  392 new commands, matching the earlier mechanism prediction.
- Both matched A/B images and the count witness reproduce original source and
  output pixels: each 3,686,400 bytes, SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- Each run maps all seven sources and creates/releases 670 objects. All query
  obligations pass: 9 issues, 5 polls, 4 results and 5 readiness observations.
  This remains production-counter occlusion (`gpu_oracle=0`), not proof of true
  GPU occlusion results.
- Xcode/Metal tools execute, and archive parity, branch symbols and numerical
  screen checks pass. No Wine staging or game launch occurs.

These are correctness/mechanism checks, not new timing samples. Reboot-time
background work remains active, including ANECompilerService near one CPU
core at the post-check census. Defer live timing until background load settles;
do not kill unrelated services. Live economy, common-observer-to-canonical
qualification and largest default CPU-owner attribution remain open.

## Live preparation completed, timing not started

`run.py prepare` completes after recovery. `inputs.json` freezes 22 input hashes;
`clocks.json` passes ten monotonic-clock bracket checks on each of ARM64 and
x86_64. Independent rereading confirms all hashes and all twenty brackets.
Adapter reproduction, historical-parser and synthetic-screen tests still pass.
No A0/A1/A2/B1/B2/A3 receipt exists and no Wine files have been staged.

The retained 20:58:03--20:58:43 host census shows ANECompilerService at
96.2--99.4% CPU in the four interval samples (the initial top sample is not an
interval rate). Other background work also varies. This is an observed
timing-readiness hold, not a renewed execution failure or an optimization
result. Do not consume the fixed comparison runs under this known interference.

The comparison will test private-image live Replay economy, not automatically
close canonical transfer: both arms still share sparse worker reports and the
boundary-only PE reporter. The controlled workload also retains max-threads2
and snd_targets16; it must not be relabeled an untuned application baseline.
`DXMT9_METAL_OCCLUSION=0` matches the current provider default, but remains a
production-counter query policy rather than a real GPU occlusion oracle.

## Post-reboot background-load follow-up

The later post-reboot host check is retained separately in
`traces/stkcop-prefix-live-h538/post-reboot-host-readiness-2.txt` and
`post-reboot-appstore.sample.txt` (SHA-256
`9438ccd667c9cd668de613d6595cbc8b39428f533231ad627c2657d138c7d77c`).
The five-second read-only sample exits successfully under the bounded watchdog
(`post-reboot-appstore-sample.log.json`); it does not launch a renderer.
Appstoreagent again spends repeated stacks in BiometricKit connection setup,
bootstrap lookup and `objc_sync_enter`, matching the pre-reboot stack family.
These are stack occurrences, not CPU-percentage attribution. Independent top
intervals show appstoreagent near one CPU and simultaneous ANE compiler/security
scanner load. Thus reboot recovered target execution but did not permanently
clear the independently observed background contention. Its root cause and
relationship to the old target-launch stall remain unproved. No service is
restarted, killed or disabled; live comparison has still not started.

## Original preparation

Private current-source A/B Unix images are built for the next live experiment.
Both contain the same H516 Replay/Encode current-thread CPU observers; only
`DXMT9_EXPERIMENT_PREFIX_PREPARE=0/1` differs. This does not add a new timing
sample, establish live CPU reduction, or promote the candidate.

The existing [H537](present-pacing-prefix-economy.537.md) result remains scoped
to one captured interval: 3.379 ms paired-median planned-Replay reduction,
not ms/frame or FPS. Canonical largest-owner attribution remains unresolved.

## Historical execution preflight failure (before reboot)

A new standalone x86_64 program containing only `puts` and exit0 also stalls
before target output. It has no dxmt9/Wine/Metal dependency. Its PID62065 remains
in `Us`, and the earlier H537 PID51619 remains in `U`; exact-child SIGKILL did
not remove them. A fork-before-exec watchdog returns after its deadline and
records residual processes instead of waiting indefinitely.

The system `/usr/bin/arch -x86_64 /usr/bin/true` control succeeds immediately.
An owned sleep timeout control is killed and reaped successfully. Thus this is
not shown to be a dxmt9 execution failure, but neither a global Rosetta outage
nor its root cause is established. No host services or caches were changed.
No game was launched or Wine file staged in this preflight. Host recovery is
needed before further target execution; do not accumulate stalled copies.

## Private build boundary

Meson confirms canonical inputs are current, including the unix install-name
fixup. H516 hooks are applied to private copies of only three translation units:
Replay offload, queue execution, and queue lifecycle binding. Canonical sources
and images are hash-unchanged. Archive inspection verifies A/B differ only in
the queue object; common observer objects are identical. The candidate object
references `prepareOwnedSuffix`; the reference does not. This is build evidence,
not fresh execution witnessing. No new production code is added in H538.

Unlike H537's offline provider, the live pair leaves the production validation
observer object unchanged; that observer is disabled during the proposed live
run. Live economics must therefore be measured, not imported from H537.

## Frozen next comparison

The contract fixes six runs: A0/A1 control, then A2/B1 and B2/A3. Both arms use
the same H533 boundary-only PE, H518 clock service, Day/high1280x720 workload,
H502 spatial window, and sparse self-CPU cursor accounting. Do not multiply
sampled Replay scope sums by256. Require both opposite-order lower reduction
bounds to exceed the full A0/A1 difference envelope. Primary is Replay CPU per
observed completed frame; report whole-scene totals, frame counts, Primary and
Encode bounds, and every component interval too.

Before timing, close H537's missing unpriced mechanism witness and check exact
same-input pixels/query obligations on both new images with observers OFF.
Common-observer-to-canonical qualification, live economy, and global ownership
remain independent gates. This preflight prepares images and a tested numerical
screen; the live launch/staging adapter is now derived and tested without target
execution. Its actual host/Wine integration remains unexecuted.

## Adapter preparation while execution is unavailable

`prepare_adapter.py` derives H534's catalogue-only staging/cleanup supervisor
with the same H533 PE and controller in both arms, H538 Unix image selection,
six-run order, unique output names and a pre-staging recovery/correctness gate.
The unchanged H506 runner is relinked against H538 inputs and copied byte-identically
to A/B. No binary is launched during this preparation. The inherited cleanup
also refuses restoration while Wine processes remain alive.

`preflight.py` runs the separate unpriced count witness and two original-pixel
checks only after recovery, with a fork-before-exec watchdog. Its authenticated
receipt is required before any live preparation/staging. The analyzer uses
boundary-only Primary maps for both arms; no all-root attribution is invented.

Non-executing verification passes:

- Generated adapters exactly match the checked derivation; all image/input hashes
  match. These checks do not claim target runtime correctness.
- Seven entry points (prepare plus six runs) reject a surviving known diagnostic
  process before target launch or file staging. PID reuse with a different path
  does not falsely match; a new PID with the same image does match.
- Reparse H534 A1/A2's actual self-CPU and boundary maps (4,096 and 3,872 rows).
  All 36 stage/interval CPU envelopes reproduce the retained analyses exactly.
  This tests the parser, not a new H538 measurement.
- Synthetic no-effect, reduction and corrupt-counter cases exercise the full
  six-run comparison, retaining `default_transfer=false` in every result.

An actual `run.py prepare` invocation still rejects the two known `U` processes
before creating live inputs. Both remain present after three consecutive goal
turns containing the same launch blocker. No further meaningful live attribution
or reduction verification is possible without an external launch-state change.

After recovery, recheck residual processes and the current computer-use guide
before GUI operations. Execute `preflight.py`, then `run.py prepare`, then the
fixed six labels in contract order; analyze each using `analyze_run.py <label>`
and finally `compare.py`, all through `scripts/run_python.sh`. Any failure
remains evidence; do not extend the population or silently replace a run.

## Read-only resume diagnosis, 2026-10-14 16:26 KST

Both original diagnostic PIDs still exist with the same paths in `U`/`Us` after
more than ten hours. A ten-second bounded `sample` of PID62065 produces no
stack and is stopped/reaped by the watchdog. Sampling `oahd` is denied for
insufficient privileges. Neither sample process remains; no target is relaunched.

A fixed four-second unified-log window, 16:26:20 through16:26:24, contains
456 error rows from `syspolicyd`'s execution-policy subsystem:

- 304 `Unable to initialize qtn_proc: 3` messages.
- 152 `dispatch_mig_server returned 268435459` messages.

The current SDK's `mach/message.h` defines hexadecimal `0x10000003` as
`MACH_SEND_INVALID_DEST`; this is decimal268435459. The retained messages do not
identify the stalled target PIDs. They support investigating execution-policy
IPC, but do not establish the stall's cause, a Rosetta outage, or that restarting
one service will repair it. No privileged commands, service restarts, quarantine
changes or cache deletion are performed. Recovery or authorized host-level
diagnosis is still required before live CPU attribution can continue.

## User-authorized privileged stackshot, 16:56 KST

The user collected `traces/stkcop-prefix-live-h538/host-privileged.spindump`
with a five-second, 10ms, six-process targeted stackshot. Original report:
500 steps; appstoreagent CPU4.817s and syspolicyd CPU0.005s. Appstoreagent's
stacks repeatedly traverse `BiometricKitXPCClient initializeConnection`,
connection setup/interruption and local `objc_sync_enter` contention. This is
an independently observed background CPU load (~96% of one CPU), not dxmt9 CPU
or proof that appstoreagent owns the stalled launches.

The original text has only four process sections: amfid, appstoreagent, oahd
and syspolicyd. Neither target PID51619 nor62065 has a process stack section.
Reprocessing the retained data specifically for those two PIDs also yields no
process sections (`host-target-reparse.txt`, command receipt in
`host-target-reparse.log.json`). Both targets remain `U`/`Us` in a separate ps
check. Do not interpret their missing stacks as completed processes, successful
cleanup or a known kernel wait owner.

Oahd/amfid's displayed threads wait in signal handling and their idle workers
are omitted; syspolicyd mostly waits in run loops during this five-second
window. This does not join the earlier execution-policy errors to either target.
The confirmed service identity is `system/com.apple.security.syspolicy`.
A single-service restart may be used as an explicitly labeled recovery test,
not as a diagnosis. No service restart has yet been performed by the assistant.
