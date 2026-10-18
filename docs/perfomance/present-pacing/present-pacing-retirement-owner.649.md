---
domain: present-pacing
workload: private worker-retirement observer integration
title: "H649 — owner-qualified terminal observation without an early join"
type: evidence
status: native-and-capture-qualified-live-attribution-open
updated: 2026-10-17
source: traces/stkcop-retirement-owner-h649/{contract.md,retirement.hpp,parse.py,bounds.py,envelope.py,envelope-derivation.json,preparation.json,qualified-build.json,verification.json}; traces/stkcop-retirement-owner-h649/native/verification.json; traces/stkcop-retirement-owner-h649/oracle/runs.json
related: present-pacing-thread-retirement.648.md; present-pacing-owner-composition.647.md; present-pacing-owner-lifecycle-live.646.md
---

# H649 — a terminal observation, not a synthetic successful join

H648's native mechanism now connects to H643 worker creation and payload
retirement in coherent private binaries. H584's already-requested sparse CPU
samples trigger bounded terminal sweeps **after** their CPU sample. Polls that
do not emit a sample perform no new sweep. No product joins, worker topology,
resource semantics or GPU policy change.

Each worker registers its exact `mach_thread_self` right and native TID before
H643 Enter. A noncopyable process-local token retains that right without
releasing, rebinding, importing or recycling it. Only successful `mach_port_type`
returning exactly DEAD_NAME, after the same owner's payload-retired event,
authorizes a terminal AFTER timestamp. Failed queries and missing tokens remain
unknown. SEND means port present, not proven execution capability.

The registry has no persistent-state import or reset API. Exclusive trace-file
creation rejects reinitialization across exec; corpse execution cannot emit a
later observation. This explicitly scopes H648's kernel-port-reset exception:
ordinary dxmt9 Device reset is not privileged host exec/corpsify. The pinned XNU
source is explanatory reference, not asserted to match the installed kernel.
Native controls qualify behavior on the actual ARM64/Rosetta host.

## Qualification

| Evidence | Result |
|---|---|
| ARM64/Rosetta OFF/ON | Four processes pass; each calls/destroys three payloads and TLS objects exactly once |
| Pure terminal-admission predicate | 128 combinations per process; only retained, continuous, retired, successful dead-name authority admits |
| Terminal before join | Six observed workers; body/callable/TLS held-open controls never close early |
| Corrupt wire evidence | 14 mutations rejected per observed architecture |
| Reinitialization | Both architectures reject an existing trace with exit79; original bytes unchanged |
| Coherent build | Seven designated object replacements; every other archive object preserved; install-name audit passes |
| Original capture O0/I0/N0/O1 | All four preserve original source/output pixels, query diagnostic and resource lifetime |
| Capture registration | 22 observed worker instances registered and parsed; all original joins remain intact |

Each capture creates/releases670 resources and maps7 source boundaries. Original
source/output hash is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Query issues9/polls5/results4/readiness5 agree, but this remains a production-
counter diagnostic, **not a real GPU occlusion oracle**. The capture exercises
registration and sweeps; the native fixtures exercise natural termination before
join, including archive roles. Neither substitutes for live archive attribution.

Source inversion recovers the entire original H643/H640 headers and all seven
source TUs. The H642 envelope arithmetic is unchanged; only the terminal field
and accepted authority tags are generalized. The receipt records
`retained_thread_port_dead`, never manufactures `successful_join`, preserves
all unknown time through observation, and excludes time only after its upper
endpoint. Six actual native tails remain fully bounded before that endpoint.

## Preserved build-check failure

The first archive comparison stopped because it counted `__.SYMDEF` symbol-index
metadata as an unexpected changed object. All seven compilations had completed.
The corrected finalizer excludes only recognized symbol-index metadata, checks
every changed object against its compiled bytes and verifies the exact intended
object set. The failed `build.json` is preserved; `qualified-build.json` is the
authoritative completed receipt. No unfavorable runtime arm was replaced.

## Remaining scope

No live CPU saving or default promotion is claimed. H650 freezes four original-
game arms O0/R0/R1/O1 to close archive retirement and test observer transfer,
without appending to H641 or H646. Conditional-worker policy, complete PE/root
coverage and foreign platform remainder still require explicit audit before a
global largest-owner claim.
