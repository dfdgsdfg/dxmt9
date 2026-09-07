---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Inline constants after the real PE pixel prerequisite: matched economy spots"
type: evidence
status: measured-not-promoted
updated: 2026-09-08
source: experiments/output/pe-inline-constant-economy-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-inline-economy-c5c16a78-gt2-{off,on}-perf-r2-20260908
related: present-pacing-inline-constant-pixels.265.md; present-pacing-inline-constant-current.258.md; present-pacing-pe-compact-binding-validation.262.md
---

# Inline-constant economy after actual PE pixel checks

## Decision

Keep `DXMT9_PE_INLINE_CONST_DELTA` default-off. The matched current-image
spots retain the small FPS signal seen in
[H258](present-pacing-inline-constant-current.258.md), but neither pair
establishes reduced process CPU consumption. The bounded native/fault/pixel
prerequisites in H263-H265 make further policy work safer; they are not a
substitute for an economy gate or broader rendering coverage.

| Workload | OFF FPS | ON FPS | Change | OFF / ON partial CPU ms/frame | CPU change |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 29.787 | 30.230 | +1.49% | 70.182 / 70.349 | +0.24% |
| 06 GT2 | 18.266 | 18.428 | +0.88% | 95.690 / 95.313 | -0.39% |

Each product ran OFF then ON once. The order was not counterbalanced, and
host/thermal isolation was not established. There were no concurrent builds,
tests or second Wine benchmarks during accepted scenes. A 30-second idle
gap followed the 05 OFF completion; 06 also has a long inactive result-GUI
tail. This is not a controlled cooldown protocol or a statistically qualified
improvement/neutrality finding. Historical README 30.1/18.1 uses a different
index-reorder setting; exceeding those numbers in one candidate run is not
production recovery. README and renderer defaults remain unchanged.

## Identity and collection

Measurement HEAD is `c5c16a78`. All four accepted runs use identical five-image
receipts. Current PE32 SHA-256 is
`f42a07b926b26a2502bb0f2fde5c8ca62a3c8119bcf8d26532ebf69269cbda0c`;
PE64 is `3851d51f5bdbf2ee177bee99b7212982104ffc0b8bd22dce766c933c235019fd`.
Both were relinked after restoration of H265's temporary replay mutation;
no production source diff remains. Unix is byte-identical to H265's restored
`e4543457d6992f1a2a8a088a590e75170360d8938e4c4aeda582ab3b1da4a09a`.
Both bridge DLLs retain the previous identity. The per-run recipes preserve
complete hashes, sizes and environment, and post-run checks compare both
staging sources and actual Wine-root images with those frozen identities.

Sikarugir cx-24.0.7 runs through the canonical catalogue runner, GT2 named
lane, perf/Warn, 1024x768 for 05 and 1280x720 for 06. Replay offload and frame
sampling are ON. CPU-ready Tape, segmented transport and opaque index reorder
are explicitly OFF. Detailed PE statistics, ledger, phase timers, module map
and stopped-PC sampling are OFF. Only inline OFF/ON differs within each pair.
A read-only PID-qualified process CPU/RSS observer polls every two seconds.
`run_economy.py` and `comparison.json` in the analysis root preserve collection,
acceptance checks and exact arithmetic.

The first 05 preparation attempt failed before Wine launch because an
installer-triggered incremental build could not find the cross compiler.
Its `r1` recipe/log is retained but excluded. Both canonical PE builds were
completed and the compiler PATH corrected before fresh `r2` collection. The
failed preparation is not a failed graphics run or part of the FPS population.

## Populations and locality

| Run | Scene frames | Frame range | Scene wall ms | CPU-joined frame increments | CPU interval seconds |
|---|---:|---|---:|---:|---:|
| 05 OFF | 1,925 | 1-1,925 | 64,625.702 | 1,868 | 61.136 |
| 05 ON | 1,953 | 1-1,953 | 64,604.548 | 1,892 | 61.117 |
| 06 OFF | 1,171 | 1,010-2,180 | 64,106.822 | 1,130 | 61.052 |
| 06 ON | 1,180 | 993-2,172 | 64,033.520 | 1,135 | 61.187 |

05 uses every positive-wall frame. 06 additionally requires more than 100
draws to exclude loading, not slow scene frames. FPS is a frame-log scene
average, not process-elapsed FPS or a benchmark-owned `.3dr` score. CPU joins
exclude missing/terminal/unchanged frame intervals and combine all process
threads over different partial populations; they are not PE-exclusive times.

CB/frame remains 4.001 for 05 and 4.002 for 06. Pass/frame is
15.785/15.792 and 19.863/19.857 for OFF/ON. Summed GPU CB times are
1.995/2.011 and 11.946/11.940 ms/frame; these sums are not GPU critical-path
latency. The 05 final record totals normalized by encoded Present decrease
2,698.756 to 1,702.521, about 37%. The analogous 06 snapshot values are
1,603.324 to 1,038.781, but include loading and omit the unreported tail;
they are not scene-only counts. This confirms the record-shape mechanism,
not a proportional CPU saving. Constant values still cross the boundary and
must be applied to draw state.

All four runner results pass with eight retained renderer error counters at
zero. 05 exits normally, with final counter Presents 1,926/1,954 after logged
frames 1,925/1,953. The permitted 06 timeout closes its finished result GUI;
last counter Present 2,160 precedes final logged frames 2,180/2,172, so it does
not prove the unreported tails error-free. ON captures show rendered forest
frames at 05 1:00.37 and 06 0:36.93, not whole-scene pixel equivalence.

## Next decision boundary

Do not turn the removed record count or instrumented admission estimate into
an additive PE CPU budget. Current evidence distinguishes a possible small
critical-path benefit from an unproved total-CPU benefit. Before promotion,
expand the remaining consumer/layout/failure scope in H265 and use matched,
order-counterbalanced repetitions to test the small signal. Separate producer
work from replay/encode and game/Wine time when attributing savings; do not
label the partial all-thread process measurements as PE CPU. Further code
changes should remove measured duplicated work, not add cache/authority
machinery solely to increase a synthetic count improvement.
