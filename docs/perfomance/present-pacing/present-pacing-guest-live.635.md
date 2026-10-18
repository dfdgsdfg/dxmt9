---
domain: present-pacing
workload: STALKER Day/high late camera corridor, private guest instrumentation
title: "H635 — live guest marker installation and coarse Primary CPU partition"
type: evidence
status: live-installation-qualified-default-transfer-and-removal-open
updated: 2026-10-16
source: traces/stkcop-guest-install-h634/{live.py,live-contract.md,control.cpp,control-verification.json,image_probe.c,analyze_live.py,live-analysis-tests.json}; traces/stkcop-guest-install-h634/live-armed-0/{receipt.json,loaded-images.txt}; traces/stkcop-guest-install-h634/live-armed-1/{receipt.json,analysis.json,guest.bin,image-probe.stdout,progress.jsonl,post-tail.png,catalogue-result.json}; experiments/output/app-d3d9-stkcop-bench-guest-install-h634-live-armed-0/{dxmt9.log,result.json}
related: present-pacing-guest-install.634.md; present-pacing-guest-observer.633.md; present-pacing-owner-union-live.629.md
---

# H635 — the private observer runs through the real late scene

H634's observer now runs in an authenticated private STALKER executable. The
retained successful attempt reaches all seven predeclared camera knots, seals
528 rows covering 132 complete render paths, and exits normally. Post-corridor
screen inspection shows no obvious black-screen/repeated-geometry defect; it
does not establish pixel-exact equivalence or absence of transient defects.

This closes the live installation and event-collection feasibility question.
It does **not** close marker perturbation, default-path cost ownership or a
positive removable CPU amount.

## Scope and identity

Sikarugir, Day/high, 1280x720, `-max-threads 2`, `snd_targets 16`, product perf
observers off, draw command limit 0 and production occlusion counter mode 0.
The existing staged five renderer images are preserved and authenticated;
`--skip-stage` avoids a renderer change. All hashes are in the run receipt.
This is not asserted to be a fresh build of every dirty source change.

The private original-hash-qualified EXE and observer DLL are read back through
a PE helper: module paths, first 512 header bytes and all four patched sites
match the prepared file. macOS vmmap independently identifies the Unix provider.
The native PID/birth/thread and guest thread are separate identities. The map
controller checks native process birth and epoch before atomic one-shot arm
and stop requests; 23 controller cases pass.

The helper observes the H502 camera corridor. Arm occurs at knot 0, stop at
knot 6. No builds, parsers or UI actions occur inside that corridor. Startup
focus and the post-corridor screenshot remain outside. There is no claim that
external camera envelopes prove identical submitted draw work across runs.

## Result and coarse cost partition

| Adjacent sampling boundaries | Intervals | CPU ms/interval | Elapsed ms/interval |
|---|---:|---:|---:|
| Begin → dispatch | 132 | 0.028 | 0.022–0.029 |
| Dispatch → End | 132 | 43.347 | 45.541–45.548 |
| End → continuation, including End tail | 132 | 0.071 | 0.077–0.085 |
| Continuation → next Begin | 131 | 10.347 | 14.767–14.775 |

CPU values are differences between native self-counter samples, not isolated
function-body timings. They include marker work and every nested game, Wine,
PE and synchronous Unix call. Elapsed endpoints include the sample-read
aperture; these ranges are not confidence intervals. Unequal interval counts
must not be casually summed into a frame-latency estimate. Render-path closure
is not an independently identified Present.

The useful next-action evidence is that dispatch dominates these guest
boundaries; Begin and End alone do not explain Primary's CPU amount in this
run. The dispatch CPU cannot be relabelled as dxmt9 CPU. Its game/Wine work
must be excluded with same-episode PE/root-union evidence before comparing
against Replay and Encode. H629's older, differently observed partition numbers
must not be subtracted from this run.

All 528 rows have one qualified native identity, monotone clocks/CPU samples,
valid phase grammar and a clean sealed state. There are no failed Begin or
unpaired continuation rows. Eleven corrupt-artifact cases reject identity,
epoch, capacity, sequence, order, backwards CPU and terminal/tail defects.

## Preserved first attempt

`live-armed-0` initialized the observer, but was stopped before scene collection.
The harness incorrectly demanded PE paths in macOS vmmap; the engine also
reported `OpenAutomate Failed loading: 'daybenchmark.test'`. The private parent
directory did not mirror the original app-root layout. The second attempt adds
links to original root assets and verifies PE identity with the read-only guest
helper. It does not replace or erase the failed attempt. Both runs restored
three clock services, protected images and original shared configuration.

## Completion and open gates

The successful game's return code is 0, with no timeout. Catalogue return code
1 carries only the predeclared `missing_capture`: capture-delay 600 deliberately
suppresses the runner's screenshot; the independent post-corridor screenshot
exists. This administrative exception does not waive game, scene or stream
failure. Shared state is restored after owned Wine processes terminate.

Next fix a matched unmodified/inert/armed population before judging overhead.
For default-owner transfer, connect this coarse Primary boundary to only the
PE root union and sparse Replay/Encode self endpoints; do not re-enable all
H629 detailed replay/encode diagnostics. Separate any game work from PE/Unix
ownership, then test a correctness-qualified removal against the same input.
No positive removable live CPU amount is established here.
