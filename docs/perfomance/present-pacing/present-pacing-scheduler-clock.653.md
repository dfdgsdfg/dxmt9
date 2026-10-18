---
domain: present-pacing
workload: native ARM64 and Rosetta attached-process scheduler clock controls
title: "H653 — attach-mode scheduler intervals share a bounded native clock mapping"
type: evidence
status: native-attach-clock-qualified-live-coverage-open
updated: 2026-10-17
source: traces/stkcop-scheduler-clock-h653/{probe.cpp,run.py,analyze.py,analysis.json}; traces/stkcop-scheduler-clock-h653/{arm64,x86_64}/{receipt.json,target.jsonl,thread-state.xml,thread-info.xml,os-signpost.xml,os-signpost-arg.xml,context-switch.xml,toc.xml,scheduler.trace}; traces/stkcop-scheduler-preflight-h652/analyze.py
related: present-pacing-scheduler-preflight.652.md; present-pacing-retirement-live.650.md; present-pacing-snapshot-performance.641.md
---

# H653 — qualify the target-window clock before attributing native threads

System Trace successfully attaches to already-running ARM64 and Rosetta
fixtures. Three bracketed signposts, emitted on two target threads, admit a
common native-to-trace offset. This permits conservative target-window selection
without relying on rounded trace start dates. Both fixtures exit naturally.
No game, product binary, Wine installation or frozen H641/H650 population changes.

## Mapping and actual controls

For exported event time `t` and its native emission bracket `[before, after]`,
the offset lies in `[before - t, after - t]`. Intersect all three brackets;
reject disjoint intervals or wrong process/thread/sequence identity. A native
window `[a, b]` maps outward to `[a - offset_high, b - offset_low]`.
This qualifies the observed short fixture, not arbitrary clock drift or sleep.

| Observation | ARM64 | Rosetta x86_64 |
|---|---:|---:|
| Common offset aperture | 1.708µs | 9.958µs |
| Target native threads in both exported inventories | 3 | 4 |
| Busy-body self CPU | 300.151000ms | 300.136000ms |
| Mapped-window Running | 299.155484ms | 298.762238ms |
| Mapped-window Running + Interrupted + Unknown | 300.151583ms | 300.146792ms |
| Unknown within busy-body outer window | 0 | 0 |

The self endpoints have the previously qualified 2µs allowance each. The
conservative scheduler amount contains the actual self CPU in both controls,
even without spending that allowance. Running alone again undercounts CPU.
These differences are not estimates of observer overhead or application savings.

An idle thread exists before attachment and waits until after the final
signpost. Its selected-window history starts Blocked and has no Running or
Interrupted time. Its whole-body self interval starts before tracing, so that
CPU delta is deliberately **not** compared with the selected trace window.
Rosetta's additional exception-server thread is included, not silently dropped.

The busy thread is created after attachment. Its history contains Unknown
before the first known state; the broader signpost corridor retains
115.625µs/204.999µs of this uncertainty. No birth time is invented to remove it.
Thread histories are contiguous and include their exported Terminated suffixes.
The separate empty ThreadTermination table is not used as a census.

## Parser and qualification

The signpost export includes two table projections of each event. Six rows
represent three events per architecture. Deduplication requires the same
resolved event object and matching fields; equal-looking independent events
are not merged. XML references are type-checked and missing/cyclic references
reject the result. Target PID/TID values must agree across event fields,
thread-info and thread-state; names are not identities.

Sixteen negative cases reject wrong thread/process, disabled/out-of-order
anchors, disjoint clock offsets, dangling references, independent duplicate
events and uncovered windows. Another 576 finite cases check outward window
containment for every enumerated offset. Capture input hashes are verified;
the report pins exported artifacts and all files in both retained trace bundles.

## Remaining global gate

This is not Wine coverage, a proof of lossless scheduling export, a general
XNU state-semantics proof, or an observer-OFF transfer. In particular, attach
success and internally consistent inventories do not prove that every native
thread or event was captured in a larger workload. Missing histories, loss
indications and Unknown intervals must remain explicit in the live analysis.

Next connect clock anchors to an original-game diagnostic episode and compose
the scheduler envelope with H650-style PE/root and worker identities. Preserve
the same camera corridor and distinguish native game/Wine/platform CPU from
dxmt9 ownership. Run tracing separately from untraced performance comparisons;
do not apply a constant tracing-overhead subtraction.

H650 still ranks Replay above the observed composed owners, not all possible
default-path owners. H641 remains the separate positive snapshot-removal
evidence. Neither global largest-owner attribution nor FPS gain is newly proved.
