---
domain: present-pacing
workload: STALKER CoP Day high 1280x720 controlled default, late spatial knots
title: "Thinned self reports leave Replay and Encode worker cost indistinguishable"
type: evidence
status: thin-live-endpoints-qualified-global-owner-unresolved
updated: 2026-10-14
source: traces/stkcop-source-self-thin-h516/{contract.md,live-contract.md,observer.hpp,spec.cpp,test.py,test-receipt.json,diagnostic.patch,build-receipt.json,run.py,inputs.json,clock.cpp,clocks.json,analyze_live.py,analysis.json,verify.py,verification.json,bound-tests.log,restored-tests.log,run1/receipt.json,run1/self.bin,run1/catalogue-result.json,run1/post-tail.png,run1/restore/restored.json,run1/benchmark-results/daybenchmark.result}; traces/stkcop-source-self-live-h515/{bounds.py,test_bounds.py,clock.cpp}; traces/stkcop-source-self-report-h514/{observer.hpp,analyze.py,control.cpp}
related: present-pacing-source-self-live.515.md; present-pacing-source-self-report.514.md; present-pacing-self-counter-boundary.513.md
---

# Remove the reporting-frequency confound before choosing a worker

H515's supposedly coarse observer clocks every Replay item, including frequent
StateBlock work. H516 changes only diagnostic reporting admission: first and
every 256th Replay item, per thread and while requested. Ordinary Encode
endpoints are unchanged. The original H514 header, data schema, self counter,
binding and pair semantics are reused unchanged.

This is **not** sampled-duration extrapolation. The worker's absolute CPU cursor
continues across every unreported item. H515's enclosing-before/after algorithm
subtracts cumulative cursors at the same external scene window and retains the
wider uncertainty. The sampled Replay scope sum is explicitly not a thread
total; multiplying it by 256 would be invalid.

## Verification and live coverage

Each ARM64/Rosetta fixture checks 262,144 pure cursor transitions across all
initial states and wraps. Real-observer tests check skipped metadata factories,
disabled requests, thread-local cadence, closing after deactivation, and
unchanged Encode reporting. Both pass without warnings. Five canonical restored
native tests and six bound tests pass. A raw-row audit independently reproduces
all twelve worker/scene interval bounds.

One new controlled-policy episode uses the same H515 PE images, game image,
scene knots, worker/audio controls and product-observer-OFF policy. Seven
stable active-game knots are retained. Game exit is zero, no timeout; the sole
catalogue `missing_capture` remains administrative, not an unqualified pass.
The separate post-tail image shows no obvious corruption, not an exact
cross-run pixel oracle. This is an observer-density sensitivity comparison,
not a paired renderer optimization or default-transfer campaign.

| Observed quantity | H515, dense | H516, thin |
|---|---:|---:|
| Trace rows | 255,242 | 4,164 |
| Replay endpoint pairs | 126,065 | 499 |
| Ordinary Encode endpoint pairs | 1,555 | 1,582 |
| Missing/lost/faulted rows | 0 | 0 |
| Raw-qualified Encode joins | 0 | 0 |

All 498 successive sampled Replay raw differences are exactly 256. Total row
count falls 98.37%; this is reporting-volume reduction, **not** a CPU or FPS
improvement claim. The runs have different frame counts. All 1,582 Encode raw
ordinals remain zero; scope kind and raw identity are not fabricated from a
queue sequence. Unsupported session/DCE/lookahead paths remain outside coverage.

## Late-window result

Knots 1--6 cover 114 observed completed frames and approximately 6.77 seconds.
The self CPU range includes reporter CPU and inter-scope worker work. It is
not a critical-path percentage, and concurrent worker values must not be summed
into frame latency.

| Worker | CPU / observed completed frame |
|---|---:|
| Replay | 31.4839--31.9530 ms |
| Encode | 31.7751--32.1928 ms |

The full-window bounds **overlap**. All five subinterval comparisons also
overlap. Thus the defensible conclusion is co-sized worker costs near 32 ms,
not a default Replay-over-Encode ordering. H515's small diagnostic ordering is
not robust across reporting density. A single dense/thin pair does not isolate
its exact overhead from workload/run variation.

## Remaining objective and next decision

Neither run measures the mixed Primary thread's dxmt9-root contribution, and
thin reporting has not passed a fresh default OFF/ON transfer campaign. These
are still diagnostic-image worker costs. The largest **global default-path**
dxmt9 single-thread owner and a correctness-qualified removable amount remain
unresolved; there is no optimization to promote from this experiment.

Do not spend another campaign resolving the small Replay/Encode ordering.
The next owner decision needs a low-interference authority for Primary's
renderer-call interior or a sufficient non-renderer lower bound. H477's
all-call clocks and H511's guest-PC-only subtraction remain rejected; any new
method must name its different mechanism and validate coverage/interference
before claiming a default ranking. Removable work still requires a distinct
semantic-preserving mechanism, not the entire measured worker budget.

## Restored state

The temporary three-file product patch and diagnostic images are frozen under
`traces/`. Product sources and both canonical Unix images are restored exactly
to H514's baseline hashes. Fourteen shared Wine/prefix/support/result files
are restored and independently hash-checked. No policy or runtime observer is
promoted; pre-existing dirty work is preserved. No commit or push is made.
