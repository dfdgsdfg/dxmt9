---
domain: present-pacing
workload: 3DMark05 GT2 fixed Render Tape compatibility route
title: "H388: Uniform lookup owners from existing counters"
type: evidence
status: diagnostic-attribution-stalker-transfer-open
updated: 2026-09-27
source: traces/compat-append-h387/uniform_analysis.py; traces/compat-append-h387/uniform-summary.json; traces/compat-append-h387/{off0,parent0,split0,split1,parent1,off1}/provider.stderr
related: present-pacing-compat-append-phases.387.md
---

# H388: Uniform lookup, component interning and storage

## Method

Re-analyze the six existing H387 blocks, without another runtime observer or
new run. `uniform_analysis.py` checks complete/reset-successful receipts, equal
input components, identical uniform work counts across blocks, and exactly one
counter summary per log. It records the source log hashes. Reproduce with
`scripts/run_python.sh traces/compat-append-h387/uniform_analysis.py`.

These counters are process totals over **18 iterations including two warmups**.
They cannot recover H387's sixteen-retained-sample means or percentiles. Divide
by 18 only; ranges below span six block averages. Existing `cpu_ms` counters
measure elapsed scopes, not exclusive native CPU service. Bucket lookup is
nested inside total lookup. Instrumentation cost remains included.

## Observations

All six blocks have the following work per complete input:

| Work | Count |
|---|---:|
| Draws / full uniform lookups | 1,866 |
| Last-handle hits | 919 |
| Bucket hits / misses | 73 / 874 |
| Bucket probes / different-hash collisions | 168 / 95 |
| Candidate hits / linear hits | 0 / 0 |
| Equal-hash collisions / semantic-hash misses | 0 / 0 |
| New uniform records | 874 |
| New fixed / vertex / pixel components | 184 / 870 / 259 |
| Full payload materializations / fallback materializations | 0 / 0 |

| Existing scope | ms per input, six-block range |
|---|---:|
| Full uniform lookup | **0.934–0.950** |
| ↳ Bucket lookup (nested) | 0.105–0.109 |
| Fixed-component find | **0.299–0.303** |
| Vertex / pixel component find | 0.032–0.032 / 0.037–0.039 |
| Fixed-component append | 0.037–0.038 |
| Vertex-component append | **0.178–0.185** |
| Pixel-component append | 0.023–0.027 |
| Uniform record reserve / copy / link | 0.028–0.029 / 0.031–0.032 / 0.030–0.030 |

This is not evidence of a hash collision storm, long bucket walks, or full
payload rematerialization. New uniform records frequently reuse fixed and pixel
components; 870 new vertex components accompany 874 new uniform records.
This establishes storage activity, not whether each new component is avoidable.

## Code ownership and limits

In `src/dxmt9/dxmt9_backend_types.hpp`:

- `ChunkSlot::findDrawUniformPayload` receives already-computed hashes. Even a
  last-handle hit resolves component handles and calls
  `drawUniformPayloadRecordMatches`: constant counts/bytes, fixed content and
  identity are validated. The non-bucket remainder also includes control flow
  and timer/counting overhead; it is **not an isolated equality cost**.
- `drawUniformFixedPayloadMatches`'s full-payload overload constructs
  `makeDrawUniformFixedPayload` for comparison. This is a source-level
  projection candidate, not proof that the optimizer emits a physical copy.
- `ChunkSlot::findDrawUniformFixedPayload` tries the last handle and then a
  linear scan (including that same record if reached). The 0.30ms find scope
  combines iteration, hash checks and content equality. Its existing counters
  do not establish scan length or how much the repeated last check costs.
- `appendDrawUniformPayloadWithFixedProjection` shares an already-derived
  fixed value between component lookup and append. Do not re-propose H12's
  already-implemented removal of duplicate extraction *inside this append*.
  Full-payload lookup before append remains a distinct path.

## Next experiment and safety boundary

Prioritize full-match fixed projection/comparison and fixed-component lookup,
then vertex storage. A bounded candidate can compare fixed fields directly
without constructing a temporary; inspect optimized code before claiming a
copy was removed. Another candidate is a call-local component witness shared
between full lookup and append, but it must remain tied to the same immutable
payload and destination identity. Neither candidate is implemented here.

Do not replace content validation with hash-only acceptance or blindly use
`memcmp`: fixed payload equality is fieldwise and floating-point NaN,
signed-zero and padding behavior must be preserved. Any substitution needs
forced-collision/identity/value tests, equal work counts and same-input CPU
semantic verification, followed by observer-OFF order-reversed measurement.
New component-level clocks would need their own overhead control; H387 already
showed roughly 0.525ms of additional split observation.

No production code, queue policy or lifetime contract changed. No new GPU
qualification or FPS claim is made. This remains GT2 fixed-input evidence;
STALKER late-frame attribution still requires an admitted equivalent input or
a separately qualified live diagnostic.
