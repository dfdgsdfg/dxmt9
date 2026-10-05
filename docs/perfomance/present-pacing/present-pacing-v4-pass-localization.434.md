---
domain: present-pacing
workload: STALKER CoP capture29
title: "L3 pixel divergence localized to a common RT0 interval"
type: experiment
status: hot-row-invalidation-corrected-l3-qualified
updated: 2026-10-03
source: experiments/output/stkcop-v4-grouping-16/verification.md; experiments/output/stkcop-v4-grouping-16/pass-{l0,l3}-r1/result.json; experiments/output/stkcop-v4-grouping-16/binding-{l0,l3}-r1/result.json; experiments/output/stkcop-v4-grouping-16/roundtrip-r1.json; traces/stkcop-v4-grouping-16/{l0,l3}-first-common.bin.json; experiments/output/stkcop-v4-hot-row-17/verification.md
related: docs/perfomance/present-pacing/present-pacing-v4-grouping-comparison.433.md; specs/experiments/harness/replay/spec.md
---

# Natural pass localization

Existing pass-end RT0 hash dumps and effect-draw/original binding witnesses
retain the same L0 exact result and L3 rejected result as [H433](present-pacing-v4-grouping-comparison.433.md).
No extra draw split or CPU drain is inserted. These diagnostic readbacks add
GPU work and are not policy qualification or performance evidence.

The first differing common draw-bearing RT0 endpoint is original event5570,
record2, one-based draw2741: L0 sequence4/encoder9 versus L3 sequence168/encoder0.
The prior equal endpoint on the same RT0 is event5297/record1, draw2597.
This narrows the color divergence interval to 144 draws, event5332/record2
through event5570/record2, spanning one L0 pass versus 19 L3 passes.
RT0 is A8R8G8B8, 1280x720, with known Store and one sample. Other MRTs and
depth/stencil remain unobserved; matching RT0 does not certify their contents.

Raw readback at that endpoint differs at 181 pixels / 543 bytes, all RGB
darkening in x=688..711, y=159..177, maximum channel delta 14. Alpha matches.
The later final image differs at 257 pixels. The legacy raw comparison CSV
mislabels its changed-byte count as `changed_pixels`; the 181-pixel count is
independently calculated in four-byte pixels, not copied from that column.

## Mechanism fixture

`render_pass_roundtrip_metal_spec.mm` tests immutable 12-draw blending with
a boundary after draw3, independently comparing same-CB pass close/reopen and
two ordered CBs against a repeated uninterrupted reference. RGBA8Unorm and
RGBA16Float, additive and source-over cases all produce byte-exact source and
sampled RGBA8 output with zero GPU errors. Native arm64 and x86_64 tests pass.

The fixture uses safe shader math and excludes depth/stencil, sRGB, mutable
uploads and dxmt9 scheduling. It fails to reproduce a generic color-roundtrip
problem, but cannot rule out workload-specific state or attachment behavior.
No runtime renderer change or new production scheduling proof is claimed.

## Consumed input discriminator

The original index/vertex footprint witness passes all 2,799 draws even when
L3 pixels fail. Actual bound VS/PS/FFPVS byte logs agree across the two routes;
some FFPPS fields differ. Actual issued PSO receipts and compiled shaders
match for all 283 differing draws. Of 56 fog-field differences, 53 are dead
in their actual fragment entry. Three draws (2647, 2688, 2702; original
5457/1, 5534/1, 5551/1) consume fog through FFPPS slot 3: L0 binds mode/source
3/1 while L3 retains 0/0 from an earlier fog-inactive command. Fresh L3
encoders with the same fog-active state instead bind 3/1.

The production binding predicate omits hot RS/TSS/viewport inputs from its
fixed-payload identity. Both FFPPS and FFPVS require these inputs. Corrective
work qualifies immutable hot-row identity by source, segment and row, rather
than hashing another per-draw struct or comparing reusable scratch addresses.
Segment qualification is essential: different arena segments can both have
local row zero under the same source sequence.

At the end of slice16, L3 remained rejected and was not measured for
performance. Deterministic regression coverage and fresh original CPU/GPU
qualification of a corrective binary were the required next gate; the
following slice supplies that causal test without changing the reference.

## Corrective verification

The source/segment/row-qualified fix restores unchanged original pixels in
`experiments/output/stkcop-v4-hot-row-17/l3-r1`: source and final SHA-256 are
`2c616e5aa12d5a0d03f4fb2229809ee3f4b360b243f7aae05a5016d4f1a67113`,
with 2,799 draws, 174 encode chunks and zero GPU errors. Fresh
`paired-l3-r1` also passes both CPU routes, original EVENT readiness and two
original GPU executions per route. This establishes causality for the retained
L3 counterexample; it is not general rendering or FPS proof.

Native build and four focused native/Metal tests pass, including bounded
hot-identity transition/owner checks and serial/parallel slot-3 negative
controls. Fresh L0/L64 paired qualifications also pass on the same corrected
binary. x86 focused tests/audit pass 5/5 and all 230 TLA configurations pass.
Five corrected-image comparisons pass 180 executions (two warm-ups plus
16 measured iterations per arm). L0 A/A process-CPU median difference is
-0.039%; L3 is 9.207%/10.543% higher than L0 in forward/reverse order, while
L64 is 2.820%/2.522% lower. Source counts are 3/172/10 for L0/L3/L64 with
512 consumer commands and 2,799 draws preserved. These are immediate-FIFO
CPU replay-body measurements, not actual Metal encode time or FPS. Background
system activity and wall-time outliers are retained in the receipts. No
policy default is changed. Full evidence and limitations are recorded in
`experiments/output/stkcop-v4-hot-row-17/verification.md`.
