---
domain: present-pacing
workload: 3DMark05 GT2 r405 original Render Tape
title: "H396: Deferred ingress OFF/ON GPU exact qualification"
type: evidence
status: captured-input-gpu-qualified-wild-open
updated: 2026-09-27
source: traces/ingress-h396/{run.py,audit.py,summary.json}; traces/ingress-h396/{compatibility,production-slot}-ingress-{0,1}/result.json
related: present-pacing-ingress-handoff-validation.395.md; present-pacing-fixed-projection-validation.391.md
---

# H396: The candidate executes and preserves this captured frame exactly

H395 committed as `c7146c40`; runtime candidate is `d37935e5`. Toolchain doctor
passes: Xcode/xctrace 27, Metal compiler 32023.921, Apple M1/Metal 4. Use the
existing production-provider GPU readback harness, not a gputrace/timing run.

## Method

The immutable original GT2 input retained by H391 is used unchanged. Run
Compatibility OFF/ON, then ProductionSlot OFF/ON, each in a fresh provider
process with two cold-device iterations, zero warmups, identity partitioning,
zero headroom and authenticated capture-source boundaries. Input source plan:
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.

Inherited renderer/Metal knobs are removed. Both CPU-ready Tape and commit
offload are explicitly zero, so CPU-ready routing cannot accidentally select
capture-only admission in the reference run. The only OFF/ON environment
difference is `DXMT9_DEFER_INGRESS_RESOURCE_MARKS`. The provider deliberately
runs synchronously: this is an execution/pixel oracle, not a worker-overlap test.

`runProviderReplay` calls the production
`persistResolvedResourcesAndCaptureBindings` before source-policy replay.
Thus this harness reaches the changed admission branch even though it uses
prevalidated provider inputs rather than the Wine bridge queue. Existing perf
counters provide a separate mechanism signature below.

## Results

| Route | Ingress deferred | Iterations accepted | Direct / compatibility draws per iteration | Ticket recheck count, whole process |
|---|---|---|---|---|
| Compatibility | OFF | 2/2 | 0 / 1,866 | 474 |
| Compatibility | ON | 2/2 | 0 / 1,866 | 372 |
| ProductionSlot | OFF | 2/2 | 1,856 / 10 | 110 |
| ProductionSlot | ON | 2/2 | 1,856 / 10 | 8 |

All eight iterations have zero GPU errors and strict source/output oracle
matches. Independent audit rehashes all 16 source/output RGBA files, each
3,145,728 bytes, matching original SHA-256:

`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`

Within each route, the complete path-counter objects match across both modes
and iterations. ProductionSlot's existing SoA-growth count is 21 in both modes;
this experiment does not eliminate or newly introduce it.

Input components, provider/validator fingerprints, all loaded image hashes and
normalized environments match across the four runs. Loaded Unix provider:
`72d52840b59df1b1fa528b35f9531b4b401e9128fc051c38fa4aeeb7f9e3c9a5`;
native dylib:
`522a241f8ddd27f95f4ed0dd8fe24cfefdbf771399fdca1808acebc15b38c547`.

Ticket **rechecks**, not actual re-stamps, drop by 102 per two-iteration process
on both routes; re-stamp fires remain zero. This confirms a real branch change,
not two identical policy executions hidden behind an ineffective environment
setting. Provider capture-source replay re-marks local ranges independently:
its aggregate decrease is not the expected mark-count decrease for ordinary
asynchronous application replay.

## Scope and remaining gates

This closes fresh exact GPU evidence for this captured input and these two
routes. It does not prove arbitrary texture/surface mutation sequences, query
or Reset races, all partial-effect failures or asynchronous worker progress.
H395 provides separate bounded worker/backing evidence, not a substitute for
that remaining composition audit.

No FPS, timing benefit or STALKER visual claim is made. Keep the runtime default
OFF. Next is completion of the remaining family/reset coverage and a supervised
STALKER OFF/ON correctness run, followed by low-interference timing. No README
performance value changes.
