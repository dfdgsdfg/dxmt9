---
domain: mini-replay-bisection
workload: 3DMark05 GT1 / GT2
title: "Uniform invalidation: concrete clip pixels pass, wild FPS promotion withheld"
type: experiment
status: accepted-verdict
updated: 2026-09-16
source: experiments/output/uniform-invalidation-qualification-20260916-r632.noindex/wild-campaign.json; experiments/output/uniform-invalidation-qualification-20260916-r632.noindex/wild-audit.json; experiments/output/uniform-invalidation-qualification-20260916-r632.noindex/native-final.log; experiments/output/uniform-invalidation-qualification-20260916-r632.noindex/x86-final.log
related: docs/perfomance/mini-replay-bisection/uniform-dependency-reuse.15.md; specs/backend/gap.md
---

# Uniform invalidation qualification

## Verdict

The known-RenderState dependency candidate committed as `8975ef47` passes
the new concrete cached FFP/clipping pixel oracle on arm64 and x86_64. Eight
same-image 3DMark05 wild runs complete without recorded rendering errors.
They do **not** establish an FPS improvement or a noise-qualified
non-regression: GT1 mean changes −0.08%, GT2 −0.57%, with a 4.2% first-to-last
OFF drift in GT2. `DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION` stays default-off.
No runtime code, cache policy or default is changed by this qualification.

## Concrete GPU gate

`tests/native/backend/uniform_invalidation_metal_spec.cpp` uses a persistent
bound VB and actual `Device::submitDirectReplayDrawFromCurrentState`, not
DrawPrimitiveUP or a fake/suppressed backend. It renders 30 64×64 BGRA8 frames:

- FFP → programmable VS/PS → FFP, with an x/y-swapping view matrix.
- Irrelevant `RS_ALPHA_REF` changes while alpha testing is disabled.
- Clipping enable and plane mask on → off → on.
- Positive/negative clip plane and StateBlock restoration.

FFP world-space clipping must retain the right half; programmable clip-space
clipping must retain the top half. Four known-color interior probes per frame
make stale masks, wrong coordinate space and non-rendering fallbacks visible.
The wrapper compares **all active pixel bytes**, excluding row padding,
between separate OFF/ON processes. Both architectures produce the same
pixel transcript SHA-256:
`ef83939770ce9b6e6728bd564b5fa8207f38a0113683d8d61df552ad3d955fd7`.

Path-execution checks require 30 Metal draw calls, no GPU errors, rejected
chunks, missing pipelines or library failures. Full builds change **30→21**
and full-payload reuse **0→9**, binding the pixel result to the changed cache
branch. A machine without a Metal device returns Meson skip (77), never a
successful pixel receipt. The fixture uses the binding-agnostic cache with
ordinary queue submission; it does not certify Direct Slot lease placement.
The prior fixed-input tape GPU/CPU evidence covers that complementary path.

Final native tests pass **4/4**; x86_64 tests plus install-name audit pass
**5/5**. The new test adds concrete evidence to R-BACK-2.15.5, not a new
temporal protocol. Shared-predicate exhaustive refinement and actual
setter/cache checks remain the formal/code-binding layer from the prior leaf.

The first fixture draft used an insufficient programmable input declaration
and failed in the OFF reference. The final fixture explicitly supplies
float4 POSITION plus D3DCOLOR for the programmable mode and XYZW FVF for FFP.
The failed logs remain preserved; no production change was made to accommodate
the fixture.

## Wild method and identity

Apple M1, Sikarugir `sikarugir-cx-24.0.7`, 3DMark05 1024×768, no AA.
Each scene runs **OFF → ON → ON → OFF**, serially, with no concurrent build
or native test. The `perf` profile, asynchronous commit replay, frame sampling
and default full hash observation are identical between cells; the dependency
probe is off. FPS is the benchmark-owned `.3dr` value under these diagnostics,
**not** observer-free FPS or the runner's process-elapsed estimate.

