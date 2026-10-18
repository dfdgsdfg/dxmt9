---
domain: present-pacing
workload: STALKER Day default-policy late camera interval
title: "H622 — actual-live Encode draw-loop decomposition"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-live-detail-h622/{contract.md,live-contract.md,build.json,source-verified.json,native-tests.json,detail-tests.json,preflight-verified.json,process-exit-tests.json,inputs.json,verification.json}; traces/stkcop-encode-live-detail-h622/{O0,N0,N1,O1}/{receipt.json,analysis.json,attribution.json,post-tail.png}
related: present-pacing-owner-composition.621.md; present-pacing-encode-phases.610.md; present-pacing-encode-setup.611.md; present-pacing-encode-issue.612.md; present-pacing-encode-census.613.md
---

# H622 — setup, index/issue and caller preparation share the live loop

The actual-live loop is not explained by one narrow operation. Setup,
index/issue and the caller-loop remainder are substantial overlapping CPU
bounds. Setup and index/issue each exceed argument/uniform and stream-prep
upper bounds in both detailed runs, but neither separates from the other,
the caller remainder, or the separate pass-management region.

Detailed observation is intrusive: finite ON-minus-OFF Encode contrasts are
+2.076 to +4.126ms/frame, against a 1.455ms/frame OFF envelope. These are
observed differences, not a universal observer-tax correction. No removable
CPU budget, global default owner, optimization or FPS gain is established.

## Qualification and fixed population

The private image retains H602 default provider/runtime semantics and canonical
PE binaries. Exactly three runtime members carry instrumentation: queue root,
chunk phases and draw phases. H610's reversible draw instrumentation is combined
with H613's 32,768-root capacity, a distinct H622 namespace/magic and 640-byte
rows. All arms observe coarse roots; only N enables per-draw detail. CPU clocks
remain at root boundaries; detailed clocks measure wall, not per-phase CPU.

Both native architectures pass 12 controls and 22 corrupt-trace rejections;
four detail-switch controls pass. Source transformations reverse to their
authenticated parents/current draw source. Install-name and image checks pass.
Original H502 input OFF/ON preserves source/output SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
670 created/released wrappers, seven mapped sources and the 9/5/4/5 query
transcript. Both ordinary-queue controls pass. Original detail ON observes
3,982 encodeDraw entries; OFF observes zero detail entries, with the same
757 DrawRuns and 761 loop scopes. No new rendering transition is introduced.

Fixed O0,N0,N1,O1; no replacement runs. Sikarugir Day/high, 1280x720,
`-max-threads 2`, `snd_targets=16`, product perf OFF, default command limit 0
and production-counter occlusion 0. All use the same Unix SHA-256:
`cd666f1326f7f3be12e2ad1227f761569d5127a90df14ac7aa7036296162d1e7`.
The selected interval is camera knot 1 through 6, not the complete benchmark.
Every run reaches all seven active camera knots, exits naturally with game
return code 0 and restores all 14 files. The catalogue's only failure is the
predeclared missing capture scheduled outside the experiment. All four
post-tail images were viewed and are visually coherent; these images do not
prove every live frame is pixel-exact.

Metal-toolchain qualification and original GPU oracles follow the Metal
debugging workflow. Computer-use is limited to startup focus and post-tail
screenshots. No concurrent builds, parsers or other experimental workload run
during selected intervals. Independent raw decoding in `verify.py` checks
46,891 Encode roots, self-CPU endpoint arithmetic, exclusive conservation,
loop/encodeDraw nesting, identity/thread consistency, detail-OFF absence,
detail-ON entry counts and 56 file restorations.

## Whole-thread and loop CPU

CPU bounds in milliseconds per completed frame; not confidence intervals.
The loop is part of Encode and must not be added to the Encode total. Replay
and Encode overlap and must not be summed as frame latency.

