---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "GT2 corrected CPU policy matrix: a small Direct Slot benefit, no 5MiB case"
type: experiment
status: qualified-scoped-comparison
updated: 2026-09-16
source: experiments/output/gt2-policy-matrix-20260916-r622.noindex/campaign.json; experiments/output/gt2-policy-matrix-20260916-r622.noindex/campaign-complete.json; experiments/output/gt2-policy-matrix-20260916-r622.noindex/analysis.json; experiments/output/gt2-attachment-authority-20260916-r621.noindex/audit.json
related: docs/perfomance/mini-replay-bisection/attachment-authority.08.md; docs/perfomance/mini-replay-bisection/gt2-policy-matrix.06.md; specs/experiments/harness/replay/fixed-input-experiment.md
---

# GT2 corrected CPU policy matrix

## Verdict

After the [attachment-authority correction](attachment-authority.08.md),
ProductionSlot exact costs **3.48–3.69% less CPU time** than Compatibility exact
in both execution orders. This is a qualified, single-block fixed-input CPU
observation, not game FPS, isolated replay service time or a universal win.
The 1MiB/64MiB candidate has a smaller 1.10–2.97% signal relative to exact;
5MiB/64MiB has no demonstrated CPU benefit and materially worse GPU-path
capacity economics. No runtime default is promoted.

## Conditions and qualification

- Same r405 original, source boundaries, binaries and loaded images as r621;
  manifest, source-plan SHA, build/image hashes and host snapshots are in
  `campaign.json`. x86_64 release/O3 through Rosetta, CPU fresh-device reset,
  immediate controlled completion, real-GPU cold-device qualification.
- Ten comparisons: A/A brackets, four policy pairs and their reverse orders.
  Each side runs two detailed CPU verification iterations, two GPU exact
  iterations, then two warm-ups plus sixteen measured CPU iterations.
- **40 CPU verification, 40 warm-up, 320 measured CPU and 40 GPU exact
  iterations**. All comparisons and the independent post-run reauthentication
  pass. Original source/output RGBA remain exactly
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`, with zero
  GPU command-buffer errors. No tolerance or source boundary is changed.
- No concurrent build/test/sampler/trace during measurements. All raw samples,
  warm-ups, policies, command logs and child receipts are retained.
- CPU timings include setup, admission, mutations, replay, publication,
  controlled completion and reset, and exclude real encoding/GPU/Wine supply.
  GPU execution qualifies correctness and mechanisms here, not GPU speed.

A/A process median differences are **-0.77%** at the start and **+0.08%** at
the end. The preceding three r621 pairs were -0.28%, -0.27% and -0.63%.
The old large Clear-driven process modes did not recur in these blocks;
this does not guarantee their absence on all future inputs or hosts.

## CPU cost

Reverse results below are normalized to the same baseline/candidate direction,
not the literal second-process/first-process ratio. Negative means lower cost.
The balanced value is the geometric mean of two ratios, descriptive only.

| Baseline → candidate | Forward | Reverse | Balanced |
|---|---:|---:|---:|
| Compatibility exact → ProductionSlot exact | -3.69% | -3.48% | -3.58% |
| ProductionSlot exact → 1MiB/64MiB | -2.97% | -1.10% | -2.04% |
| 1MiB/64MiB → 5MiB/64MiB | -1.30% | +0.12% | -0.59% |
| ProductionSlot exact → 5MiB/64MiB | +0.49% | +0.59% | +0.54% |

The route comparison is 72.370→69.702ms forward and 73.130→70.588ms in the
normalized reverse direction, about 2.5–2.7ms per complete CPU harness
iteration. Do not compose separate table rows into a synthetic policy ranking:
these small effects use different processes, and the 1MiB/5MiB ordering flips.
1MiB remains a promising candidate; its precise gain needs repetition and
attribution before any asynchronous/wild promotion.

## Mechanisms and capacity

All CPU iterations conserve 185 submitted/completed/reclaimed sources and
656 source-summary commands. Compatibility executes 0 Direct/1,866
compatibility draws; each Direct policy executes 1,856 Direct/10 compatibility
draws with 182 leases. Thus the measured route is actually exercised.

The CPU lane immediately completes each source and recycles far fewer payloads
than real GPU execution. These are deliberately separate evidence columns:

| Direct policy | CPU provision/reuse per iteration | CPU SoA growth | GPU SoA growth per iteration | GPU retained capacity | GPU optional denials per two iterations |
|---|---:|---:|---:|---:|---:|
| exact | headroom mechanism disabled | 1 | 21 | not reported by disabled ledger | 0 |
| 1MiB/64MiB | 3 / 179 | 0 | 0 | 20.86MiB | 0 |
| 5MiB/64MiB | 1 / 181 | 0 | 9 | 61.84MiB | 56 |

GPU two-iteration process totals are 52 provisions/312 reuses for 1MiB and
24 provisions/284 reuses for 5MiB. The latter additionally reuses exact
fallback twelve times; there are no exact-fallback replacements or rejections.
Denials are safe optional-provisioning fallbacks, not failed rendering.
Capacity figures are ledger-retained payload bytes, not process RSS or total
Metal allocation. A zero exact-mode gauge must not be interpreted as no memory.

The CPU lane cannot predict the GPU lane's outstanding-owner pressure:
5MiB has no CPU denial but 56 GPU denials. That difference is why a correct,
faster synchronous CPU experiment alone cannot promote a runtime policy.

## Next gate

Keep exact as the existing baseline and 1MiB/64MiB as an opt-in candidate.
Use the corrected diagnostic phase ledger to separate actual replay savings
from fresh resource setup/admission, then qualify asynchronous residency/pacing
and repeated wild A/B before default promotion. There is no evidence here for
raising headroom to 5MiB. Broader tapes, warm-resource steady state and source
arrival timing remain outside this single-frame controlled-completion result.
