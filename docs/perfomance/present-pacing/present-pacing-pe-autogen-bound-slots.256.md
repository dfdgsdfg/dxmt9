---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Immutable bound-AUTOGEN slot elision"
type: evidence
status: correctness-verified-performance-open
updated: 2026-09-07
source: experiments/output/pe-autogen-bound-slots-20260907/analysis; experiments/output/conformance-20260907-122714; experiments/output/conformance-20260907-122759; experiments/output/conformance-20260907-122548
related: present-pacing-pe-full-selfpc.255.md; ../../../specs/d3d9/recorder/spec.md
---

# Immutable bound-AUTOGEN slot elision

## Decision and bounded contract

At `8f6b1653`, [H255](present-pacing-pe-full-selfpc.255.md) retains 14/9
non-crossing PE samples in `generateBoundAutogenMipmaps` for 05/06 GT2.
These are stopped-PC counts, not exclusive CPU time or a savings estimate.
Each draw initializes three 20-entry candidate arrays and scans all texture
slots, invoking wrapper RTTI for non-null bindings before checking immutable
AUTOGEN eligibility and mutable Dirty.

R-CORE-REC-7.60 adds a four-byte `BoundAutogenTextureSlots` projection of
immutable creation metadata, piggybacked on existing membership validation.
Normal SetTexture and StateBlock Apply publish it with the Live binding;
Recording and failed setters do not. Release/reset clears it before releasing
bindings. Unexpected Apply revalidation failure clears the affected bit and
retains the existing traversal-then-terminal policy. Dirty, resource pointers,
wire identity, generation and completion are not cached in the projection.

An empty mask bypasses arrays and RTTI. Nonempty masks visit ascending slots,
read current Dirty, deduplicate raw identities by first occurrence and use the
existing bound wire identity. Record acceptance still precedes Clean. No
bridge ABI, queue, Metal code, cadence or runtime knob changes.

## Verification and limitations

Four canonical builds pass. Native focused checks pass 8/8, including the new
bounded refinement; the unchanged TLA suite passes in 151.61 seconds. The
x86_64 AUTOGEN, core mip-generation and install-name checks pass 3/3.

The new native test consumes the real mask and existing mip-state reducer:
12 transition cases, 72 mask laws, 212,992 state checks, 1,795 draw checks,
four explicit cases and two detected mutants. The three-slot/four-resource
domain exhausts 64 Live x 64 snapshot x 4 Dirty bases; draws cover failure
at each source slot, accepted-prefix settlement and terminal suppression.
The dense oracle derives eligibility directly from Live state, not the tested
mask. All 20 physical positions are separately pinned. This is bounded value
refinement, not a generated binding of private COM callbacks or fault injection
into those callbacks; it does not prove arbitrary thread/driver execution.

`resource_autogen_stateblock_apply_latest_mip` runs real D3D9 under Sikarugir
on both PE architectures. A 64x64 AUTOGEN texture sampled into an 8x8 target
produces exact red, then green after a level-zero update without rebind, yellow
while a StateBlock is only Recording, and blue after null binding followed by
Apply. No explicit GenerateMipSubLevels call can hide a missing draw-time
generation. Final builtin resources suites pass 3/3 with zero skips on each
architecture. This fixture covers 2D hidden mips, not every cube/volume format
or application scene; app-local evidence remains open.

A deliberately built PE32 mutant omits only successful Apply republication.
Its first three pixels pass; the fourth is stale green `0xff00ff00` instead
of blue `0xff0000ff`. The auxiliary runner conservatively reports all three
cases as fail although stdout identifies one pixel failure. Its process
verdict, not the runner's zero exit status, is the negative oracle. The mutant
is frozen separately, removed from source and not the final staged image.
The restored image passes. Initial fixture runs skipped because AdapterFormat
was incorrectly A8R8G8B8; querying the actual display format corrected that
test defect. Those skips are not qualification evidence.

## Linked cost and provenance

Both linked images branch on an empty mask before candidate-zero stores and
the `memset`/RTTI calls. Function prologue and stack reservation remain; this
does not make the call free. Total outlined code grows 716 to 747 bytes in
PE32 and 775 to 792 in PE64, despite less work on the empty path. Exports,
vtable/RTTI ownership, build options and all 12 manifest hot-entry metrics are
unchanged. Those 12 do not include SetTexture, which gains publication work.
DLL file sizes remain 1,905,664/1,771,520 bytes. Device layout grows eight
bytes in PE32 to 105,544 including alignment; PE64 remains 106,680 by using
padding. The layout pins explicitly record this cost.

Final PE32 SHA-256:
`a41f8e48dda2c6110e61d4bb7f33f5cce191e91a32bfbc9a1a5c08197c1feb15`.
Final PE64 SHA-256:
`2d04bf90e744ce045628af626bec117e409d3b6acecb040995f9dbc163ee52a9`.
`candidate-linked-final.json`, frozen images and disassembly identify them.
Provider and both bridges are byte-identical to H255. Earlier test-build
receipts and the mutant are not the final measurement hashes.

Actual sampler-OFF 05/06 GT2 checks and separate full PE profiles are still
required. No CPU reduction, FPS improvement or README recovery is claimed
from static work removal and correctness tests alone.
