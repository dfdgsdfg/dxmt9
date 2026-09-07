---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Public constant range authority removes duplicate Live guards"
type: evidence
status: implementation-verified-runtime-pending
updated: 2026-09-08
source: experiments/output/pe-validated-constant-range-20260908/analysis
related: present-pacing-pe-settlement-refinement.259.md; ../../../specs/d3d9/recorder/spec.md
---

# Public constant range authority removes duplicate Live guards

## Mechanism and scope

H259's linked PE32/PE64 bodies retain null/empty/extent checks both in the
public constant setter and its outlined fixed-width Live helper. The current
stopped-PC profiles locate both bodies, but do not price these guard blocks.

R-CORE-REC-7.62 introduces `ValidatedConstWrite<ElemSize>::withCheckedRange`
in `src/d3d9/d3d9_pe_const_shadow.hpp`. A synchronous visitor issues a
noncopyable, private-constructed range only for nonempty, non-null input whose
64-bit end fits the family cap. The range binds source/start/end/width; the
private Live kernel consumes those facts without another range check. No
heap-owned token, persistent shadow field, raw unchecked public entry, wire,
transport, queue or Metal change is introduced.

All six public setters retain poison-first and diagnostic routing.
`D3D9DeviceImpl::applyConstStateWriteValidated` retains the existing
`applyConstStateWriteImpl` write-permission/Recording envelope. Empty calls
still allow arbitrary start and null input, but do not bypass phase permission.
Recording still copies through `recordConstStateWrite`. The generic checked
fixed-width entry still validates unqualified inputs. Growth, zero extension,
changed-only bitwise copy and dirty membership are unchanged.

## Bounded verification

`dxmt9-pe-chunk-record-value-spec` exercises 55 factory calls per family,
330 total: a complete small start/count domain, full-cap and last-register
boundaries, overflow, nonempty null, unaligned source, A/A/B/A, zero extension,
exact self-alias and warm storage reuse. The actual factory and Live kernel
are checked against the independent prior transition's values/extent/dirty
effects. Invalid and empty callbacks must execute once without Live effects.
Compile-time pins reject raw/default/copy/move construction and assignment.

Deliberately changing `end > cap` to `end >= cap`, or restricting empty writes
to `start == 0`, makes the native target fail. Both mutants were restored
before final builds. Full canonical builds pass; restored native focused
12/12 and x86_64 focused 6/6 pass, including source-route, negative ABI-audit
and Unix install-name checks. Logs retain the two negative controls and the
final positive runs. The route audit now checks each of the six setter bodies
independently; a later setter cannot satisfy an earlier missing route marker.
That audit is source ownership evidence, not a semantic proof.

This is a local value refinement, not a changed thread or lifecycle protocol.
Existing phase-model/native tests retain their original scope. The new native
factory test does not invoke the private PE adapter through a poisoned or
Terminal transaction. No new concurrency or whole-scene pixel proof is claimed.
Ordinary C++ references can still escape if a future callback is miswritten;
the production synchronous call sites, not noncopyability alone, close that
borrow lifetime.

## Linked economics and final identity

An initial outlined visitor materialized three closures per public call. The
retained `outlined-prototype-*` images rejected that code shape. The factory
and public adapter now explicitly inline, while the Live kernel remains
outlined. Neither PE image emits a factory/witness symbol or adds a setter
call: all six setters retain 15 static direct calls across all branches.

The manifest's raw F-setter body grows 5 bytes/one instruction on PE32 and
shrinks 7 bytes/three instructions on PE64. Symbol-to-next-symbol extents in
the separate audit include padding and must not replace those raw metrics.
The two validated kernels remove 16/11 decoded instructions on PE32 and
17/15 on PE64. Kernel stack reservations are unchanged except PE64 width 4,
which drops 16 bytes. Observed checked wrappers grow, retaining validation
for their unqualified callers. There is no universal code-size reduction:
the DLL grows 2,048 bytes on PE32 and 1,024 on PE64. Existing device/recorder
layout pins, exports, vtable/RTTI owners and build options remain equal.

Final restored PE32 SHA-256:
`23c2b44524fded748e6926f7c8f2b9ece3fed255b056a2209208bd58c7147f29`;
PE64: `0eb59521085213c1a82e9d48d13f696d89e6a9d1184762ac68029d95467b1ba3`.
`qualify_linked.py` confirms that each differs from its independently audited
first link only at four COFF/debug timestamp bytes. All other image bytes
and raw metrics are identical. The Unix provider and both bridge DLLs retain
H259's exact hashes. `candidate-linked-validated-audit.txt`,
`candidate-linked-validated-symbols.txt` and `final-linked-qualified.json`
retain the disassembly, timestamp and metadata evidence.

## Remaining measurement gate

Actual final-image Wine tests pass five cases per PE width: vertex constants,
pixel shader get/set, pixel constants, constant Apply and cross-stage
StateBlock. New public checks cover null/empty/arbitrary-start, the last valid
register and one-past-cap for all six families, plus invalid/empty VS-F writes
while Recording. The existing recorded-value checks remain intact. All ten
cases have zero failures, skips, timeouts or missing verdicts; every staging
sidecar matches the final images. This is getter/StateBlock evidence, not a
draw or full poisoned-adapter matrix.

Separate sampler-OFF 05/06 GT2 CPU/FPS spots plus complete PE profiles remain
pending.
H259's qualified references are 29.682/18.220 FPS and approximate partial
all-thread CPU of 70.153/95.472 ms/frame. Its original interfered 06 perf r1
is excluded. Historical README 30.1/18.1 also differs in the index-reorder
setting; it is context, not a matched A/B. Guard removal alone establishes
neither reduced CPU nor recovered FPS.
