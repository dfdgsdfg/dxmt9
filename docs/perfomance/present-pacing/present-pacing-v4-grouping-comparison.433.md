---
domain: present-pacing
workload: STALKER CoP capture29
title: "V4 grouping qualification and localized L3 pixel counterexample"
type: experiment
status: partial-grouping-qualified-l3-pixels-rejected
updated: 2026-10-03
source: experiments/output/stkcop-v4-grouping-15/verification.md; experiments/output/stkcop-v4-grouping-15/paired-{l0,l64}-r1/result.json; experiments/output/stkcop-v4-grouping-15/paired-l3-r{1,2}/result.json; experiments/output/stkcop-v4-grouping-15/experiment-{l0-l64,l64-l0,l0-aa}-r1/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-capacity-comparison.432.md; specs/experiments/harness/replay/spec.md
---

# Bounded grouping on unchanged capture29

The final grouping binary qualifies L0 and L64 against original CPU logical
work, EVENT readiness and original exact GPU readbacks. Each qualification
contains both routes and two fresh GPU executions per route. The draw-command
limit is a physical command-publication threshold, not a draw-count threshold.
Positive grouping is restricted to Compatibility; ProductionSlot remains L0.
Headroom is zero throughout. No production default changes.

| Compatibility policy | Actual sources | GPU encode chunks | Render passes | Original pixels |
|---|---:|---:|---:|---|
| L0 | 3 | 5 | 18 | Exact |
| L64 | 10 | 12 | 25 | Exact |
| L3 | 172 | 174 | 181 | Rejected twice |

All preserve 512 physical commands, 2,799 draws and 80 compatibility batches;
GPU command-buffer errors are zero. Qualification is conditional on the
declared legal-NOOVERWRITE/closed-writers contract, not a proof of arbitrary
future backing preservation. This capture has EVENT readiness, not numeric
occlusion result coverage.

## CPU comparison

Each comparison uses two warmups and 16 measured fresh processes per arm, with
authenticated unchanged input, binaries, launchers and final receipts.

| Ordered arms | Body wall medians | Process CPU medians |
|---|---:|---:|
| L0 / L64 | 70.020 / 68.090 ms | 70.386 / 68.772 ms |
| L64 / L0 | 67.776 / 69.733 ms | 68.419 / 70.070 ms |
| L0 / L0 | 69.817 / 69.858 ms | 70.262 / 70.193 ms |

L64 reduces process CPU by 2.292% forward and 2.356% reverse, relative to
the L0 median in each comparison. This is cold CPU replay with an immediate
FIFO null-backend consumer. It is not Metal encode time, GPU throughput or
game FPS; the increase in real GPU pass count remains a separate tradeoff.
A/A median differences are 0.060% wall and -0.098% CPU, not a confidence
interval or universal noise bound. All 108 executions (12 warmups, 96 measured)
and final authentication pass. L3 is not performance-eligible.

## Preserved L3 counterexample

Both independent L3 attempts produce identical source and output SHA-256
`d0d37fdf01c59c5e84d0e84991f6405dac1b100f160f6a60bdbbaa8372735be3`,
instead of original `2c616e5aa12d5a0d03f4fb2229809ee3f4b360b243f7aae05a5016d4f1a67113`.
Exactly 257/921,600 pixels differ in x=687..711, y=158..177. RGB only darkens;
alpha is unchanged. Maximum channel delta is 14, mean absolute byte delta
0.0010921224. The original Present target is 1280x720 X8R8G8B8 without MSAA.

Actual L0/L3 color Load/Store counts are 21/387, depth 12/175 and stencil
9/138. DontCare and Resolve counts are zero, as are framegraph optimization
and tile-FFP counters. This does not support dropped attachment storage or
framegraph reorder as explanations. `startRenderPass` in
`src/dxmt9/dxmt9_draw_encoder_chunk.mm` resets binding shadows and dirties
uniforms on each new encoder; a generic missing reset is not established.

Candidate causes are attachment store/load or blending/depth precision,
isolated successor binding state, and publication-sensitive mutable resource
contents. The capture includes RGBA16Float targets, but their presence alone
does not prove precision loss. Logical mapping and query readiness cannot
certify binding bytes or attachment preservation.

Separate `witness-l0-r1` and `witness-l3-r1` diagnostics in the same artifact
directory enable the existing `--observe-encode-draws` observer. Both qualify
all 2,799 draws and 26,623,246 original IB/VB footprint bytes with zero address,
snapshot-association or byte-proof rejections. L0 remains exact; L3 retains
the identical wrong pixel digest. This narrows selected index/vertex address
and instantaneous byte reconstruction as causes. The witness does not prove
texture/uniform equality or future GPU-execution-time backing preservation,
and its callbacks/read locks make it ineligible for performance comparison.

Next: identify the first differing attachment or binding at an L3-only seam.
Separate a diagnostic render-pass close/reopen from a real source publication
with unchanged original input. A focused immutable RGBA16Float blending-to-RGBA8
fixture can test the first mechanism. Do not relax exact qualification or
promote the failed candidate based on its small affected area.
