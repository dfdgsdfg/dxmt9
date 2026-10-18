---
domain: present-pacing
workload: STALKER Day/high late camera corridor
title: "H646 — same-episode owner observation completes, archive retirement remains unknown"
type: evidence
status: population-complete-global-ranking-open
updated: 2026-10-17
source: traces/stkcop-owner-lifecycle-live-h646/{contract.md,batch.json,inputs.json,run.py,analyze.py,owners.py}; traces/stkcop-owner-lifecycle-live-h646/{O0,I0,R0,C0,C1,R1,I1,O1}/{receipt.json,analysis.json,self.bin,post-tail.png,host-before.txt,host-after.txt}; traces/stkcop-owner-composition-h647/verification.json
related: present-pacing-owner-lifecycle-live.645.md; present-pacing-owner-composition.647.md; present-pacing-snapshot-performance.641.md
---

# H646 — original-game population completes without replacing any arm

All eight predeclared arms complete in order `O0 I0 R0 C0 C1 R1 I1 O1`.
H645's clock failure is resolved by staging the fourth, app-local clock DLL;
none of H645's runs is substituted. H646's raw files, scripts and initial
analyses remain frozen. H647 independently rechecks their artifacts and adds
postprocessing without a new game run.

The result strengthens **Replay as the leading measured CPU owner**, but does
not establish a global default-path ranking or a new removable amount. Archive
threads return without an observed join, leaving their true retirement unknown.
There is also substantial temporal drift; the unfavorable C0 is retained.

## Fixed scope and qualification

Original engine/R2, Sikarugir, Day/high, 1280x720, `-max-threads 2`,
`snd_targets 16`, production query-counter mode, draw-command limit 0. Every arm
uses the same H644 coherent Unix images, H639 value audit OFF and H584 sparse
self-CPU endpoints. Camera knots 1 through 6 define the measured interval.

- O: original H640 PE, additional owner observers OFF.
- I: H628 diagnostic PE, root observer OFF.
- R: diagnostic PE with root, worker-lifecycle and foreign-scope observers ON.
- C: same as R, plus H639 snapshot sharing.

Every run reaches seven qualified knots and exits normally. Catalogue status
contains only the predeclared `missing_capture`: its automatic screenshot is
disabled; the independent post-corridor screen exists. Each restores 14 shared
files; each R/C run additionally recovers four temporary clock services.
H647 rechecks image/script hashes, restoration backups, process births,
request epochs, raw stream grammar and staged-image receipts.

Main inspected all eight post-corridor PNGs: the building/interior scene is
visible without an obvious black-screen or repeated-geometry defect. Positions
and animation differ. These single screenshots neither establish exact pixels
nor exclude transient rendering defects. No fresh GPU oracle is claimed.

## Measured worker CPU

Bounds are CPU ms per selected completed frame, not statistical intervals.
The FPS column describes the same short camera interval, not the full benchmark.

| Arm | Frames | Replay | Encode | Selected FPS |
|---|---:|---:|---:|---:|
| O0 | 109 | 33.439–33.597 | 31.811–32.037 | 16.167–16.176 |
| I0 | 102 | 34.005–34.165 | 32.575–32.865 | 14.596–14.600 |
| R0 | 101 | 33.510–33.666 | 31.930–32.380 | 14.241–14.246 |
| C0 | 87 | 34.922–35.130 | 34.047–34.281 | 12.449–12.454 |
| C1 | 90 | 33.517–33.733 | 34.397–34.915 | 13.594–13.602 |
| R1 | 83 | 40.177–40.364 | 38.101–38.553 | 10.598–10.602 |
| I1 | 88 | 40.579–40.787 | 38.317–38.588 | 12.758–12.763 |
| O1 | 92 | 39.545–39.764 | 37.870–38.155 | 13.236–13.242 |

All six O/I/R arms have Replay lower > Encode upper. C0 also does; C1 reverses
that ordering. O0→O1 Replay increases by at least 5.948ms/frame without changing
policy. Therefore this population does not independently reproduce H641's
removal screen, and an R/C contrast cannot be treated as a universal gain.

Before/after host snapshots contain appreciable unrelated CPU activity,
including Orca helpers and system services. These snapshots are outside the
measured corridor and are not a time-integrated attribution of the drift.
Do not blame a specific process, subtract its percentage, or discard C0.

## Same-episode auxiliary coverage

| Arm | Primary root CPU upper | Finish upper | Completion upper |
|---|---:|---:|---:|
| R0 | 19.416 | 1.053 | 0.334 |
| C0 | 23.339 | 1.130 | 0.347 |
| C1 | 22.246 | 1.056 | 0.316 |
| R1 | 22.953 | 1.058 | 0.365 |

Each observed run has 19 worker instances. The bootstrap owners join before
the target; four Library and four Pipeline workers join, each contributing
<0.009ms/frame upper. Each run records two foreign requests, fully disposed
outside the target; their observed request/body/original-disposal overlap is
zero. This does not erase unobserved platform post-release work.

Root maps have two epochs on the same native Primary TID, with no sticky fault;
the inactive first slot is empty. Root upper includes waits, callbacks and
endpoint margins, not just exclusive PE computation. Exhaustive PE entry/thread
coverage still requires audit.

Final ArchiveLoad and ArchiveSave owners have payload-retired events but no
join. H642 must therefore retain an unknown suffix through the target end.
Their bounds are the entire selected wall interval: 70.221, 80.329, 73.561 and
94.356ms/frame respectively. **These are uncertainty bounds, not measured
archive CPU costs.** A body return, payload destruction or eventual process
exit does not prove retirement before the corridor.

## Consequence

H647 composes existing sparse samples into worker lifecycles and closes the
vacuous Replay/Encode lifetime bounds without rerunning the game. Actual archive
thread retirement, effective conditional-worker policy, PE coverage and observer
transfer remain open. H641's separately qualified 5.645–6.670ms/frame Replay
removal evidence is unchanged; neither global ranking nor FPS improvement is
promoted by H646.