`wild_campaign_first.py` and the GT2-resume `wild_campaign.py` preserve exact
commands, settings, timestamps and five full build SHA-256 values.
`audit_wild.py` independently checks runtime/profile/lane, all staged image
receipts, `.3dr` digest/size/scene/resolution, nonempty capture and zero failure
counters. It also rechecks final build images. The final `wild-audit.json`
has `accepted_complete_campaign=true` for eight qualified artifacts.
The provider hash is
`ab76decb97e43d8e707557f0ec2bed1f06d8ef095c86e77dc0e94db4569797c6`.

All output suffixes below are under
`experiments/output/app-d3d9-3dmark05-`. GT1 uses prefix
`uniform-r632b-gt1-`; GT2 uses `uniform-r632c-gt2-`.

| Scene | Order / suffix tail | Mode | `.3dr` FPS | Encoded presents |
|---|---|---|---:|---:|
| GT1 | `1-mode0` | OFF | 31.7918 | 3,304 |
| GT1 | `2-mode1` | ON | 31.7978 | 3,305 |
| GT1 | `3-mode1` | ON | 31.7538 | 3,300 |
| GT1 | `4-mode0` | OFF | 31.8114 | 3,306 |
| GT2 | `1-mode0` | OFF | 30.7972 | 1,925 |
| GT2 | `2-mode1` | ON | 30.5204 | 1,907 |
| GT2 | `3-mode1` | ON | 29.4378 | 1,840 |
| GT2 | `4-mode0` | OFF | 29.5042 | 1,844 |

All eight have process return 0, no timeout and captured `.3dr` results.
Scoped live/stored screenshots show no black-screen or repeated-geometry
failure. This is sampled visual supervision, not exact comparison of every
wild frame. `chunk_reject`, `command_chunk_rejects`,
`gpu_command_buffer_errors`, `pipeline_build_fail_library` and
`draw_skipped_no_pipeline` are zero in every final receipt.

Two excluded setup attempts are preserved separately:

1. A pre-launch installer rebuild lacked llvm-mingw in PATH. It never became
   a measurement. Canonical PE builds were restabilized before the campaign.
2. `uniform-r632b-gt2-1-mode0` completed and emitted `.3dr`, but the default
   70-second screenshot raced scene/process completion (`missing_capture`).
   It is not counted. All four GT2 comparison runs use the same 30-second
   capture delay, with fresh suffixes. GT1 retains the common 70-second delay.

## Work reduction versus throughput

| Scene | OFF mean FPS | ON mean FPS | Change | Batch uniform-build scope, ms/Present OFF → ON |
|---|---:|---:|---:|---|
| GT1 | 31.8016 | 31.7758 | −0.08% | 0.452 → 0.351–0.353 |
| GT2 | 30.1507 | 29.9791 | −0.57% | 2.584–2.701 → 0.822–0.845 |

The full-build counts fall from 81,352/81,755 to **2/2** in GT1, and from
865,658/829,168 to **2/2** in GT2. The candidate therefore executes and removes
the targeted repeated work in real asynchronous replay. These scopes include
observation and elapsed-time effects; they must not be summed into a predicted
FPS gain or presented as pure memcpy/CPU service.

GT2's two OFF runs differ by −4.20%; its two ON runs differ by −3.55%.
Matched forward and reverse comparisons are both negative (about −0.90%
and −0.23%). The drift prevents confident attribution, but is not grounds to
declare those deficits harmless. Performance non-regression is still open.
No evidence here establishes a GPU ceiling or proves replay is irrelevant.

## Next gate

Keep the candidate default-off. Repeat the same-image `.3dr` comparison with
perf counters and frame sampling **both disabled**, using matched order and
a stable host; retain separate diagnostic runs for work attribution. Only
then decide promotion or investigate why reduced replay work does not improve
the frame critical path. Broader workloads and arbitrary FFP/clipping values
remain outside this bounded qualification.
