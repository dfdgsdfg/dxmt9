---
domain: present-pacing
workload: STALKER CoP capture29
title: "Repeated V4 admission is expensive but does not own the grouping gap"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-phase-timing-21/verification.md; experiments/output/stkcop-v4-phase-timing-21/{l0-r1,l3-r1,l64-r1}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-query-timing.437.md; docs/perfomance/present-pacing/present-pacing-v4-inprocess-repetitions.436.md; specs/experiments/harness/replay/spec.md
---

# Admission, pre-arm checkpoint and finish

Prepared-only clocks now distinguish the outer V4 source-plan/admission/setup
validation from the second admission inside replay, the pre-arm queue checkpoint
call and execution finish/qualification. Historical unprepared calls receive
no new clock reads. No admission was removed, no lifetime was reused and no
queue/query ordering changed. Versioned diagnostic export rejects malformed
present measurements and leaves unqualified observations null.

One final image executes capture29 L0/L3/L64 with two warm-ups and 16 measured
iterations each. All 54 executions preserve original pixels and EVENT readiness
with zero GPU errors; executable, loaded images, plan and input identities match.

| Policy | Outer preparation ms | Inner admission ms | Pre-arm checkpoint ms | Finish ms | Replay wall ms |
|---|---:|---:|---:|---:|---:|
| L0 | 281.955 | 68.609 | 1.394 | 0.543 | 358.344 |
| L3 | 281.295 | 68.722 | 1.414 | 0.938 | 334.943 |
| L64 | 281.269 | 68.660 | 1.352 | 0.555 | 327.009 |

Values are measured-only elapsed medians, not thread CPU or an additive
partition. Outer preparation is outside replay wall and includes destruction
of its temporary admission owner. Inner phases are within replay wall; the
checkpoint clock excludes the subsequent lease lifetime and finish excludes
the following cleanup.

Two conclusions are distinct:

- Repeated cold admission is a substantial harness iteration cost. Shared
  input preparation has not yet made all immutable V4 derived work reusable.
- This work is nearly policy-invariant. It does not explain the 23--31 ms
  replay-wall gap, nor do the measured checkpoint/finish differences.

The useful next implementation is single-admission ownership, preserving the
authenticated plan and stable result-evidence binding. The executor retains an
evidence pointer while replay currently receives a by-value result; a borrowed
executor cannot simply be passed across that boundary. Later immutable plan
reuse must keep local query identities/epochs, cursors, fences and actual
qualification per execution. Removing a duplicate validation must not remove
the original pixel/query/failure gates or become a claimed live-game speedup.

Native and x86 builds pass; native focused tests pass 4/4 and x86 tests including
install-name audit pass 5/5. No new TLA run is claimed for timing-only scopes.
Background load, uncontrolled caches, warm-device closure, numeric occlusion,
exclusive encode attribution and live qualification remain open. No renderer
policy is promoted.
