---
domain: present-pacing
workload: STALKER observer preparation, PE32 markers with actual native CPU clock service
title: "H633 — connect guest phase markers to bounded storage and native CPU identity"
type: evidence
status: integrated-synthetic-observer-qualified-live-installation-open
updated: 2026-10-16
source: traces/stkcop-guest-observer-h633/{observer.hpp,shim.S,fixture.S,test.cpp,build.py,run.py,verify.py,verification.json}; traces/stkcop-guest-observer-h633/discovery-fixed/{build.json,run.json,cpu.bin,stdout.txt,stderr.txt}; traces/stkcop-guest-observer-h633/{integrated,diagnosis,loader-diagnosis}/run.json
related: present-pacing-guest-phases.632.md; present-pacing-guest-marker.631.md; present-pacing-clock-bridge.562.md; present-pacing-owner-union-live.629.md
---

# H633 — actual CPU service behind the markers, not yet installed in the game

The private observer now connects all four synthetic guest boundaries to the
existing H562 PE→Unix native CPU service and fixed-capacity storage. Wine passes
512 original/instrumented state comparisons, yielding 1,408 authenticated CPU
rows. Eight injected failure conditions reject attribution. No game is executed
or modified; no default CPU owner or removable cost is established.

## Connected path and scope

The x86 markers save guest GPRs, flags, LastError and x87/SSE state. They install
a clean x87/MXCSR environment before entering the C++ callback, then restore
the guest environment and original control flow. This closes H631/H632's
specific clean-FP-environment gap for this callback. It does not qualify AVX,
arbitrary SEH/unwind, stack exhaustion or a fault inside the logger.

The callback uses a preinitialized service pointer and pre-touched fixed storage
for 16,384 rows. It performs no hot-path formatting, file I/O or allocation.
One designated writer owns the stream. A foreign thread poisons the stream
without writing its rows; reentry, sample/identity failure, order violation and
capacity exhaustion also fail closed. This is not a multi-writer or concurrent
reader protocol. The fixture writes binary artifacts only after each synthetic
path has returned and the writer is quiescent.

Each row retains the complete native CPU packet, guest thread ID, ordinal,
boundary kind and enclosing guest ticks. Native identity is **process ID,
process birth and native thread ID**; the guest TID is a separate value. The
H632 reducer classifies normal render-path closure, Begin failure and unpaired
continuation. A closure is not equated with one Present or complete D3D9 scope.

## Actual results

| Check | Result |
|---|---|
| Normal path, End tail path, failed Begin, inactive bypass × 128 flag patterns | 512 paired machine-state cases pass |
| Begin/Dispatch/End/Continue invocation counts and original return addresses | Pass |
| Hidden EAX dispatcher input and displaced continuation load | Pass |
| Clean FP environment on callback entry; guest state after callback | Pass |
| Native CPU records | 1,408 rows; one native identity; monotonically ordered CPU samples and timestamps |
| Native identity mutation, packet error, backwards CPU counter, reentry | Four rejections, no accepted row |
| Foreign writer, full buffer, missing End, incomplete tail | Four rejections |
| Original synthetic instructions restored | All four sites |
| Cleanup | Four temporary service/discovery images recovered; five renderer and two game images unchanged |

The parser independently authenticates the build/run/raw artifacts, checks all
512 event sequences and 1,408 packets, intersects both clock-domain observations
per packet, and verifies restored shared state. Observed bridge-call wall spans
are 1.250–5.667µs, median 1.541µs; self-counter read brackets are 0.500–1.541µs,
median 0.666µs. These are **fixture observations**, not a bound on total marker
cost: save/restore, callback setup and row publication lie partly outside that
timed bridge interval. They must not be subtracted from future live CPU results.

## Preserved loader failure and correction

The first integration run failed initialization. A diagnostic build localized
it to `LoadLibraryA("h562_clock.dll")`, error 126. A separate loader trace showed
`status=c0000135`, before any CPU packet could be read.

The original H562 fixture executable sat beside the diagnostic DLL; the new
isolated executable directory did not. H629's live runner likewise stages an
app-local discovery image in addition to the Wine-root builtin pair. Adding
the same authenticated PE32 discovery image fixes the lookup while retaining
the root PE32/PE64 DLLs and Unix service. All three failed attempts and their
successful service recoveries are preserved. This was a loader setup defect,
not a CPU-clock failure or an unfavorable timing sample.

## Remaining live gate

The callback/clock/reducer integration is now exercised, rather than merely
tested as separate components. Safe game installation is still absent. A cold
startup boundary must establish loaded image identity and exact patch bytes,
install all sites before the engine can execute them, and roll back any partial
installation. External activation, measurement-window closure and artifact
recovery must also avoid reading this single-writer buffer concurrently.

The next live comparison remains unmodified → inert marker → armed observer
under a fixed population. Initializer and phase timings are not a dxmt9-only
budget: guest callbacks include game and synchronous Wine/Unix work. H629's
default-transfer gap and a correctness-qualified removal intervention remain
necessary to meet the overall largest-owner/removable-CPU objective.
