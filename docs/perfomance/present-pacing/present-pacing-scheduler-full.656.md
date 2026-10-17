---
domain: present-pacing
workload: STALKER Day high late camera corridor
title: "H656 — baseline reverses worker ranking; full System Trace stop times out"
type: evidence
status: incomplete-population-scheduler-stop-failed
updated: 2026-10-17
source: traces/stkcop-scheduler-full-h656/{contract.md,inputs.json,derivation.json,analyze_cpu.py}; traces/stkcop-scheduler-full-h656/{O0,R0}/{receipt.json,analysis.json,self.bin,post-tail.png}; traces/stkcop-scheduler-full-h656/R0/{scheduler-receipt.json,scheduler-record.log,trace-stop.sample.txt,preserved.ktrace}
related: present-pacing-scheduler-live.655.md; present-pacing-scheduler-minimal.657.md; present-pacing-snapshot-performance.641.md
---

# H656 — do not transfer the diagnostic worker ranking to default

The frozen O0,R0,R1,O1 population stopped after R0's scheduler stop/save failure.
R1/O1 were not launched. O0 and R0 both reached all seven camera knots, exited
the game normally and restored all14 shared files. R0 also recovered all four
diagnostic clock-service files. Its cleanup receipt retains the sidecar error;
the population is not reported as successful.

| Arm | Frames | Replay CPU ms/frame | Encode CPU ms/frame | Observed ordering |
|---|---:|---:|---:|---|
| O0, original PE; trace/root/lifecycle OFF | 128 | 28.828–29.026 | 29.532–29.824 | Encode > Replay |
| R0, diagnostic PE; trace/root/lifecycle ON | 92 | 35.206–35.905 | 31.260–32.460 | Replay > Encode |

The same H649 Unix image, H639_SHARE=0, original engine/R2, seven spatial knots
and fixed workload settings are retained. H584's sparse self endpoints remain
in both arms. R0's PE/root upper is20.635ms/frame. These are CPU times, not
additive frame latency. The startup-to-startup gap is27805.835s because the
session resumed later; together with unfinished controls and a non-identical
command stream this prohibits treating O0→R0 as pure instrumentation overhead.

O0 is a counterexample to an unconditional "default Replay is always larger
than Encode" claim. It is not discarded because earlier H641/H650 references
ordered them differently. R0's failure cannot erase its independently written
CPU endpoints, but those endpoints cannot establish successful scheduler
coverage either. `analyze_cpu.py` retains H650's CPU/identity/image/restoration
checks, accepting only this exact recorded sidecar error for R0 and explicitly
labelling the result as CPU-only qualification.

## Trace failure evidence

This attempt used a full non-rolling System Trace, max25s, with16GiB prelaunch
headroom. All three native marker helpers completed. xctrace remained at
`Stopping recording...` until the180s save watchdog killed it. At the checked
wait point17GiB was free, unlike H655's disk-exhaustion failure.

The one-second xctrace sample shows its main thread in the NSRunLoop/mach_msg
wait and the `com.apple.dt.tap` queue waiting in DTXConnectionServices on a
semaphore. This localizes the observed wait, not its ultimate cause. Unified-log
inspection reported Processor Trace-service/connection errors but did not prove
a causal link. No current xctrace/Wine process remained after cleanup.

The6216398752-byte closed raw file is hard-linked to `R0/preserved.ktrace`;
SHA-256 is `19c4348a2599f328aed983312b8197aad8280fc24a31f76d9d04bb90470953bc`.
No raw import is substituted for missing calibrated full-window evidence.

## Next decision

Stop the broad-template game loop. H657 first checks the minimal installed
`Thread Activity` plus `os_signpost` instruments against actual native self CPU
and an external clock. Any later live attempt is a new population, not an R0
replacement. A shorter pre-corridor start may reduce recording volume, but its
readiness and complete knot/marker coverage must be demonstrated before use.

H641's snapshot removal result remains historical qualified evidence for its
own population, not a newly measured saving or current largest-owner proof.
The current default maximum, observer transfer and removable amount for that
maximum remain open.