| Run | Frames | Replay CPU | Encode CPU | Inclusive draw-loop CPU | Tail FPS |
|---|---:|---:|---:|---:|---:|
| O0 | 124 | 29.344–29.481 | 30.140–30.374 | 18.247–19.857 | 18.378–18.385 |
| N0 | 121 | 30.507–30.628 | 33.942–34.266 | 21.651–23.388 | 18.118–18.126 |
| N1 | 120 | 30.316–30.472 | 33.671–34.044 | 21.625–23.250 | 17.249–17.259 |
| O1 | 112 | 31.035–31.219 | 31.199–31.595 | 18.825–20.308 | 16.535–16.542 |

The final OFF FPS is below both ON values despite lower Encode CPU. This is
another reason not to interpret FPS or concurrent worker totals as a direct
phase-cost experiment. Replay's finite N-minus-O contrast spans
−0.903 to +1.284ms/frame, within its 1.875ms/frame OFF envelope. Whole-loop
contrast is +1.317 to +5.141ms/frame; its OFF envelope is 2.061ms/frame.

## Detailed exclusive regions

| Region | N0 CPU ms/frame | N1 CPU ms/frame |
|---|---:|---:|
| Caller loop outside encodeDraw | 3.043–4.606 | 2.978–4.507 |
| encodeDraw scope remainder | 0.148–1.382 | 0.205–1.407 |
| Setup | 3.535–5.116 | 3.505–5.055 |
| Argument/uniform preparation | 1.197–2.723 | 1.191–2.684 |
| Stream preparation | 1.057–2.569 | 1.030–2.515 |
| Fixed-function vertex region | 0–0.194 | 0.003–0.198 |
| Vertex binding | 0.006–0.623 | 0.016–0.620 |
| Base-state phase | 0.269–1.200 | 0.324–1.239 |
| Index/issue | 3.443–5.022 | 3.551–5.103 |
| Tail | 0–0.032 | 0.0003–0.030 |
| Separate pass management, outside loop | 3.530–5.064 | 3.559–5.062 |

The N0/N1 contained populations include 494,246 / 490,136 encodeDraw calls
across 90,985 / 89,125 caller-loop scopes. Selected-window root counts are
769 / 727 plus two boundary roots each. Root complement bounds account for
possible off-CPU time; whole-loop bounds are recomputed as a union, not by
adding child lower bounds. Unassigned Encode budgets are 0.618–1.242 and
0.470–1.021ms/frame.

Names are code regions, not pure categories. In
`src/dxmt9/dxmt9_draw_encoder_draw.mm`, `encodeDraw`, Setup includes diagnostic
checks, parameter/classification work, declaration decode, and conditional
pipeline/depth-state lookup, labeling and binding. The later phase called
BaseState is not the only location performing base-state work. Index/issue
contains both preparation and actual recorded Metal draws; it is not pure
driver time. Caller-loop remainder in
`src/dxmt9/dxmt9_draw_encoder_chunk.mm`, `encodeChunk`, includes binding
projection and transition preparation before calling encodeDraw.

## Decision and remaining gate

Do not promote an optimization from these observations. H611/H612's cold
capture declaration/label/draw-call proportions do not transfer by multiplying
live draw counts. H621's Primary/auxiliary exclusions also do not transfer to
this population, which does not observe those bodies.

The next bounded question is whether Setup is primarily per-draw invariant
preparation or the conditional `!effectiveSkipBaseStateBind` lookup/bind path.
Separate those two owners with a smaller observer before introducing reuse.
The caller already skips base binding within a DrawRun, so adding another
cache without measuring actual misses would duplicate an existing mechanism.
A reusable immutable setup must qualify declaration/FVF, effective stream
stride, shader kind, binding override and diagnostic-probe dependencies; PSO
or VS identity alone is insufficient. Preserve labels and fallback behavior.

In parallel conceptually, IndexIssue needs actual draw emission separated
from surrounding preparation on live input, not a repeat of the captured
ranking. This is a distinct owner question, not authorization for concurrent
timing runs. Failed scratch/raster/VS-shadow/owned-run/fixed-projection trials
remain closed. Actual removal still requires a frozen same-input candidate
screen, GPU equality and observer-OFF live validation.
