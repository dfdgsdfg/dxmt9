---
domain: present-pacing
workload: Sikarugir x86 standalone ABI/marker prototype, not STALKER
title: "Clockless outer-call marker: low-cost prototype and conservative reducer"
type: evidence
status: prototype-qualified-not-installed-no-workload-attribution
updated: 2026-10-07
source: traces/stkcop-clockless-h457/{marker.hpp,probe.cpp,run_probe.py,stress.cpp,read_stress.c,run_stress.py,bound.py,test_bound.py,generate_tables.py,tables.json,tables.generated.cpp,tables.o,audit.py,audit.json}; traces/stkcop-clockless-h457/{release,fenced,serializing,serializing-r2}/{receipt.json,marker.hpp,probe.cpp,stdout.txt,stderr.txt}; traces/stkcop-clockless-h457/{stress-r2,stress-negative}/{receipt.json,marker.hpp,stress.cpp,read_stress.c,stdout.txt,stderr.txt}; traces/stkcop-clockless-h457/serializing-r2/disassembly.txt
related: present-pacing-api-residency.453.md; present-pacing-completed-workload-visibility.455.md
---

# A lower-cost route to Primary attribution, not an attribution result

**Follow-up:** [H458](present-pacing-primary-interposer-rejection.458.md) rejects
the cloned-vtable integration on actual optimized objects. The synthetic
results below remain calibration evidence, not permission to deploy that
observer into STALKER.

H455 leaves the largest default dxmt9 single-thread owner unresolved because
Primary includes game, Wine, PE and synchronous Unix work. H453's per-call QPC
and locked increments perturb the workload and omit original function edges.
H457 tests a replacement diagnostic mechanism without changing renderer source,
Wine staging, runtime defaults or the three canonical x86 runtime image hashes.

## Executed prototype

A single-writer aligned 32-bit generation is odd while an outer original call
executes, even outside. Shared TLS depth suppresses nested transitions. There
are no hot-path clocks or locked increments. Exhaustion latches permanently odd
and a fault, never reusing an even value. Registration is cold.

A COM-shaped test clones the complete five-slot synthetic table and preserves
its RTTI prefix. Typed stdcall wrappers enclose an untouched indirect original
call and never inspect the object after Release. Actual Sikarugir execution
passes integer/64-bit/floating argument and result checks, nested dispatch,
dynamic_cast/object identity and a Release which destroys the object.

Plain release publication is rejected as an occupancy certificate: a reader
could see stale even while the original executes. The final candidate emits
`MFENCE; LFENCE` after odd publication and before the original call, with
compiler barriers. Cross-process atomicity and translated-fence/native-reader
ordering remain explicit platform assumptions, not C++-only proofs.

Twelve alternating-order synthetic comparisons perform one million calls per
arm. Final r2 adds 7.320–7.418 ns/call elapsed over the minimal direct original.
Earlier release/MFENCE/serializing variants add approximately 8.0–8.5 ns/call.
These tightly coupled empty-call trials neither price the complete interposer
nor bound workload perturbation. Do not subtract them from a STALKER budget.

A separate native ARM shared-map reader observes a Wine/Rosetta x86 writer:

| Final protocol stress | Result |
|---|---:|
| Reads | 410,600,000 |
| Stable even / stable odd | 188,923,374 / 161,817,748 |
| Changing generation | 59,858,878 |
| Even certificate while independent body witness is active | 0 |
| Deliberately omitted entry publication, detected contradictions | 1,022,373 |

These are correlated stress observations, not independent trials or a maximum
visibility-delay bound. The negative control verifies detection, not exhaustive
protocol correctness.

## Full-table compile gate

The installed mingw SDK supplies types for 15 concrete dxmt9 COM tables and
320 method thunks. Each field offset and whole-table size is statically checked;
all compile on x86. Base/Ex/IUnknown exposure must retain the concrete table,
not truncate to the requested IID's public prefix.

Runtime installation is **not implemented**. Six shader-validator C slots need
a separate adapter without an RTTI prefix; twelve public exports need outer
wrappers. Returned-object closure must be method-specific: for example,
CheckResourceResidency's Resource** is an input array, while GetContainer may
first expose a most-derived object through IUnknown. Immutable table clones
must outlive cached zero-reference resurrection and reentrant Release. Mapping
reload epochs and full export/output closure remain deployment prerequisites.

## Conservative reducer

The offline reducer retains a predeclared stratified sample population. An
outside certificate needs identical valid even generations and identities,
ordered loads bracketing the planned instant, no faults, and qualified
publication. Late, missing, duplicate, malformed or epoch-uncertain rows count
as renderer-possible; they are not discarded or redrawn.

For duration weights `w_i`, possible indicators `Y_i`, and predeclared failure
probability `delta`, the conditional one-sided bound is:

```text
q_upper = min(1, sum(w_i * Y_i) + sqrt(log(1/delta) * sum(w_i^2) / 2))
renderer_CPU_upper = min(native_Primary_CPU_upper, window_wall * q_upper)
```

It is not `q_upper * Primary_CPU`. Native CPU snapshots must enclose the whole
window; frame normalization requires a separately qualified positive lower
denominator. Six tests cover changed/nested/odd/exhausted generations, missing
and duplicate samples, scheduling/identity failures, unequal strata, unqualified
publication, and CPU units/endpoint containment.

The statistical statement assumes independently uniform planned instants and
a trajectory not materially changed by the sampling schedule. The reducer
cannot establish those premises from rows alone, detect hidden stale-even
publication, or grant complete ingress coverage. No workload observations have
been fed to it. Multiple chosen windows need a confidence-budget policy.

## Decision

The ABI, synthetic overhead and cross-architecture gates justify implementing
the complete diagnostic boundary and scheduled native reader. They do not
establish a Primary/Encode ordering in STALKER. A subsequent same-camera
diagnostic/default perturbation check is still required before transferring a
diagnostic-run bound to the default path. H455's visibility-associated savings
and H456's limited GPU safety qualification remain separate evidence.

## H458 integration checkpoint, not a workload result

The subsequent local prototype in `traces/stkcop-primary-bound-h458/` implements
the full concrete-table interposer, validator adapter and export wrappers. Its
x86 D3D9 DLL builds, and read-only review covers output closure, readable
current-image RTTI guards, cached zero-reference objects, reentrant Release,
DLL-local epochs and overlapping-image invalidation. These are implementation
checks, not actual-object or Wine execution results. The scheduled native reader
and workload qualification are still missing.

The diagnostic image is archived as `images/d3d9.dll` under that directory;
the temporary entry-source overlay is removed and the canonical x86 D3D9 build
output is restored from its hash-checked baseline. Shared Wine staging remains
baseline. No H458 STALKER run, CPU bound, policy promotion or performance claim
is made. The ignored prototype is local experimental evidence, not committed
production instrumentation.

## Next decision

Prioritize qualification of H455's real-occlusion candidate over extending
general Primary attribution: it already has repeated late Encode CPU effects.
Close actual MSAA counts/resolved pixels, public PE query/lifecycle and fault
coverage, then qualify scene behavior and observer-OFF performance. Compare
query-driven visibility against an appropriate reference; do not require query
counts to equal the primitive-count approximation being replaced.

Stop promotion on correctness, lifetime or progress failures. If qualification
passes but the default-path experiment shows no repeatable benefit, record
that result without claiming H455's diagnostic delta as an FPS improvement.
Return to H458 only when its remaining attribution question would change the
implementation decision. Neither this prioritization nor the bounded stencil
oracle establishes the largest default-path single-thread owner.
