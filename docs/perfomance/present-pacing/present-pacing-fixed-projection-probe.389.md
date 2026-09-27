---
domain: present-pacing
workload: synthetic fixed-uniform predicate
title: "H389: Fixed projection survives isolated x86 optimization"
type: evidence
status: feasibility-only-production-validation-open
updated: 2026-09-27
source: traces/compat-uniform-h389/projection_probe.cpp; traces/compat-uniform-h389/projection_probe.s; traces/compat-uniform-h389/result.txt
related: present-pacing-uniform-lookup-owners.388.md
---

# H389: Is the temporary projection a real candidate?

H388 left open whether the compiler eliminates the fixed-payload projection.
An isolated x86_64 `-O3 -DNDEBUG` probe using the actual snapshot types shows
that it does not eliminate it in this call shape. **No production code changed.**

## Reference and candidate

Reference: `fixed == makeDrawUniformFixedPayload(full)`.
Candidate: compare the same twelve members directly, in declaration order,
using their existing equality operators and short-circuit behavior.
No byte comparison, hash-only acceptance, persistent witness or handle cache.

The noinline wrappers isolate these two expressions; they are not a claim
about every inlined production call site. The Apple Clang 21.0.0 assembly for
`projected` reserves 2,576 stack bytes and calls `memcpy` five times to build
the 2,568-byte fixed value before comparison. The `direct` wrapper does not
construct that value. A final production decision must inspect the actual
optimized lookup caller, not extrapolate solely from these wrappers.

## Bounded value checks

398 reference/candidate comparisons pass. The fixture independently varies a
representative scalar in each floating-point member across all pairs of
`+0`, `-0`, `+1`, `-1`, infinity and quiet NaN, plus baseline and clip-mask
mismatch. Thus NaN and signed-zero behavior are compared to existing equality,
not redefined by a byte comparison. This is representative-member coverage,
not exhaustive testing of every element or signaling-NaN exception behavior.

The candidate is a pure same-call value substitution. No temporal state,
resource lifetime, command ordering or interleaving is modified, so this probe
needs no new TLA transition. Production integration would still require native
lookup collision/identity tests and captured-input semantic verification.

## Isolated feasibility timing

One million equal-input calls per block, x86_64 under Rosetta, order
reference/candidate/candidate/reference:

| Wrapper | ns/call |
|---|---:|
| Reference, first | 445.61 |
| Candidate, forward | 377.74 |
| Candidate, reverse | 380.28 |
| Reference, last | 445.54 |

This synthetic hot-cache probe suggests about 65–68ns/call local headroom.
There is no declared warmup campaign, representative mismatch distribution,
or measured captured-replay gain. Multiplying by H388's 992 full-payload hits
would give only about 0.065–0.067ms/input **as a conditional scale estimate**,
not a measured saving or an upper bound. Fixed-component find's separate
~0.30ms scope already consumes a fixed projection; this change would not
remove its equality work or linear search.

## Decision

The projection-free predicate is a plausible small candidate, not a large
explanation of compatibility append cost. Next integrate one shared pure
predicate, preserve hash/identity guards, test collisions and actual lookup
behavior, and compare the same GT2 input with observer-OFF order reversal.
Inspect the production object's generated code. Do not add a runtime mode or
remove content validation. STALKER transfer and GPU/wild qualification remain
open; this probe is neither FPS evidence nor a promotion result.
