---
domain: present-pacing
workload: STALKER CoP capture29
title: "Stable evidence ownership removes the second prepared V4 admission"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-single-admission-22/verification.md; experiments/output/stkcop-v4-single-admission-22/{paired-l0-r1,l0-r1,l3-r1,l64-r1}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-admission-timing.438.md; specs/experiments/harness/replay/spec.md
---

# One admission with stable result ownership

The prepared V4 provider now retains one stack execution owner. Its General
replay helper mutates caller-owned result storage rather than copying it;
evidence pointers and reserved vector storage remain stable. A single pre-arm
claim binds exact plan, General plan, tape, evidence and policy owners. It
rejects failed, cancelled, progressed, consumed or mismatched owners without
effects. No input reparse, second admission, heap ownership transfer or generic
evidence relocation is introduced.

Cancellation remains at the original helper destruction tier. Explicit cleanup
still cancels before flush, resource retirement and observer closure. The
owner is fresh for every invocation; this does not qualify warm reuse or cache
mutable query identities, fences, cursors or actual execution evidence.

## Evidence

- The production claim predicate is exhaustively checked over 1,024 Boolean
  states and its consumed successor. Actual executor tests cover distinct
  same-digest owners, policy/state mismatches and stable evidence capacity.
- Native build and five focused tests pass; x86 build and six tests including
  install-name audit pass. General historical provider tests are included.
- Existing TLA inventory passes all 230 configurations. It complements the new
  native claim proof; it is not a newly added handoff model or a pixel oracle.
- Final-image paired qualification passes Compatibility and ProductionSlot:
  CPU logical streams retain 2,799 draws and 2,806 original items; two actual GPU
  executions per route match original pixels and EVENT readiness.
- L0/L3/L64 then pass 54 fresh-device actual-Metal repetitions, all with original
  pixels/EVENT readiness and zero GPU errors. Every row reports one admission
  and a consumed prepared handoff, independently of the timing fields.

## Measured scope

| Policy | Outer preparation ms | Inner admission ms | Replay wall ms |
|---|---:|---:|---:|
| L0 | 282.222 | 0 | 289.766 |
| L3 | 280.845 | 0 | 264.984 |
| L64 | 281.814 | 0 | 255.661 |

These are measured-only medians after two warm-ups, with 16 measurements per
policy. [H438](present-pacing-v4-admission-timing.438.md) observed a second
admission of about 69 ms. That call no longer executes; zero alone is not the
proof, the explicit count/consumption receipt is.

The retained execution owner is now destroyed after both admission/replay
timers. H438 included temporary/local owner destruction inside those scopes.
Consequently the replay-wall decrease is not an isolated full-provider speedup
or live-game FPS claim. Outer immutable plan/setup work remains around 282 ms
per iteration; caching those facts is separate from reusing mutable execution.

The paired oracle does not establish complete scalar/uniform semantic
equivalence, future backing preservation or numeric occlusion. Legal
NOOVERWRITE/closed writers remains a caller contract. Uncontrolled background
load/caches, warm-device A-B-A and broader live qualification remain open.
No renderer policy is promoted from these diagnostic results.
