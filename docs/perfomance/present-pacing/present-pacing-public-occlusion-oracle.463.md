---
domain: present-pacing
workload: Public PE D3D9 visibility and pixel oracle, Sikarugir CX24.0.7
title: "Public visibility results distinguish actual occlusion from conservative work"
type: evidence
status: bounded-public-pe-pass-edge-and-wild-qualification-open
updated: 2026-10-07
source: traces/stkcop-public-occlusion-h463/{run.py,edge-oracle.cpp,oracle.cpp}; traces/stkcop-public-occlusion-h463/{x86,x64}-{on,delay,off}-explicit; traces/stkcop-public-occlusion-h463/{x86-on,x86-on-r2,x86-on-overscan}
related: present-pacing-completed-workload-visibility.455.md; present-pacing-occlusion-stretch-oracle.461.md; present-pacing-default-thread-identity.462.md
---

# Real visibility suppresses an actually occluded result at the PE boundary

The new on-demand `d3d9_occlusion_x64.exe` target is compiled for both PE32
and PE64 (the basename follows the existing query target convention).
`occlusion_query_draw_pixels` calls only public D3D9 APIs. A 16x16 single-sample
target receives an unqueried depth/color predecessor, then three reissues of
one query: visible green, depth-rejected red, visible blue. Viewport-overscanning
geometry makes the analytic result exactly 256, 0, 256 covered samples.

The only progress operations before query completion are END, EndScene and
GetData(FLUSH); no Present, readback or subsequent draw drives completion.
Pending polls must leave the result sentinel unchanged, every poll preserves
adjacent bytes, and repeated completed polls return the same count. Only after
S_OK does the test read back every pixel, followed by separate 16x16-to-8x8
point/linear StretchRect and exact uniform-color readbacks.

## Executed controls

Apple M1 / Metal 4, Xcode 27.0 / Metal 32023.921, Sikarugir CX24.0.7,
Metal API validation ON, runtime performance counters OFF. All accepted runs
pass the ABI handshake; their staged-build sidecars identify the actual PE
and Unix binaries. These are correctness runs, not performance comparisons.

| Public lane | Real visibility | Completion delay | Counts | Verdict |
|---|---|---|---|---|
| x86 | ON | None | 256 / 0 / 256 | Pass, no skips |
| x64 | ON | None | 256 / 0 / 256 | Pass, no skips |
| x86 | ON | 200 ms | 256 / 0 / 256 | Pass, pending counts 325 / 235 / 223 |
| x64 | ON | 200 ms | 256 / 0 / 256 | Pass, pending counts 392 / 146 / 235 |
| x86 | OFF | None | 2 / 2 / 2 | Expected negative control: exactly three count failures |
| x64 | OFF | None | 2 / 2 / 2 | Expected negative control: exactly three count failures |

The OFF cases retain correct pixels but fail the sample-count checks, including
the completely hidden draw. Thus an application that uses zero to suppress
subsequent work receives materially different information. This supports the
mechanism behind H455, not an FPS or removable-CPU quantity by itself.

The newly built Unix provider is SHA-256
`6dcd5d761b29711c4a2764080eaf72bb5c272197a3616bf3b6df1f36de208152`.
PE binaries were rebuilt through their canonical Meson targets after an old
postprocess stamp rejected the initial run. They are not asserted byte-identical
to H455/H462. Every accepted run uses explicit `--exe-dir`, `--pe-dll-dir`,
`--pe-arch` and `--winemetal-so`. The wrapper backs up and atomically restores
all five shared Wine artifacts, with SHA-256 receipts. The restored Unix image
is H462's `7f45561ddf0fd22f477e3d3447000913aac83c5b61950726fbe17cffc88aece5`.

## Retained failures and exclusions

- The first `x86-on` attempt is a staging rejection, not a renderer result.
- `x86-on-r2` and `x86-on-overscan` inadvertently execute the x64 auxiliary:
  the runner's `--exe-dir` default is a fixed x64 directory, despite help
  describing it as following `--exe`. Their claimed x86 test hash is not a
  loaded-test identity. They are excluded from the x86 matrix. The corrected
  wrapper supplies the directory explicitly for every `*-explicit` run.
- That initial PE64 edge geometry (`edge-oracle.cpp`, -0.5 to 15.5 XYZRHW)
  returns 225 / 0 / 225 and fails pixel checks. The final oversized geometry
  isolates visibility from edge positioning; it does NOT fix or qualify the
  edge discrepancy. `src/dxmt9/dxmt9_ffp_shaders.cpp`'s pretransformed branch
  maps XYZRHW without the half-pixel adjustment used by its transformed branch.
  This is a concrete follow-up candidate, not a proven root cause or Wine
  oracle verdict. Edge behavior remains a correctness qualification gap.
- This public fixture covers 1x UP draws, not public MSAA/indexed/shader-draw,
  partial coverage, overlapping queries or device loss. H459/H461 provide
  different, native coverage; do not silently combine them into full public
  or whole-scene correctness.
- Uniform pixels prove preservation, not point/linear filter weight quality.
  No new production ordering/lifetime transition was introduced, so no new
  TLA result is claimed. Existing lifecycle models do not prove these pixels.

## Decision for the CPU objective

H462's close dedicated-worker costs still do not bound default Primary's
renderer component. H455's 4.60–6.11 ms/frame Encode difference remains a
visibility-associated candidate, not a validated removable budget. H463 closes
one concrete public-boundary gap needed to qualify that candidate, and shows
why changing query results can reduce *incoming work* rather than merely speed
up identical commands. It neither establishes the largest default dxmt9
single-thread cost nor closes the original objective.

Keep the provider default OFF. Resolve or qualify the retained edge failure
before broad promotion, then use one matched same-build workload comparison
to price the candidate. Do not repeat H462's successful thread identity census.

[H464](present-pacing-occlusion-edge-isolation.464.md) subsequently reproduces
the edge failure on both real and conservative dxmt9, while the same public
input passes WineD3D. It isolates a shared raster gap, not a new occlusion
regression, and does not claim that the gap is fixed.
