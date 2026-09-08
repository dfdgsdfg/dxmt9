---
domain: present-pacing
workload: actual PE public COM; 3DMark05 GT2; 3DMark06 GT2
title: "Qualified pin writer: remove unused private output"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-qualified-pin-output-H301-20260909/analysis
related: present-pacing-pe-draw-parent-tail.300.md; present-pacing-pe-identity-call-abi.297.md; present-pacing-pe-warm-pin-identity.296.md
---

# Qualified pin writer: unused output removal

H301 starts at `868dfa6f`. The sole production caller of
`PeSemanticBatchOwner::pinPreparedQualifiedBinding` discarded its physical-pin
ordinal output and always supplied a non-null wire-index pointer. The helper
now has only the required wire-index reference output. The unused caller
temporary, argument, success stores and optional-pointer branch are removed.

This is a private representation refinement under R-CORE-REC-7.36, not a new
transaction or scheduling policy. Wire-index assignment remains before
validation, including failure paths. Current pin/object/kind/generation/id
checks, the new-pin identity snapshot before retain, insertion/count ordering,
rollback and settlement are unchanged. No layout, public COM, bridge, wire,
generic-pin or callback contract changes.

## Linked and functional evidence

Four canonical builds pass. Host native checks pass 8/8 and x86_64-native
checks pass 4/4, including install-name validation. The existing TLA+ regression
passes in 147.41 seconds; no new formal transition or broader proof is claimed.
Existing compact seven-family mutation, null/duplicate, warm-hit, exact/segmented
byte, retain-fault, preserved-prefix rollback and retry tests exercise the
production writer. `economics-main.json` archives the focused test logs.

| Linked scope | PE32 baseline → candidate | PE64 baseline → candidate |
|---|---|---|
| Explicit helper arguments | 6 → 5; `retl $0x18` → `$0x14` | 6 → 5 |
| Each of five typed helper spans | 432 → 432 bytes | 528 → 496 bytes |
| Each helper's linear instructions | 136 → 134 | 143 → 140 |
| Top-level qualified writer span/instructions | 1856/452 → 1856/452 | 2112/396 → 2112/396 |

The frozen linked receipts include exact direct call contexts for seven
logical families, with shared buffer/surface helper instantiations. The PE32
caller passes five rather than six stack arguments; PE64 no longer initializes
and passes the unused output temporary. Counts include padding, not cycles,
executed paths or a whole-parent CPU saving. Unchanged writer metrics are not
a byte-identity or performance-neutrality proof.

Both bridge DLLs and the Unix provider are byte-identical to baseline. The
only changed product source is `src/d3d9/d3d9_pe_semantic_owner.hpp`.
Candidate PE32/PE64 image hashes are
`898394b8eb8e54b7e357a47376ee6b1efb285611e174ad80879435d8759241ab` and
`c36d0cfaf5ec3a2c20a29440d7cc3f8c3c73dd3167091e253060674223d32f7c`.

## Actual public-COM costs

Six PE processes pass 162 state/input/HRESULT/pixel/release cells. Four
logging-OFF timing processes use the same fixture and recipe, one per
version/width: 512 measured + eight warm groups of 32 iterations, offload on,
inline/segmented/tape/index-cache and perf/recorder diagnostics off. Two
candidate boundary traces qualify no timed recorder flush. All 56,322 group
and tail samples are retained; no slow sample is removed. PE32 is Sikarugir
WoW64/Rosetta, and PE64 is not native ARM64 PE.

| Whole-matrix raw scope, including warm groups | PE32 baseline/candidate | PE64 baseline/candidate |
|---|---:|---:|
| Setter/draw QPC | 0.419 / 0.650 s | 0.281 / 0.359 s |
| EndScene/Issue/GetData tail | 17.807 / 25.430 s | 17.406 / 23.278 s |
| GetData portion | 17.460 / 24.858 s | 17.141 / 22.842 s |
| Summed whole-row process CPU | 15.56 / 23.60 s | 14.62 / 21.04 s |

These candidate totals are higher and remain warning evidence, not a saving.
Every timing process has exactly 14,040 GetData calls for 14,040 groups.
H299/H300 already demonstrate broad same-image population shifts in this
drain-heavy fixture. A main-thread process snapshot near the end of the
candidate PE64 run also observed Spotlight `mds_stores` at about 99% CPU;
that observation neither proves overlap throughout the run nor attributes the
delta to Spotlight. No build, native test or second benchmark overlaps a
timing run. Small-delta economics and exclusive PE CPU remain unqualified;
process CPU includes setup, warm-up, completion and oracle work on all threads.

Raw logs, strict reparsing, identical fixture/tool/build recipes, full source
and five-product identities, actual staging and public-vtable bindings are
preserved in `economics-main.json` and its per-run receipts. Bounded FFP pixels
and getters do not prove all programmable-shader bytes or temporal game frames.

## Fresh GT2 spots and disposition

Both spots use the canonical Sikarugir catalogue runner, perf/warn profile,
validation off, frame sampling on and PE observers off. Offload is on;
inline/segmented/tape/index-cache are off. 05 focus keepalive is off; 06 uses
the focus-free named-lane GUI selector. No manual UI operation, build, native
test or second benchmark overlaps either run. Before/after process snapshots
are retained; Spotlight was no longer among the high-CPU processes before
launch. They are snapshots, not continuous proof of a quiet machine.

| Product | Resolution | Consecutive scene frames | Scene wall | Sampled FPS | README delta |
|---|---|---|---:|---:|---:|
| 05 GT2 | 1024x768 | 1–1950 (1,950) | 64,569.427 ms | 30.200 | +0.33% vs 30.1 |
| 06 GT2 | 1280x720 | 988–2163 (1,176) | 64,064.313 ms | 18.357 | +1.42% vs 18.1 |

Positive-wall frames with more than 100 draws select the consecutive scene;
no slow scene frame is trimmed. These are renderer scene-sampled averages,
not process-elapsed estimates or observer-free benchmark scores. 05 exits
normally and its `.3dr` is recovered. 06 reaches its normal post-scene GUI
timeout/143, emits no `.3dr`, and is accepted by the catalogue's timeout policy.
Both saved captures show rendered scenes; neither proves temporal correctness.

All eight reported chunk/GPU/pipeline health counters are zero. 05 has full
coverage through frame 1951. 06's snapshot reaches 2160, leaving the final
three scene frames without full-health coverage. These are fresh runs; no
earlier user-affected result is incorporated. Both runtime staging sets match
the candidate's five images. `final-audit-main.json` binds the code, linked,
native/TLA, public-COM, wild, capture/result and documentation evidence.

Retain the independently reviewed dead-output cleanup without economic
promotion. Near-README single spots do not establish causal FPS improvement,
performance neutrality or reduced CPU usage. README stays unchanged. The
remaining prerequisite for judging tiny PE savings is a calibrated bounded
producer-window harness with fewer diagnostic completion drains, retaining
mandatory final completion and state/pixel/order checks; production query
semantics must not be weakened to improve a fixture score.
