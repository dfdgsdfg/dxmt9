---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "CPU slow-mode attribution: software attachment Clear dominates the substituted backend"
type: experiment
status: diagnostic-attribution
updated: 2026-09-16
source: experiments/output/gt2-cpu-slowmode-20260916-r619.noindex/profile-plan.json; experiments/output/gt2-cpu-slowmode-20260916-r619.noindex/profiles-complete.json; experiments/output/gt2-cpu-slowmode-20260916-r620.noindex/campaign.json; experiments/output/gt2-cpu-slowmode-20260916-r620.noindex/summaries.json; experiments/output/gt2-cpu-slowmode-20260916-r620.noindex/audit.json
related: docs/perfomance/mini-replay-bisection/gt2-policy-matrix.06.md; specs/experiments/harness/replay/fixed-input-experiment.md; specs/experiments/gap.md
---

# CPU slow-mode attribution

## Verdict

The identical-policy process variation is concentrated in **replay CPU work**,
not fixture creation, controlled completion or reset. External samples name
software attachment Clear, predominantly its depth-shadow fill, as the
strongest owner. The CPU fixture executes a shadow path the normal
GPU-readback-capable provider skips for GPU-owned attachments. Whole-iteration
CPU time is consequently not representative of production replay alone.

This localizes the variable cost, but does **not** establish why the same
software loop runs at different speeds across processes. Rosetta translation,
address layout, CPU placement/frequency and cache effects are still hypotheses.
No runtime behavior, policy/default, Clear implementation or pixel oracle was
changed. Neither a policy ranking nor an FPS gain is accepted.

## Unchanged-image external samples: r619

Four Compatibility-exact processes each ran 32 iterations, followed by two
six-iteration verification processes. The providers/libraries are byte-identical
to the [preceding matrix](gt2-policy-matrix.06.md). Each process was externally
sampled for four seconds at a 2ms interval, starting after launch plus 0.75s.
These are deliberately perturbed diagnostic observations, not ranking samples.

| Process | CPU median after two warm-ups (ms) | Main-thread samples in `Device::clear` |
|---|---:|---:|
| 01 measure | 258.277 | no usable stacks; preserved, not treated as zero |
| 02 measure | 354.240 | 1,132 / 1,535 = 73.75% |
| 03 measure | 226.677 | 830 / 1,536 = 54.04% |
| 04 measure | 228.276 | 816 / 1,530 = 53.33% |

The clear stack runs through `replayNonDrawRecord` → `dxmt9p_device_clear` →
`Device::clear` → `fillDepthStencil`, with some descendants unresolved by
Rosetta symbolization. Count the inclusive Clear frames; counting only named
`fillDepthStencil` leaves misses those descendants. Sampling fractions are not
precise per-iteration costs and must not be subtracted to manufacture a
corrected performance result.

Verification processes cost roughly 1.24s CPU per iteration; existing verify
phase fields show about 1s of semantic/resource observation. They are unsuitable
for policy timing and are not mixed with measurement.

## Exclusive diagnostic ledger: r620

`cpu-experiment --run-mode diagnose --policy-lane` now preserves measurement's
production calls and conservation checks while recording an exclusive nested
wall/process-CPU ledger in the cold controller. No clocks enter renderer
signatures. Ordinary `measure` retains zero/unmeasured phase fields. Diagnostic
receipts have `accepted=false`, `diagnostic=true`, no semantic qualification,
and cannot enter policy ranking even if their mode label is changed.

Only the x86_64 release CPU test controller was rebuilt. Its SHA-256 is
`dd43c2e5ddf2152f0b152b25192dd01a34c326fcebf48a4975a6012b4b1775ff`;
the shared runtime, winemetal, GPU provider and validator remain byte-identical
to r619. Both controller images are preserved in the artifact directories.
All runs use the unchanged r405 GT2 original and source-plan SHA-256
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
There is no sampler or concurrent build/test during this block.

Eight diagnostic processes each execute two warm-ups plus 16 retained samples.
Ordinary measurement before/after uses the same 18 iterations, followed by two
full-oracle verification iterations: **182 iterations total**. The independent
audit reauthenticates original/staged input components, actual loaded images,
source settlement, lifecycle conservation and every phase sum. Every iteration
has 1,866 draws and 185 submitted/completed/reclaimed sources.

| Diagnostic process | Total CPU median (ms) | Replay CPU median (ms) |
|---|---:|---:|
| 02 | 251.641 | 194.300 |
| 03 | 229.161 | 171.627 |
| 04 | 251.409 | 192.656 |
| 05 | 319.085 | 260.152 |
| 06 | 233.977 | 172.377 |
| 07 | 232.014 | 172.855 |
| 08 | 317.798 | 260.128 |
| 09 | 230.460 | 173.123 |

For the representative 03→05 shift, total CPU grows 89.924ms and replay grows
88.526ms. Admission stays around 19.2–19.5ms; mutation 15.7–16.1ms;
object setup 9.8–12.9ms; publication 7.6–7.7ms; completion 0.92–0.94ms;
cleanup 2.44–2.51ms; fixture creation about 0.32ms and teardown about 0.02ms.
These are individual phase medians, not an additive decomposition of the total
median. Individual receipt sums do conserve both clocks exactly.

The bracketing ordinary measurement medians are 226.507 and 317.207ms: the
slow mode is not unique to enabling diagnostic clocks. Wall and CPU track
closely in the diagnostic processes. No `pmset` warning was recorded, which
does not prove fixed frequency/core residency or host isolation.

## Code explanation and remaining gate

- `src/dxmt9/dxmt9_device.hpp`, `Device::supportsGpuReadback`, defaults false.
  The test `RoutingDevice` does not override it. Having real CPU-visible Metal
  buffers/textures is not the same as implementing GPU readback.
- `src/dxmt9/dxmt9_device.cpp`, `DeviceImpl::supportsGpuReadback`, returns true.
- `src/d3d9/core_resources.cpp`, `Device::clear`, skips CPU shadows only when
  GPU readback is trusted and the attachment is GPU-owned. Otherwise depth
  Clear locks the shadow, allocates scratch, fills pixels and copies back.
- `src/d3d9/core_format_utils.cpp`, `fillDepthStencil`, performs per-pixel
  format/depth conversion. This is the sampled hotspot, not queue waiting.

The next useful implementation is a **truthful attachment-content authority
boundary** for the CPU policy fixture, not a false `supportsGpuReadback=true`
override or removal of Clear records. It must preserve actual command order,
attachment identity, resource lifetime and conservative rejection of operations
requiring unavailable contents. The full semantic and original-pixel GPU
qualification must remain independent. Then repeat A/A and forward/reverse
policy comparisons with production-equivalent CPU work. The current results
do not justify changing Direct/headroom defaults or optimizing software Clear
as a production GPU performance fix.

## Validation

The full x86_64 build and six focused Meson tests pass: CPU phase ledger,
capture-source policy, measurement/diagnostic conservation, CPU policy Python,
CPU policy CLI and CPU experiment CLI. Coverage includes nested exception
unwinding, unavailable/regressing clocks, inconsistent totals, diagnostic
relabel rejection and an actual diagnostic CLI run. The final rebuild after
indentation cleanup has the same controller hash as the measured image.
Post-instrumentation verification matches the old image's semantic digest and
all 1,866 ordered draw observations. Performance-document source audit and
`git diff --check` pass. `final-testlog.{txt,json}` are retained in r620.
No queue/lifetime transition or production rendering code changed; this round
does not claim new TLA or GPU pixel evidence.
