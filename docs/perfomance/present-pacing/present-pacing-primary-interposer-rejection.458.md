---
domain: present-pacing
workload: Sikarugir x86 actual D3D9 object fixture, not STALKER
title: "Reject cloned-vtable Primary instrumentation: final-class identity mismatch"
type: evidence
status: rejected-observer-semantic-change
updated: 2026-10-07
source: traces/stkcop-primary-bound-h458/{objects.cpp,run_objects.py,images/d3d9.dll,baseline/d3d9.dll}; traces/stkcop-primary-bound-h458/{texture-baseline-r5,texture-interposer-r6}/{receipt.json,stdout.txt,stderr.txt,objects.cpp,run_objects.py}
related: present-pacing-clockless-primary-prototype.457.md; present-pacing-completed-workload-visibility.455.md
---

# Actual-object execution rejects the cloned-vtable observer

H457's synthetic ABI fixture and H458's full-table build did not establish
semantic transparency in the optimized production DLL. H458's actual-object
fixture finds a counterexample before a STALKER launch: a texture created by
the same device is rejected by `SetTexture` only with the interposer.

| Same fixture executable | D3D9 image | Texture membership outcome |
|---|---|---|
| Baseline r5 | `534ea6f17128354ea350b7d6000c750e82bfcc3556d827464ebf02e69e8f02d0` | PASS, process exit 0 |
| Interposer r6 | `7fbda1fe19dde57e6444aaef00290748ccf50ac4e637bfd63ae35cba64b0e17c` | `D3DERR_INVALIDCALL`, process exit 2 |

Both receipts identify executable SHA-256
`d9ca2c7c830f22cef6cefa9bddb4a7432c284952d7a8481d8e044756314d2827`.
The fixture performs three DLL load/export/unload cycles, creates an Ex device,
exercises base/IUnknown aliases and backbuffer/container/swapchain aliases,
releases/reacquires the cached backbuffer, then creates and binds a texture.
The diagnostic passes the reload bookkeeping checks and reaches texture
binding without a coverage fault. A zero coverage-fault flag therefore does
not qualify semantic transparency.

## Mechanism verified in the emitted image

`src/d3d9/d3d9_pe_device_child_surface.cpp`, `D3D9PeValidateTexture`, performs
`dynamic_cast` to concrete `final` texture classes. Clang lowers these to exact
original-vtable-address comparisons. In the archived diagnostic image, the
function at `0x10061e60` loads the object's vptr and compares it with
`0x1014d4bc`, `0x1014d51c`, and `0x1014d57c`. Symbol inspection identifies these
as the original 2D/cube/volume texture vtable address points. A mismatch returns
`0x8876086c` without reading the RTTI prefix.

H458 replaces that address with a clone in its own array. Preserving table
extent, calling conventions and both Itanium prefix words cannot preserve this
optimized identity test. Independent read-only review confirms the machine-code
finding and retracts the earlier assumption that prefix preservation sufficed.

## Experimental limits and restoration

Initial r1/r2 attempts lacked the harness-owned Wine surface-protocol declaration
and failed device creation. R3 supplies the manifest's matching declaration and
exposes the texture failure. The broader baseline r4 fixture progresses beyond
texture binding but later rejects StateBlock creation after a chunk-admission
failure; that unrelated sequence is not a passing all-family fixture. R5/r6
reduce the test to the distinguishing texture sequence. No full 15-family,
negative-foreign-object or workload qualification is claimed.

Each runner restores the exact shared Wine baseline in `finally` and records
its SHA-256. Canonical build D3D9 and shared Wine D3D9 hashes both remain the
baseline hash above; no temporary entry-source overlay remains. Diagnostic
images and failed runs are retained as ignored local evidence.

## Decision and effect on the CPU objective

Reject the cloned-vptr observer for actual workload attribution. Do not remove
`final`, rewrite validation, or disable optimization to make the observer pass:
that would change the production work being attributed. No STALKER run, CPU
ranking, removable-cost bound or performance improvement follows from H458.

This closes an invalid measurement route, not the overall objective. H455 still
supplies repeated visibility-associated tail Encode CPU effects, while mixed
Primary CPU does not rank PE/synchronous Unix against Encode. Continue with the
real-occlusion correctness and observer-OFF effect gates described in H457's
next decision. Any future Primary observer must preserve original vptr identity
and pass an optimized actual-object counterexample before workload deployment.
