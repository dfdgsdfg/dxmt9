---
domain: present-pacing
workload: STALKER Day high 1280x720 late camera knots1–6; original game
title: "H650 — live archive retirement no longer prevents observed-owner ranking"
type: evidence
status: live-terminal-attribution-passed-global-default-coverage-open
updated: 2026-10-17
source: traces/stkcop-retirement-live-h650/{contract.md,inputs.json,derivation.json,batch.json,run.py,owners.py,analyze.py,verify.py,verification.json}; traces/stkcop-retirement-live-h650/{O0,R0,R1,O1}/{receipt.json,analysis.json,self.bin,post-tail.png}; traces/stkcop-retirement-live-h650/{R0,R1}/{workers.bin,retirement.bin,foreign.bin,gap.bin.196}
related: present-pacing-retirement-owner.649.md; present-pacing-owner-composition.647.md; present-pacing-owner-lifecycle-live.646.md; present-pacing-snapshot-performance.641.md
---

# H650 — actual terminal evidence closes the archive uncertainty

All four prospectively fixed arms O0/R0/R1/O1 finish naturally. Every arm
restores14 shared files; both R arms recover their four temporary clock-service
files. No arm is replaced, no game DLL is patched and no renderer policy is
promoted. The independent verifier rechecks4,095 hashes and reconstructs raw
CPU intervals, lifecycle composition and terminal bounds without rerunning Wine.

O uses the original H640 PE images. R uses H628 PE-root union plus H643 worker,
H644 foreign-block and H649 retained-port observers. All arms use the same
coherent H649 Unix image, H584 sparse self endpoints, audit OFF and H639 sharing
OFF. Original engine/R2, Sikarugir, Day/high1280x720, max-threads2, snd_targets16,
production query counter and camera knots1–6 match the frozen contract.

## Same-episode CPU bounds

Values are milliseconds per completed frame in the selected episode. Intervals
are finite endpoint/selection enclosures, not statistical confidence intervals.
PE-root values are conservative CPU uppers from enclosing wall unions, including
waits and callbacks. Do not add concurrent thread values into frame latency.

| Arm | Frames | Replay CPU | Encode CPU | Observed PE-root upper |
|---|---:|---:|---:|---:|
| O0 |91|38.630903–38.911703|32.197488–32.877399|Not observed|
| R0 |87|38.700892–38.917580|33.436210–33.783433|22.488714|
| R1 |99|34.479861–34.635802|32.792089–33.203635|19.656875|
| O1 |105|34.994315–35.184935|33.328652–33.608652|Not observed|

Replay's lower bound exceeds Encode's upper in all four arms. In both observed
arms it also exceeds every other **observed, composed** native-thread upper.
The nearest competitor remains Encode, with separation4.917459ms/frame in R0
and1.276226ms/frame in R1. This is stronger than H646/H647, where unknown archive
tails could cover the whole episode. It is not yet an exhaustive global/default
ownership certificate.

## Archive tails: unknown wall capacity, not measured work

Each R run registers18 worker instances. Six receive retained-port terminal
observations; existing successful joins remain separate evidence. In particular,
the final ArchiveLoad and ArchiveSave are payload-retired but unjoined. Both
now have identity-qualified dead-name observations after the first sparse
endpoint. No successful join is fabricated.

| Arm | ArchiveLoad CPU upper | ArchiveSave CPU upper | Unknown wall suffix retained |
|---|---:|---:|---|
| R0 |0.117376ms/frame|0.117692ms/frame|10.211750 /10.239167ms total|
| R1 |0.093856ms/frame|0.093952ms/frame|9.291791 /9.301291ms total|

The independent check clips the interval from the first camera request's outer
start through the first qualifying terminal AFTER timestamp. It charges that
entire overlap as possible CPU and proves equality with the envelope's remaining
archive bound. It does not claim the archives actually consumed that CPU, nor
that they terminated at payload return. Only time after terminal observation
is excluded. The previous70–94ms/frame uncertainty in H646 is therefore no
longer a ranking obstacle in these new episodes.

Finish upper is1.119472/1.100778ms/frame and Completion0.356908/0.351960.
Each joined compiler worker is below0.0085ms/frame. Four bootstrap instances
are joined before the interval. Each R run has two completely disposed foreign
requests, no outstanding/unclosed requests, and zero recorded scope overlap
with the selected episode. That last fact covers owned request/body/disposal
scopes, not all subsequent platform-internal work.

The PE map has two attachment epochs on the same native TID. The old epoch has
no activated work; all observed roots are on the current Primary thread.
No other registered PE caller appears. This is dynamic evidence for the
instrumented entry surface, not proof that the entry surface is exhaustive.

## Observer transfer and visuals

Do not infer a fixed observer correction. R0−O0 Replay overlaps zero, while
R1−O1 is negative. Encode increases in the first pair and decreases in the
second. Original O0→O1 Replay also changes substantially. All arms remain in
the record; their differences do not isolate observer overhead or a new saving.

All four post-tail screenshots were inspected. Building/interior geometry is
plausible, without an obvious whole-screen black/repeated-geometry defect.
They are different live frames, not an exact-pixel oracle. H649's independent
original-capture pixel controls remain the deterministic observer check. The
catalogue's expected `missing_capture` status reflects its deliberately late
capture timer; natural exit and separate post-tail screenshots are verified.

## Remaining completion audit

The full objective remains the largest **default-path** dxmt9 single-thread CPU
owner and its removable amount, not merely Replay versus Encode. Remaining
items are effective conditional-worker policy, exhaustive PE/root entry and
teardown coverage, foreign platform attribution, and observer-OFF transfer.
Current source review identifies the explicit gates for parallel dispatch,
watchdog, PE sampler, vertex probe and presenter acquire workers; absence from
the lifecycle stream alone must not be used to exclude them.

H641 remains the separate removable-work experiment: qualified Replay block
reductions5.645–6.029 and6.295–6.670ms/frame for shared base snapshots plus removal
of queue by-value materialization. H650 contains no candidate arm and neither
remeasures nor expands that claim. No FPS gain, production change or global
goal completion follows from H650 alone.
