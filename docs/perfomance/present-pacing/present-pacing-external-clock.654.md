---
domain: present-pacing
workload: external ARM64 clock marker and separate Rosetta scheduler fixture
title: "H654 — an external marker binds the target scheduler window without a target hook"
type: evidence
status: external-clock-qualified-live-application-open
updated: 2026-10-17
source: traces/stkcop-external-clock-h654/{marker.cpp,run.py,analyze.py,stream_states.py,verify.py,analysis.json,verification.json}; traces/stkcop-external-clock-h654/run0/{receipt.json,record.log}; traces/stkcop-external-clock-h654/run1/{receipt.json,target.jsonl,marker.jsonl,thread-state.xml,thread-info.xml,os-signpost.xml,scheduler.trace}
related: present-pacing-scheduler-clock.653.md; present-pacing-retirement-live.650.md
---

# H654 — no new marker is needed inside the game/provider

All-process System Trace records a separate ARM64 marker alongside the frozen
H653 Rosetta fixture. Their independently derived clock-offset intervals
intersect. Using **only the external marker's offset**, the mapped busy window
contains the target's actual self CPU. This enables scheduler observation with
the existing H649/H650 binaries rather than rebuilding the provider for logging.

| Observation | Result |
|---|---:|
| External offset aperture | 2.000µs |
| Target-local cross-check aperture | 5.417µs |
| Separate marker/target PIDs | 67335 / 67332 |
| Target threads in both inventories | 4 |
| Target busy self CPU | 300.117000ms |
| External-mapped possible CPU | 300.118249ms |
| Unknown within that window | 0 |

Possible CPU includes Running, Interrupted and Unknown. Running alone is
299.600112ms and remains an undercount. Duplicate signpost table projections
are handled with H653's exact event-object checks. This is native fixture
qualification, not a game timing, default-owner result or overhead correction.

## Preserved failures and resource control

The first recording completed its five-second collection but exceeded the
runner's60s watchdog while saving. `run0` remains a failed attempt; its fixture
endpoint output was not recovered by that runner. It is not silently reused as
a successful timing population. The corrected runner records timeout commands,
allows180s for finalization/export and recovers fixture output even on failure.
`run1` records/exports successfully and both fixtures exit normally.

The system-wide thread-state export is172MiB. An initial full-DOM filtering
analysis reached about2.5GiB resident memory and was explicitly interrupted;
it produced no accepted result. Its repeated list removal was also quadratic.
The replacement decoder streams rows, retains typed reference values and only
stores the target PID's intervals. It does not mistake kernel Idle intervals
for target CPU or discard references defined on another process's row.

Verification compares the streaming decoder exactly with the prior decoder
on four ARM64/Rosetta exports, rejects six malformed streams, and authenticates
470 inputs. The verification process peaks at386,236,416 bytes RSS; its actual
system-wide streaming decode takes8.66s. These are tooling-resource observations,
not game performance. No parser runs inside a live measurement corridor.

## Application boundary

The next original-game episode keeps PE/Unix images and game code unchanged,
adds external markers around the existing camera corridor, and records a bounded
rolling trace. Both marker groups and the selected CPU interval must survive
trimming. Missing histories, disjoint offsets, loss indications or uncovered
windows reject attribution. System-wide collection can perturb scheduling;
its numbers cannot replace H641's untraced removal result or an FPS baseline.

Thread-inventory agreement is not a general proof of lossless export. Global
default dxmt9 ownership and observer-OFF transfer remain open until actual-game
coverage is inspected and composed with the same-episode root/worker evidence.
