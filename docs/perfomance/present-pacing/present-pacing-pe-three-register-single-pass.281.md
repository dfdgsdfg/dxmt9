---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2; controlled PE constant setters
title: "One-pass three-register constant updates"
type: evidence
status: measured-local-gain-not-promoted
updated: 2026-09-08
source: experiments/output/pe-three-register-single-pass-20260908/analysis
related: present-pacing-pe-constant-economy.280.md; present-pacing-pe-three-register-equality.279.md
---

# One-pass three-register constant updates

H281 tests H280's next bounded candidate: specialize the 16-byte/count-three
Live update into three sequential compare/conditional-copy operations. This
removes the whole-range precomparison followed by a generic mismatch walk.
Other lengths retain one generic loop; both paths share the existing
first/last-changed merge. No persistent equality cache, lifetime, transport,
worker or publication change is involved.

## Correctness and linked code

R-CORE-REC-7.40 and R-VERIF-1.5–1.8 are covered here by the existing exhaustive
native value/dirty refinement against the independent reference transition,
plus additive selected-range tests. A new concurrent TLA+ model would not add
coverage to this call-local transform: no interleaving or progress contract
changes. This is not a claim that native tests prove arbitrary memory faults
or Metal driver behavior.

The new source-only guard fixture puts exactly 48 readable, immutable bytes
at both edges of a protected page and exercises all eight change masks
(16 cases). Destination storage remains vector-backed; this is not a
protected-destination test. Seven nonempty masks also run A, clear/A, B,
clear/A with prior outside dirty obligations (28 apply cases). Existing
dirty-mask/alignment, zero/nonzero growth, raw-bit and self-alias pins remain.
The three focused native targets pass on arm64 and x86_64; the Unix
install-name audit and full canonical PE32/PE64/Unix builds pass.

Actual PE32 and PE64 default-selector pixel fixtures pass: each has 24
signature rows and 96 readback samples. Their exact baseline comparison is
kept separate from the economy fixture, which does not draw. The existing
literal-channel tolerance is unchanged.

| Linked 16-byte kernel | Baseline | Candidate |
|---|---:|---:|
| PE32 bytes / instructions | 594 / 208 | 766 / 251 |
| PE32 prologue stack subtraction | 8 | 8 |
| PE64 bytes / instructions | 561 / 176 | 780 / 223 |
| PE64 prologue stack subtraction | 40 | 56 |

Direct-call targets/counts, public setter bodies, exports, vtable/RTTI owner
and the 4-byte kernel are unchanged. The extra PE64 frame carries start/end
state and a stack-based count subtraction/reload around materialization.
PE32's companion loop also changes register transfers. Both kernels introduce
a count-three dispatch; whole-symbol size is not a dynamic-cost metric.
Each compare uses bounded 16-byte loads. Changed elements still reload their
source before storing: one comparison does not mean one source load. There
are no new `memcmp`/`memcpy` calls in the warm path.

## Measurement identity

Baseline is `1fc29bd9` (restored H278 renderer). Candidate is the frozen
`analysis/measured-runtime.patch` on that commit, not a clean-HEAD binary.
The local driver records that patch digest for every run. The PE fixture,
bridges and Unix provider are unchanged; build options are compared with the
canonical-shaped frozen control tree.

| Artifact | Baseline SHA-256 prefix | Candidate SHA-256 prefix |
|---|---|---|
| PE32 `d3d9.dll` | `af8e1fde12b6b7ce` | `bda0d6b2a5a69c48` |
| PE64 `d3d9.dll` | `c694f8565e44427d` | `8f225b58228a66ae` |
| Unix provider | `98f16a64a310aba5` | unchanged |

The checked-in H280 harness uses 50 million warm calls per cell and explicit
process CPU, not PE-exclusive or thread-exclusive time. It prices all six
families and counts 0/1/3/4; controls are not subtracted. Repeated equal writes
do not price dirty settlement, owner emission, draws or FPS. No build/test
overlaps a timed process. Local and whole-workload decisions remain separate.

## Controlled actual-PE results

Eight fresh processes run baseline/candidate/candidate/baseline for each PE
width. All 528 cells qualify. `verified-local.json` rederives the transcripts
and verifies full image, fixture, environment, runtime-diff and build-option
identities, as well as exact equality of the 192 baseline/candidate pixel
samples. The existing 10 ms CPU quantum is 0.2 ns per call at this denominator;
two-observation averages do not improve the timer's underlying resolution.

