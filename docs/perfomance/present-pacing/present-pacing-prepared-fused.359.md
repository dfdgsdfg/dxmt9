---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H359: Fuse serial metadata into publication resource marking"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/{prepared-cost-h359-cold,prepared-cost-h359-repeat}/summary.json; experiments/output/{prepared-cost-h359-cold,prepared-cost-h359-repeat}/{aa0,aa1,ab1,ba1,ba0,timer_off1}/result.json; experiments/output/prepared-cost-h359-cold/{dxmt9-h359-native-final.log,dxmt9-h359-x86.log,dxmt9-h359-unix-final.log,dxmt9-h359-tla.log,dxmt9-h359-shadow-final.log}
related: present-pacing-prepared-minimal.358.md
---

# H359: One final command-view traversal

Base `48687601` plus this implementation. The optional serial read metadata
builder now consumes final command views from `markSlotResourcesImpl`, after
each command's captured-buffer stamp batch is flushed. The separate command
walk and draw-candidate header scan are removed. The existing raw read-set
extraction remains: this is fusion at final-Slot publication, not at initial
Replay emission, and it does not reuse the resource visitor's different
pin/backing semantics as a serial read-hazard set.

`PreparedSerialReadsBuilder` holds a private mutable prefix, reserves bounded
storage up front and publishes immutable ownership only after complete original
command-index coverage. Missing, repeated or out-of-order positions and capacity
failure discard the prefix and remain failed. Seal is consuming; moved-from or
already-sealed builders cannot publish again. Preparation failure never stops
the resource-mark loop. The disabled specialization has an empty inline callback
and creates no builder. GPU pins, stamps, command order, source identity and
Slot reclamation remain governed by their existing contracts.

## Verification

- Native prepared-plan, hazard and actual queue publication with resource dedup
  OFF/ON: 4/4.
- x86_64 equivalents plus lifecycle, session-join and install-name audit: 7/7.
- Full Unix build and existing TLA regression: pass (205.76 seconds).
- Shadow analyzer: pass after correcting its stale pre-H356 fixed-plan size
  expectation from 151624 to 168008 bytes. The general shadow plan is unchanged.
- Counter schema/table/callsite audits and `git diff --check`: pass.

The shared incremental builder is tested over 341 append histories of length
0–4 with ordinals 0–3, in addition to H358's read-hazard differential, exact
capacity, allocation-failure, stale-key and reclaim tests. Actual queue fixtures
exercise the production callback adapter for both resource marking policies.
The bounded publication reducer remains the ownership model; no new scheduler,
queue protocol or GPU resource lifetime proof is claimed.

## Measurement boundaries

H358's same-input script runs the authenticated GT2 capture-source boundaries,
ProductionSlot exact route and identity-serial policy. Each campaign has six
sequential blocks with two warmups and sixteen measured iterations per block.
Builds and tests finish before timing. Every block verifies equal loaded image
hashes, input components, capture oracle and source/output pixel bytes.

The timing schema intentionally changes: `prepared_build_and_mark` includes
allocation, the fused marking/append loop and seal. `resource_mark` now includes
the append callback when ON. Neither is the historical isolated build scope;
do not subtract medians to invent isolated append cost. `prepare_slot` is the
unchanged enclosing elapsed-wall scope and is the comparison boundary. Scopes
are not thread CPU and nested/concurrent values must not be summed.

The first campaign is retained in full. Its forward ON block has Encode median
49.112ms, while the next ON block returns to 40.331ms. That inconsistent block
effect prevents causal attribution to fusion. A second complete campaign uses
the same binary and input rather than deleting the inconvenient first result.

Across both campaigns all 216 iterations pass. All source/output readbacks
remain byte-exact with SHA-256
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
ON consumes 646 prepared rows per iteration; OFF consumes zero. Attached-row
fallbacks and GPU command-buffer errors are zero. The twelve blocks have equal
input components and loaded-image fingerprints. Provider executable SHA-256:
`977fcacfb2826d582635d80cbe6ebd395ec4ed5b558a6c65a13282bb50f6a88b`;
Unix library SHA-256:
`ec959607600a0da2f68ade2079a3c7446d85b168a02370c16431b152afe42c6f`.

Second-campaign medians, milliseconds per captured-frame iteration:

| Block | Prepare-slot | Build + mark | Mark + append (ON) | Encode chunk |
|---|---:|---:|---:|---:|
| aa0 OFF | 0.752 | 0 | 0.731 | 40.332 |
| aa1 OFF | 0.741 | 0 | 0.721 | 40.554 |
| ab1 ON | 0.906 | 0.884 | 0.794 | 40.120 |
| ba1 ON | 0.880 | 0.858 | 0.767 | 40.282 |
| ba0 OFF | 0.761 | 0 | 0.739 | 42.598 |
| timer_off1 ON | 0.874 | unmeasured | 0.775 | 40.600 |

The second campaign still has block drift (reverse OFF Encode 42.598ms versus
A/A OFF 40.332/40.554ms). Forward/reverse apparent Encode savings are therefore
not promotion evidence. Preparation overhead is approximately 0.165ms forward
and 0.119ms reverse, comparable to H358's 0.15–0.18ms. The separate-build
comparison does not establish a material speedup from fusion. Structural
removal of one command projection walk is proven; meaningful Encode or live
FPS improvement is not. No additional unchanged-policy runs are used to select
a favorable result.

This lane remains default OFF. Eliminating a traversal is not itself evidence
of FPS improvement. Allocation, per-command raw read extraction, broader
consumer savings, Arena/parallel integration and live critical-path economics
remain separate work.
