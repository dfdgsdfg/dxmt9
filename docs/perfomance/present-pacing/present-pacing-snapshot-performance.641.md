---
domain: present-pacing
workload: STALKER Day high late camera corridor
title: "H641 — audit-off snapshot sharing passes the live Replay CPU screen"
type: evidence
status: live-replay-screen-passed-global-owner-open
updated: 2026-10-16
source: traces/stkcop-snapshot-performance-h641/{contract.md,inputs.json,derivation.json,comparison-tests.json,comparison.json,verification.json}; traces/stkcop-snapshot-performance-h641/{S0,S1,O0,N0,N1,O1,N2,O2,O3,N3,S2,S3}/{receipt.json,self.bin,analysis.json,progress.jsonl,post-tail.png,catalogue-result.json}; traces/stkcop-snapshot-live-h640/qualified/build.json
related: present-pacing-snapshot-live.640.md; present-pacing-snapshot-consumer.639.md; present-pacing-minimal-owner.636.md
---

# H641 — repeated live Replay reduction, not yet global attribution

The fixed12-run audit-off population passes the predeclared Replay screen.
Reference-to-candidate block reductions are **5.645–6.029** and
**6.295–6.670 CPU ms/frame**. Both lower bounds are positive; their median
lower5.970 exceeds the largest adjacent-sham bound5.153 by0.818ms/frame.
This is finite same-camera evidence with measurement enclosures, not a
statistical confidence interval or a universal saving bound.

All eight reference episodes put Replay above Encode; all four candidate
episodes put Encode above Replay. This resolves worker ordering for this
population, unlike H636's overlapping bounds. It does not prove the largest
dxmt9 cost across the app thread and every auxiliary thread: those are not
covered by H641. Do not borrow an old PE upper bound from a different episode
to close that gap.

## Controlled change and prerequisites

Every run uses the same H640 private Unix image and four frozen PE images.
H639_SHARE selects original per-run ownership (reference) or qualified shared
base snapshots consumed as borrowed views by the queue (candidate). No rebuild
or product source change occurs. The treatment includes both shared ownership
and removal of queue by-value materialization; this experiment does not split
their individual contributions.

H638/H639 provide bounded ownership refinement, native final-SoA/query-fault
and Device fail-stop tests, and original-capture exact pixels/query/lifetimes.
H640 adds original-game actual-value checks. In H641, H640_AUDIT and
H639_WITNESS are unset, product/detail observers are off, and logging is warn.
Only H584's sparse Replay/Encode CPU endpoints remain. H640 Q1 authenticates
the same binaries' startup ABI handshake with Info logging; the performance
arms do not incorrectly require that Info line at warn level.

Original game/renderer bytes, Sikarugir, Day/high1280x720, max-threads2,
snd_targets16, draw limit0 and production-counter occlusion0 are fixed. The
order was frozen before collection: S0,S1,O0,N0,N1,O1,N2,O2,O3,N3,S2,S3.
S/O are reference and N is candidate. The treatment blocks reverse order;
reference shams bracket them. No failed run was replaced, no outlier removed,
and the population was not extended based on results.

## Every measured episode

CPU milliseconds per completed frame between camera knots1 and6. Bounds retain
sample aperture, controller delay and2us endpoint allowance. CPU from concurrent
workers must not be summed as frame latency. FPS is the selected corridor,
not the whole-Day benchmark.

| Run | Frames | Replay CPU/frame | Encode CPU/frame | Corridor FPS |
|---|---:|---:|---:|---:|
| S0 | 94 | 38.312–38.784 | 33.864–34.486 | 14.160–14.170 |
| S1 | 106 | 34.500–34.703 | 32.684–32.999 | 15.515–15.521 |
| O0 | 97 | 38.622–38.820 | 33.471–33.743 | 13.554–13.560 |
| N0 | 113 | 28.307–28.448 | 32.468–32.872 | 16.937–16.943 |
| N1 | 96 | 33.704–33.919 | 34.032–34.318 | 13.871–13.878 |
| O1 | 101 | 35.035–35.248 | 33.666–33.887 | 14.940–14.949 |
| N2 | 91 | 32.002–32.212 | 33.572–33.882 | 13.013–13.017 |
| O2 | 103 | 34.410–34.592 | 32.903–33.280 | 14.686–14.692 |
| O3 | 93 | 39.136–39.337 | 33.975–34.799 | 13.708–13.714 |
| N3 | 109 | 28.586–28.744 | 32.743–33.075 | 16.021–16.028 |
| S2 | 93 | 39.248–39.496 | 33.707–34.853 | 13.969–13.976 |
| S3 | 104 | 34.344–34.509 | 33.003–33.171 | 15.369–15.375 |

Descriptive means of interval midpoints for the four O and four N episodes:
Replay36.900→30.740ms/frame, a6.160ms/frame or16.69% difference. Encode is
33.715→33.370ms/frame. The descriptive percentage is not a guaranteed product
speedup or an FPS improvement.

| Comparison | Replay reduction | Encode reduction |
|---|---:|---:|
| O0/N0/N1/O1 block | 5.645–6.029 | −0.027–0.565 |
| N2/O2/O3/N3 block | 6.295–6.670 | −0.039–0.882 |
| Largest absolute adjacent-sham bound | 5.153 | 1.849 |

Encode block intervals contain zero, so this population establishes neither
an Encode reduction nor a clear downstream increase. Every reference's Replay
lower exceeds its Encode upper; the smallest separation is1.130ms/frame.
Every candidate reverses that ordering; N1's separation is only0.113ms/frame.
Neither relation alone establishes the end-to-end supply-limiting stage.

Camera agreement does not fix AI, draw count, geometry or GPU work. The two
sham pairs both have a substantial first-to-second difference; balanced
four-run blocks limit simple order/parity imbalance but do not eliminate all
time-varying workload confounds. The screen's margin over observed sham is
only0.818ms/frame. Retain this limitation rather than treating5.970 as a
noise-subtracted guaranteed saving. FPS ranges overlap and no FPS promotion
gate was established or passed.

## Integrity and remaining scope

All12 executions reach seven active camera knots and natural game exit0.
Computer-use is limited to startup focus and post-tail screenshots; no builds,
parsers or UI operations occur in the selected corridor. All twelve images
were inspected without an obvious black/repeated-geometry defect. This is not
live pixel-exact or transient-defect coverage. The catalogue's sole
predeclared `missing_capture` is replaced by the supervisor screenshot.

An independent verifier re-derives CPU enclosures and block/sham arithmetic
from48 raw worker samples, reconciles84 camera endpoints and464 hashes, and
checks every14-file restoration. No Wine/source-probe process remains. All
evidence, including slower runs and request delays, is retained.

The demonstrated removal candidate is repeated base-snapshot ownership and
materialization in compatibility Replay. Private captured-input and live CPU
screens now agree on its usefulness. The full objective still needs same-
episode app-thread PE/synchronous-Unix and auxiliary-thread coverage to assert
a global dxmt9 maximum. Full generation-writer/Arena integration and production
promotion are separate gaps. No commit, default promotion or README FPS update
is made, and no further run is appended to this frozen population.