| VS float cell | PE32 baseline → candidate process ns/call | PE64 baseline → candidate process ns/call |
|---|---:|---:|
| Count 3, equal | 18.0 → 17.8 | 10.7 → 10.1 |
| Count 3, last-only changed | 20.9 → 19.7 | 11.4 → 11.1 |
| Count 3, middle-only changed | 20.6 → 19.6 | 11.5 → 11.1 |
| Count 3, all changed | 23.0 → 20.5 | 12.3 → 11.6 |
| Count 1, equal | 16.3 → 17.0 | 8.9 → 9.2 |
| Count 4, equal | 19.9 → 20.1 | 11.5 → 11.8 |
| Count 0 control, not subtracted | 8.4 → 8.2 | 4.2 → 4.2 |

The changed-count-three saving is real local evidence, replacing H279's
mismatch tax. PE32 VS-F equal saving is only one CPU quantum and the same
size as its count-zero control shift; it is not independently resolved as a
setter-body saving. Companion regressions are broader than VS-F: count-one
equal calls in all four PE32 float/int families add 0.5–0.9 ns, while PE64
count-four PS-int equal adds 0.9 ns. Unchanged bool and zero-count controls
mostly move by 0–0.2 ns. The complete matrix, including all changed/control
cells, remains in the verified report.

These trade-offs prohibit calling this an unconditional PE saving. H278's
VS-F whole-log shape is not a scene-qualified, all-family weighting; warm
setters also omit dirty consumption. Actual GT2 frame/CPU spots are a separate
integration gate, not a way to hide companion costs in an FPS average.

## Wild CPU/FPS spots

One baseline and one candidate run per product use Sikarugir cx-24.0.7,
perf/Warn, frame sampling and the low-rate process CPU observer. Detailed PE
observers are OFF; replay offload is ON; inline constants, segmented transport,
CPU-ready Tape and index reorder are OFF. Resolution is 1024x768 for 05 and
1280x720 for 06. All runs are sequential, with frozen image/patch identities.
06 uses the canonical GUI lane selector and an allowed result-GUI timeout
after the complete scene, not a timed-out scene accepted as completion.

| GT2 spot | Sampled frames / seconds | Sampled FPS | Process CPU ms/frame | CB/frame | Pass/frame |
|---|---:|---:|---:|---:|---:|
| 05 baseline | 1,904 / 64.579 | 29.483 | 70.119 | 4.001 | 15.783 |
| 05 candidate | 1,928 / 64.546 | 29.870 | 69.556 | 4.001 | 15.786 |
| 06 baseline | 1,154 / 64.052 | 18.017 | 95.514 | 4.002 | 19.860 |
| 06 candidate | 1,160 / 64.034 | 18.115 | 95.125 | 4.002 | 19.855 |

`verified-wild.json` rederives these values from immutable frame/CPU artifacts.
CPU covers 96.8–97.0% of each scene's frame increments using one same-PID
cumulative endpoint delta; it includes all process threads, not exclusive PE
time. Eight reported health counters are zero. 06's final counters precede
the last frame by 10/14 frames, so those tails are not error-counter-proven.
No whole-scene pixel equivalence or statistical neutrality is claimed.

The spot deltas are FPS +1.31%/+0.55% and process CPU/frame -0.80%/-0.41%
for 05/06. They are favorable observations, not repeatable or independently
attributed effects. They do not erase the controlled companion regressions or
establish README recovery. The earlier desktop-interrupted 06 result is not
part of this cohort.

## Decision

Do not retain this runtime specialization. It solves H279's mismatch rescan,
but trades that local saving for measured non-three costs and larger code /
PE64 stack state. A single favorable wild pair per product does not establish
the net economics needed to make that trade unconditional. Preserve the
candidate DLLs, patch and full measurements for a future weighted comparison;
retain the additive native pins and contract, not a dormant production branch.
README stays unchanged.

Final restoration rebuilds all three canonical runtime directories and passes
the focused native suites again (arm64 3/3, x86_64 4/4 including install-name
audit). `restored-images.json` compares entire PE files after normalizing only
parsed COFF/debug-directory timestamps: every other byte equals baseline.
Two final restored-image pixel runs again match 192 baseline samples.
`final-audit.json` verifies all five Wine-root renderer images are restored;
no production `src/` or `include/` diff remains. The frozen candidate evidence
is not relabelled as a result of this final restored image.

The next candidate must remove common-path work without repeating this
length-specific dispatch/companion tax, or supply representative all-family
net-economy evidence. Repeating these same warm count-three tests alone cannot
close the remaining gate.
