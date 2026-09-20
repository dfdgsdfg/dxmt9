---
domain: present-pacing
workload: STALKER CoP Day
title: "H343: VolumeTexture LockBox is the first STALKER capture rejection"
type: evidence
status: capture-blocker-attributed
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-tape-h343-r687-day/{result.json,dxmt9.log}; experiments/output/app-d3d9-stkcop-bench-tape-h343-r688-day/{result.json,dxmt9.log}; experiments/output/stalker-capture-diagnosis-h343/{dxmt9-h343-tests.log,dxmt9-h343-unix-tests.log,dxmt9-h343-pe32-final.log,dxmt9-h343-pe64-final.log,dxmt9-h343-native-final.log,dxmt9-h343-unix-final.log}
related: present-pacing-stalker-tape-admission.342.md
---

# H343: The first capture blocker is volume locking

Base `478f75d0` plus capture-failure-only diagnostics. R687 initially adds
bounded stack capture on the first generic child rejection. WoW64 returns
only the current module frame and an unusable low-address frame, so it does
not identify the caller. This stack implementation is removed.

R688 instead passes compile-time caller function/line literals through the
cold `AbortRenderTapeCaptureForChild` API and its two child-context facades.
Only the first registry-invalidating abort at Info logging emits them. No
COM, normal record, or bridge ABI signature changes; capture rejection and
application execution semantics remain unchanged. No allocation or stack
walk is added to normal execution. Line numbers here are run-local diagnostic
output, not durable source citations.

The actual R688 log is:

```text
render_tape_capture first_child_abort origin=LockBox line=3059
render_tape_capture registry_invalid reason=child_abort kind=0 generation=0 object_id=0 subresource=4294967295 format=0 width=0 height=0 pitch=0 bytes=0
```

This maps to `src/d3d9/d3d9_pe_device_child_surface.cpp`,
`D3D9VolumeTextureImpl::LockBox`: after `lockTextureBox` succeeds, enabled
capture tracking unconditionally calls `AbortRenderTapeCaptureForChild`.
`D3D9VolumeImpl::LockBox` has the same unsupported-capture policy, but is not
the first observed callsite. Resource identity/format/extents remain unknown
in this diagnostic; the callsite establishes volume-lock rejection without
establishing a particular texture or partial-box pattern.

## Qualification

Same Day/high 1280x720 Sikarugir lane and capture settings as H342 R686,
skip 1200, separate output roots `stalker-day-h343-r687` and `...-r688`.
R687 PE32 SHA-256 prefix `f585855e8a3ca523`; final R688 `c182bfeabd1c174b`.
Both are intentionally terminated after collecting startup rejection evidence,
before the screenshot deadline. Their runner results correctly report process
exit/missing capture failures. They are not successful wild runs, pixel
comparisons or FPS samples. No Tape is published and no replay is attempted.
Only the identified task-owned game processes are terminated.

All four final canonical builds pass; native Tape/publication/capture specs
pass 3/3 and x86 install-name audit passes 1/1. The runtime log verifies caller
literal forwarding through the real facade. No ordering, lifetime, query,
partition or completion transition is changed, so no new formal policy proof
is claimed or needed for this diagnostic-only change.

## Parallel eligibility audit: measurement remains open

The existing `parallel_pass_shadow_*` counters cannot answer the serial-lane
question: in `dxmt9_draw_encoder_chunk.mm`, `encodeChunkImpl` calls
`produceSealedParallelPassSnapshots` only when `parallelPartitionRequested`.
That predicate requires ExplicitParallel and no `sourceOcclusionScope`.
R686's zero attempts/candidates therefore mean observation was disabled, not
that STALKER has no parallel work. No unverified parallel policy is enabled
to obtain these counters.

H337 identified a costly unscoped prelude followed by many query-scope
closures. It gives a reason to inspect query-free work, but does not establish
sealed-pass eligibility, child balance or CPU-weighted parallel coverage.
A serial-only, pre-effect observer must use the existing pure producer on
diagnostic-owned scratch, report rejection/size/balance and source identity,
and never expose those snapshots to the executable parallel path. Only then
can cost be joined to eligible work without enabling parallel execution.

## Scope decision

The capture issue is not a stale flag to clear. Required volume support
includes descriptor/subresource shape, mip/depth row-and-slice layout,
initial-content closure and ordered partial mutations, followed by provider
volume creation/upload and pixel verification. Query provider support remains
a separate next grammar gate. Do not implement those larger migrations until
the serial-only eligibility observation establishes a worthwhile target,
unless complete STALKER replay support is independently desired.
