---
domain: present-pacing
workload: STALKER CoP Day
title: "H342: STALKER fixed-input parallel experiment blocked at capture admission"
type: evidence
status: blocked-at-capture
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-tape-h342-r684-day/{result.json,dxmt9.log}; experiments/output/app-d3d9-stkcop-bench-tape-h342-r685-day/{result.json,dxmt9.log}; experiments/output/app-d3d9-stkcop-bench-tape-h342-r686-day/{result.json,dxmt9.log,actual.png}
related: present-pacing-stalker-draw-phases.341.md
---

# H342: Capture admission precedes parallel performance

The requested ladder is capture-to-serial pixel correctness, serial-to-parallel
correctness with non-vacuous worker execution, then repeated fixed-input timing.
This attempt stops at the first prerequisite: no STALKER Tape was published.
No serial/parallel pixel result or performance improvement is claimed.

## Capture attempts

HEAD `478f75d0`, Day/high, 1280x720, Sikarugir cx24.0.7, perf profile.
All attempts use `DXMT9_RENDER_TAPE_CAPTURE=1`,
`DXMT9_RENDER_IDENTITY_MODE=segment`, and a distinct drive-qualified
`DXMT9_RENDER_TAPE_OUTPUT_ROOT` below
`Z:\Users\dididi\workspaces\dxmt9\experiments\render-tapes`.
The roots are `stalker-day-h342-r684`, `stalker-day-h342-r685`, and
`stalker-day-h342-r686`, respectively. Runs use the catalogue runner with
`--capture-delay-sec 160 --timeout 480`; no direct Wine launch is used.

| Attempt | Skip Presents | Log level | Observation |
|---|---:|---|---|
| R684 | 2200 | perf default warn | Runner PASS, 1973 submitted Presents; selected boundary not reached; no bundle |
| R685 | 1200 | perf default warn | 1927 submitted Presents; no bundle; info-only capture rejection hidden |
| R686 | 1200 | explicit info | 1893 submitted Presents; pre-arm registry invalidation, followed by repeated arm rejection |

R684's staged Unix SHA-256 prefix is `ec3516aea75327c1`; the five artifacts
are recorded in each runner receipt. The native build reports no work. Xcode
27.0, Metal compiler 32023.921, Apple M1 are available. No runtime code or
partition policy is changed. Captures are not performance runs: capture
tracking/seed/storage behavior differs from ordinary production execution.
Successful app execution does not imply successful Tape capture.
All three runners PASS without timeout or reported capture error, and their
five staged artifact hashes match. R686's inspected screenshot shows the game,
not desktop fallback; it is not a replay pixel oracle. No workload process
remains running. Documentation source audit and `git diff --check` pass.

## First observed blocker

R686 records, before arming:

```text
render_tape_capture registry_invalid reason=child_abort kind=0 generation=0 object_id=0 subresource=4294967295 format=0 width=0 height=0 pitch=0 bytes=0
```

After the selected boundary it records `arm enabled=1 producer=0 publisher=1`,
then `arm aborted reason=present_output_admission` and the corresponding
`first_abort`. In `d3d9_pe_device_tape.cpp`,
`armRenderTapeCaptureAtPresentBoundaryInterval` rejects when either output
admission fails or the registry is already invalid. Therefore the later
message alone does not establish a backbuffer admission defect; the earlier
registry invalidation is the first observed failure.

`AbortRenderTapeCaptureForChild` in `d3d9_pe_device_tape_child.cpp` records the
generic `child_abort` without a caller/resource identity. Multiple callsites
use it, including volume LockBox, invalid snapshot inputs, and object capture
failures. This log does not identify which callsite executed. Volume handling
is a candidate, not an established cause. Do not clear the invalid flag,
silently drop a resource, or change the output oracle to bypass this failure.

## Separate provider limitations confirmed by code inspection

`src/d3d9/device_c_render_tape_provider.cpp` has additional prerequisites:

- `buildGeneralPlan` rejects Query object definitions; its accepted definition
  families are texture, surface, buffer, shader and vertex declaration.
- `generalRecordTypeSupported` does not accept `QUERY_ISSUE`.
- General `OrderedControl` admission accepts GammaRampSet only, not QueryGetData.
- `render_tape_runtime::createObject` has no Query construction branch.
- General texture admission/construction supports 2D/cube, not volume textures.

These are static grammar gaps, not a provider rejection observed on this
attempt: no bundle exists to run through the provider. Query completion and
recorded application decisions require an explicit replay contract before
accepting these events. The parallel pass classifier independently excludes
query-bearing passes; implementing query replay would not authorize parallel
query execution.

## Next gate

First add capture-only caller/resource-qualified rejection evidence and repair
the identified capture ownership/seed gap. Then validate the actual emitted
grammar and close required provider families, preserving query result/order
and lifetime semantics. Only a source-oracle-correct bundle may proceed to
`parallel-verify`; only non-vacuous correct parallel execution may proceed to
warm-up/repeat/order-reversed timing. Do not substitute a query-free projection
and label it as the complete STALKER late-hitch workload.
