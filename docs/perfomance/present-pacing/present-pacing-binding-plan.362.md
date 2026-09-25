---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture; STALKER CoP Day
title: "H362: Diagnostic gating and command-local binding projection"
type: evidence
status: partial-attribution
outdated: knob-removed
updated: 2026-09-25
source: experiments/output/binding-plan-h362/override-comparison.json; experiments/output/binding-plan-h362/{override-off,override-on}/result.json; experiments/output/app-d3d9-stkcop-bench-planning-h362/{result.json,daybenchmark.result,scene.png}; traces/app-d3d9-stkcop-planning-h362/planning.json
related: present-pacing-serial-encode-owners.361.md
---

# H362: Execution proof before reuse claims

Historical OFF/ON experiment: the selector is retired by
[H363](present-pacing-binding-plan-default.363.md). Its artifacts remain valid
historical evidence, but the OFF policy cannot be selected in current runtime.

Base `ed88a207` plus the uncommitted implementation. Three changes:

- Draw `PerfScope` skips clocks with perf disabled; binding-cache statistics
  are constructed/timed only with perf enabled. Disabled encoder-breakdown
  texture recording no longer performs resource lookups.
- A command-local owner builds sampler projections once and updates
  stream/raster fields in place per draw. This bypasses full packet hashing,
  comparison and copying for eligible serial draws. Resource objects, concrete
  backing snapshots, uniforms, lifetime and pass scheduling are not cached.
- An opt-in source-level trace partitions framegraph planning into observation,
  graph construction, lookahead, natural-frontier work, optimizer,
  diagnostics/prefix repair and replay selection. No per-draw clocks are added.

The reuse candidate remains default-off. See the
[contract](../../../specs/backend/encode-scheduling/binding-plan-reuse.md).

## Fixed-input GPU qualification

Same x86 provider, loaded images, input fingerprints, captured source plan
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`,
identity partition and cold-device reset. Each policy runs two warmups and four
measured iterations. All 12 iterations pass source/output exact readback with
SHA-256 `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
GPU errors are zero. `check_command_binding_plan_pair.py` verifies artifacts,
policy/image/input parity, nondegenerate pixels and nonzero candidate reuse.

| Policy | Builds (all 6 iterations) | Reuses | Encode elapsed median |
|---|---:|---:|---:|
| OFF | 0 | 0 | 40.355 ms |
| ON | 3,876 | 7,320 | 39.310 ms |

This is about 2.6% lower elapsed encode scope in one short ON→OFF comparison,
not native thread CPU, an A/A-controlled result, or an FPS improvement. Longer
order-reversed repetitions and observer-OFF wild qualification remain open.

The first two pairs under `binding-plan-h362/{off,on,final-off,final-on}`
are deliberately not accepted evidence: pixels matched but reuse was zero.
Snapshots initially excluded every draw; after removing that unrelated
exclusion, per-draw stream overrides still prevented reuse. The final candidate
uses the production `applyDrawBindingOverride` preservation contract: it changes
stream/index fields and vertex declaration streams, not sampler inputs or pixel
shader metadata. Native differential tests invoke that actual transition and
compare against full canonical packet preparation, including UP, index mode,
stream frequency, stride, offset, generation, LOD, viewport and scissor changes.

## STALKER planning split

One Day/high/1280x720 Sikarugir diagnostic run passes with zero GPU errors.
Unix image `ef68b87c2b1cf035`, PE32 `4d30258d6e7d0c3e`, PE64
`cae268981dc6cc85`. Whole-run builds/reuses are 1,232,562 / 6,513,830.
The benchmark reports 21.272827 average FPS, but this is an ON-only diagnostic
run, not matched performance evidence. The automatic `actual.png` is loading;
the separately saved `scene.png` shows the running scene. This single image
does not prove continuous visual or exact pixel correctness.

The Present 2100–2120 trace is closed, with zero overflow/incomplete writers,
zero aborted/clipped planning sources and 58 complete planning scopes.
Total planning native CPU is 8.254ms: graph construction 6.223ms, optimizer
1.201ms, replay selection 0.593ms; other child phases total 0.237ms.
Largest source 5121 has 356 commands and 0.408ms planning CPU, including
0.308ms graph construction and 0.073ms optimizer. The enclosing old
planning/setup marker is 0.411ms, so wrapper residual is small in this sample.

**H361's 40–45ms spikes did not reproduce.** Its example had 814 commands;
this run's top five planning sources have 343–357. Equal Present ordinals do
not mean equal source streams or scene populations. Do not attribute the
missing spikes to sampler reuse, diagnostic removal, or a repaired optimizer.
The next attribution gate is capturing/replaying the actual large source with
these phase endpoints, then comparing matched OFF/ON work.

## Build and test evidence

Native and x86 full builds pass, including Unix install-name fixup. Both PE
builds pass; ABI unchanged. Focused native tests pass, as do x86 binding and
install-name tests. Existing TLA regression passes (210.83 seconds); it is a
regression guard, not a new proof of pixels. Sequential reuse refinement is
bound to the production C++ transform and GPU exact comparison above.

Initial builds hit toolchain mixing: Nix linker could not read Xcode 27 SDK
`arm64e.x1` stubs; Apple `ar` then rejected Meson's cached LLVM `csrD` flags.
Explicit Xcode compiler/linker paths plus LLVM `ar` resolved this without source
or SDK workarounds. All qualification uses the final same-image pair.

The repository-wide perf-doc source audit still reports unrelated pre-existing
missing archived logs in capacity-ownership recovery, GPU-position invariance
and query-default promotion. No missing historical evidence was fabricated.
