---
domain: present-pacing
workload: STALKER CoP capture29
title: "Complete prepared-provider timing and immutable V4 preparation boundary"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-provider-call-23/verification.md; experiments/output/stkcop-v4-provider-call-23/{l0-r1,l3-r1,l64-r1}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-single-admission.439.md; specs/experiments/harness/replay/spec.md
---

# Complete provider-call baseline

The diagnostic now measures the complete `replayPreparedRenderTapeIdentity`
result-assignment expression from its caller. This includes the outer V4
plan/admission and provider-local destruction excluded by internal replay wall.
It excludes input/options preparation, factory/device creation, subsequent
device teardown and independent caller qualification. Result storage retained
by the caller is destroyed later. No runtime hot-path timer or policy changed.

The strict optional provider-call receipt exposes qualified per-execution
elapsed time only. Missing legacy fields remain accepted, unobserved values
remain null, and malformed present fields or repeat-root aggregation reject.
Native five and x86 six focused tests pass, including the 86-case capture CLI
suite and x86 install-name audit. No new state transition requires a new formal
model; the prior slice's TLA result is not presented as a new run.

## Same-input results

| Policy | Full provider call ms | Outer plan/admission ms | Replay wall ms | Inclusive encodeChunk ms |
|---|---:|---:|---:|---:|
| L0 | 569.419 | 279.467 | 289.566 | 10.426 |
| L3 | 543.624 | 280.143 | 262.711 | 22.273 |
| L64 | 537.538 | 279.954 | 256.429 | 10.605 |

Each median uses 16 measurements after two warm-ups, in L0/L3/L64 order. All 54
actual-Metal executions preserve original pixels and EVENT readiness with zero
GPU errors. Each reports one admission and a consumed prepared handoff. Input,
plan, executable and loaded-image identities match across the three sets.

These are elapsed, overlapping service scopes, not additive thread CPU or FPS.
Background load and driver-cache/order effects remain uncontrolled. There is no
older full-call measurement to establish a before/after speedup. L3 again has
more inclusive encode time despite lower provider replay wall; one scope must
not stand in for the other.

## Next ownership boundary

Roughly 280 ms per invocation remains in immutable V4 planning/admission/setup
validation. The next implementation should retain an eager, factory-validated
V4 bundle inside the stable move-only prepared-input PIMPL. It can retain owned
source/setup plans and event/command/query templates, but must instantiate
fresh mutable query identities, local record facts, cursor, Present phase and
actual completion evidence. Existing decoded sidecar views already own their
rows; an extra raw-sidecar copy is not required solely for lifetime.

The contract is now specified; cross-iteration V4 derived-plan reuse is not yet
implemented. Preparation authority must not be a caller-set bool, a digest-only
cache or a previously armed executor. Full-call timing provides a stable scope
for that comparison; moving work out of replay wall alone is not a speedup.
Warm-device, complete scalar/uniform semantics, future backing preservation
and numeric occlusion remain unqualified. No runtime policy is promoted.
