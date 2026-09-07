---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Immutable bound-AUTOGEN slot elision"
type: evidence
status: correctness-verified-no-demonstrated-fps-gain
updated: 2026-09-07
source: experiments/output/pe-autogen-bound-slots-20260907/analysis; experiments/output/conformance-20260907-122714; experiments/output/conformance-20260907-122759; experiments/output/conformance-20260907-122548; experiments/output/app-d3d9-3dmark{05,06}-pe-autogen-c9bb5c56-gt2-{perf,sampler}-r1-20260907
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

## Sampler-OFF performance spot checks

Committed `c9bb5c56` has one sequential perf run per product on Sikarugir
cx-24.0.7: 05 GT2 at 1024x768, 06 GT2 at 1280x720. Frame sampling is ON;
PE sampler/full report/module map, detailed timers, phase split and copy
ledger are OFF. No GPU trace, compilation or test suite runs alongside them.
A read-only process CPU/RSS observer polls every two seconds, so these are
not completely observer-free measurements. All five staging receipts match
the final candidate/unchanged bridge hashes above. The index-cache selection
is OFF, matching H255; the historical README configuration is not a fresh
matched baseline.

| Product | Scene FPS | H255 spot FPS | README FPS | Delta to README | Rows / wall ms | CB / frame | Pass / frame |
|---|---:|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 29.121 | 29.316 | 30.1 | -3.25% | 1,883 / 64,662.305 | 4.001 | 15.788 |
| 06 GT2 | 17.428 | 18.065 | 18.1 | -3.71% | 1,118 / 64,149.489 | 4.002 | 19.860 |

05 includes every positive-wall row, frames 1–1,883. 06 includes positive-wall
rows with more than 100 draws, frames 966–2,083, excluding loading but not
slow scene frames. H255 deltas are -0.67%/-3.53%, not an attributed code
regression or a neutrality result. Nix automatic GC and filesystem activity
were observed before measurement; the initial GC exited before the OFF runs,
but Finder returned to approximately 99–100% CPU during 06. A retained host
snapshot and later renewed GC activity make this a contended 06 spot check.
Those processes were not stopped or reconfigured. Do not explain all of the
delta by contention without a controlled comparison, or call it a code win.

Both catalogue results pass with zero retained chunk rejections, GPU errors,
pipeline build failures and no-pipeline skips. 05 exits normally; 06's
permitted timeout closes its result GUI after scene completion. Captures at
0:59.93 / 0:30.54 show coherent forest frames, not whole-scene equivalence.
Draw counts are 1,681.264/1,886.004 per frame and summed GPU CB times are
1.997/11.970 ms per frame; summed CB time is not GPU critical-path latency.

`perf-measurements.json` preserves partial, approximate frame/CPU joins:
05 has 1,802 frame increments, 128.01 process CPU seconds over 60.275 elapsed
seconds (71.038 CPU ms/frame, 2.124 average cores, RSS peak 3,466,188 KiB).
06 has 1,096 increments, 109.67 CPU seconds over 62.275 elapsed seconds
(100.064 CPU ms/frame, 1.761 cores, RSS peak 3,538,072 KiB). These include all
process threads and unequal partial intervals, not PE-exclusive or matched
baseline costs. They do not demonstrate lower CPU consumption.

The local static removal and pixel/refinement proofs stand. FPS recovery,
CPU savings and noise-qualified neutrality remain open; README is unchanged.

## Separate complete PE profiles

The same committed binaries have one diagnostic run per product with H255's
250-Hz PE sampler, module map and FULL_SELFPC enabled, Info logging and frame
sampling. Detailed timers, phase split and the copy ledger stay OFF. Their
FPS is not performance evidence. Startup GC/file activity was observed before
05, and Finder/automatic Nix activity persisted through the 06 diagnostic;
sample shares are not controlled exclusive CPU timing.

| Product | Last reported Present | Thread samples | PE samples | Buckets | Omitted / overflow | Unambiguous generation-function samples |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 1,800 | 13,605 | 1,334 | 612 | 0 / 0 | 1 |
| 06 GT2 | 1,380 | 14,252 | 1,015 | 537 | 0 / 0 | 0 |

Every full group validates (30/23 groups), with zero suspend/context/resume
failures and passing self-module probe containment. Symbol joins use the
frozen final PE32 image. Both diagnostic catalogue results pass with zero
retained renderer failure counters and matching five-artifact receipts; 06's
permitted timeout closes the completed scene's GUI. Cross-symbol ambiguity leaves 355/291 samples
unassigned; lexical family totals are not subsystem timings. The approximate
06 scene-prefix subtraction starts from report 360 before first heavy frame
371, retaining 1,012 PE/13,453 thread samples through report 1,380 in 535
buckets. The last unreported tail is absent, and this is not H255's exact
frame population. Different loading Present counts are not lost histogram
samples; conservation is checked within each run.

The outlined `generateBoundAutogenMipmaps` has 1/0 non-crossing samples versus
H255's 14/9; `D3D9PeGetDirtyAutogenTexture` has none in the assigned samples.
This is consistent with eliminating known-ineligible candidate work. Small
counts, symbol layout and host load prohibit a CPU-ms saving estimate or a
claim that the function is never called. The remaining profile still exposes
constant shadow/copy and semantic-owner admission work; its correctness and
ownership checks cannot be removed solely because they receive samples.
