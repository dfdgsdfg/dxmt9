---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE constant-flush gating and prepared section-count reuse"
type: evidence
status: native-codegen-only
updated: 2026-09-07
source: experiments/output/pe-flush-layout-20260907/analysis
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-identity-distribution.248.md
---

# PE constant-flush gating and prepared count reuse

## Scope and result

Base: `da1a4192`. R-CORE-REC-7.42–7.44 remove local repeated work; they do
not alter transport, chunk cadence, resource lifetime or queue policy.

- `D3D9DeviceImpl::flushConstShadow` in
  `src/d3d9/d3d9_pe_device_recorder.cpp` gates diagnostic dirty counting and
  shader hash lookup. Counting stays before append settlement; range reporting
  stays after successful append.
- `flushPendingConsts` uses the native-tested `dispatchPeConstFlushes` in
  `src/d3d9/d3d9_pe_const_shadow.hpp`. Clean families bypass the outlined call
  when decimation is off. Enabled decimation retains the six-call population,
  including empty calls. Each dirty check happens after preceding appends,
  preserving callback changes, first-failure order and exception propagation.
- `PeSemanticBatchOwner` in `src/d3d9/d3d9_pe_semantic_owner.hpp` reuses its
  existing sparse-layout quotas for four admission binding counts and seven
  canonical-emission mask counts. Source/callback, section shape, bounds,
  retention and rollback checks remain. No additional count cache is added.

## Evidence

All four canonical builds pass. Native PE/audit tests pass 29/29 and x86_64
focused tests pass 8/8. The unchanged TLA suite passes. Separate fresh-process
constant observer checks at N=1 and N=127 pass.

The new constant-flush test compares the actual production dispatch against
the old unconditional sequence over 4,032 traces: 64 dirty masks, seven failure
choices, three callback mutations and N=0/1/127. Each trace checks failure and
retry, accepted payload order, residual values/dirty state, and event/sample
populations. A separate throwing callback pins exception propagation. These
are fake-append local refinements, not native execution of the private PE COM
device or GPU pixels. Source contracts and linked branch inspection bind the
diagnostic gates and public dispatch call to the tested policy.

Compact tests compare canonical bytes for empty/sparse/full masks and a real
sparse/full/sparse A/B/A trace. Failed settlement releases speculative pins
before a successful retry of the same plan. Pin balance is checked after owner
destruction, not after emission while pins must still be held. A constrained
owner isolates sparse-capacity denial before callbacks or pin acquisition.

| Linked artifact | Baseline bytes | Candidate bytes | Compact writer static instructions, before → after | Software popcount expansions |
|---|---:|---:|---:|---:|
| PE32 | 1,869,824 | 1,870,336 | 514 → 394 | 6 → 0 |
| PE64 | 1,734,656 | 1,734,656 | 470 → 379 | 6 → 0 |

Counts include padding and cold branches, not executed instruction counts.
The seven source expressions include a cheaply lowered shader-mask count;
only six had the full software-popcount multiply sequence. The dispatch
template folds into the real caller's ordered checks/direct calls; it adds
no runtime family-switch layer. Guarded static initialization remains cold.
Exports and archive vtable/RTTI ownership match for both architectures. The
Unix provider and both bridge DLLs remain byte-identical to baseline.

Hash-qualified receipts, linked bodies and logs are in `source:` above:
`baseline-linked.json`, `verified-linked.json`, `*-linked.asm`,
`build-{native,unix,pe32,pe64}.log`, `native-tests.log`, `unix-tests.log`,
`observer-n{1,127}.log`, and `tla.log`. Final PE32/PE64 SHA-256 values are
`4af665c4dce352e0cc9313a705a5a11ba6e3c04e5eef1bca6e3eb88a6d17958f`
and `80beffb53579b446552ad5628d6dcc4844724a7b513140b8e0fea71e0e75c115`.

## Limits and next gate

No Wine staging or wild run was performed. No FPS, current bottleneck share,
pixel-oracle result or observer-off speedup is claimed. H248's workload sample
predates these changes; its event frequencies are prioritization evidence,
not saved CPU time. Next: matched observer-off GT2 correctness/locality and
throughput evidence with staged artifact hashes before attributing a benefit.
