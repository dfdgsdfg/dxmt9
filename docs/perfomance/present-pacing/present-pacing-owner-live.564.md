---
domain: present-pacing
workload: STALKER CoP Day high 1280x720 late spatial window
title: "Current root bound excludes Primary; remaining workers are still unqualified"
type: evidence
status: self-root-qualified-process-bound-rejected
updated: 2026-10-16
source: traces/stkcop-owner-live-h564/{contract.md,build-receipt.json,inputs.json,clocks.json,run.py,analyze_run.py,raw-verification.json,verify_raw.py,qualification.json,qualify.py}; traces/stkcop-owner-live-h564/A0/{receipt.json,analysis.json,self.bin,gap.bin.196,catalogue-result.json,post-tail.png,host-before.txt,host-after.txt,restore/restored.json,recovered.json}; traces/stkcop-process-freshness-h565/result.json
related: present-pacing-owner-domains.563.md; present-pacing-clear-live.559.md; present-pacing-process-freshness.565.md
---

# Current self-thread/root result, not a global default maximum

One new ownership episode completes all seven spatial knots with the
byte-identical H559 A Unix image and H563 explicit-domain root observer.
The game exits 0. Catalogue exit 1 has only the contracted `missing_capture`
administrative failure; this is not a catalogue pass. All 14 shared files are
hash-restored and four temporary services recovered after natural Wine exit.

Whole late interval, knots 1..6, **121 observed completed frames**:

| Owner or envelope | CPU ms/observed completed frame |
|---|---:|
| Replay calling-thread CPU | 28.473–28.880 |
| Encode calling-thread CPU | 29.030–29.261 |
| Whole Primary, including game and Wine | 49.466–50.335 |
| Inclusive Primary D3D9-root CPU upper | 17.866 |

The root upper includes waits, callbacks and 200 ns allowance per root. Its
enclosing cumulative difference covers 1,919,268 roots: 1,777.926 ms elapsed
plus 383.854 ms endpoint allowance. Explicit clock mapping is consistent
throughout the recorded population. No continuous/absolute domain substitution
or uncertainty inflation is used.

Primary's renderer-root upper is below both dedicated workers in the whole
window and all five components. Encode exceeds Replay by at least 0.150
ms/frame in this full-window observation, but component intervals overlap.
This narrow same-run separation does not establish a persistent winner or
erase the close-worker evidence in H534/H559. Thread times overlap and are not
summed as frame latency.

| Component | Replay ms/frame | Encode ms/frame | Root upper ms/frame |
|---|---:|---:|---:|
| 1→2 | 26.330–27.892 | 27.315–28.382 | 17.787 |
| 2→3 | 28.010–29.327 | 28.947–29.799 | 18.153 |
| 3→4 | 29.237–31.130 | 30.226–30.252 | 19.487 |
| 4→5 | 28.879–30.644 | 30.058–30.408 | 18.928 |
| 5→6 | 27.302–28.710 | 27.858–28.872 | 18.066 |

## Process-bound attempt explicitly rejected

The frozen analyzer also computes 202.842 ms/frame as a putative process upper
and 95.873 ms/frame as the residual after subtracting known-thread lower bounds.
**Neither is admitted as an upper bound.** The independent arithmetic check
does not validate the counter's instantaneous-endpoint premise.

[H565](present-pacing-process-freshness.565.md) falsifies that premise for
`getrusage(RUSAGE_SELF)`: active-thread CPU is delivered in delayed bursts,
even when the query runs inside its own process. A 4 us quantization allowance
does not cover that delay. `qualification.json` therefore invalidates all
process/residual bound fields and their ranking predicate while preserving
the raw files and valid independent self-thread/root results. Do not fit a
maximum observed delay or interpret the residual as measured missing work.

## Qualification and next decision

An independent raw decoder recomputes 18 thread intervals and six root
envelopes; it also verifies the six process arithmetic expressions before
their premise is separately rejected. Observer identity, image hashes,
publication, restoration and the full population are retained. The post-tail
screenshot is visually coherent, not a whole-run pixel oracle. Computer-use
only focuses the startup window and captures the post-tail image; no UI action
occurs inside the selected interval.

The observed late cadence is 17.537–17.545 FPS, not an observer-OFF baseline or
promotion result. No new Clear-fusion saving is inferred; H559's finite live
Encode reduction remains its own evidence.

The next useful coverage is cooperative **self-thread** endpoints for the
remaining dxmt9-owned roles: Finish, Completion, pipeline/library compilation,
archive load/save and presenter acquire. Capture thread entry/exit and work
boundaries so idle workers can be bounded without waking or suspending them.
This replaces the rejected aggregate shortcut; it must not exclude a worker
merely because its name or expected work sounds cheap. Shared driver/dispatch
work and observer/default transfer remain explicit scope checks.
