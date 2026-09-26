---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H382: Mark-ticket wait overlaps encode submission, with partial holder coverage"
type: evidence
status: partial-holder-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-app-waits-h382/{run.py,summarize.py}; traces/app-d3d9-stkcop-app-waits-h382/retry/{receipt.jsonl,supply.csv,app-waits.json,summary.json,supply-analysis.json}; experiments/output/app-d3d9-stkcop-bench-app-waits-h382-retry/{result.json,dxmt9.log}
related: present-pacing-controlled-app-owner.381.md
---

# H382: A concrete encode-side holder, not a complete contention explanation

## Qualification

Day/high 1280x720, `-max-threads 2`, `snd_targets 16`, perf profile.
The new bounded supply observer records actual mutex addresses, holder TIDs
and disjoint held-body segments at existing lifecycle unlock/CV boundaries.
It does not wrap a function containing a wait and call that whole span a hold.
StateBlock wait attempts bind to replay sequence only after FIFO adoption.
FIFO, capacity, scheduling and resource lifetime policies are unchanged.

The first launch failed before the app started because llvm-mingw was absent
from PATH; its runner log is retained in the parent trace directory. The retry
uses rebuilt PE and Unix artifacts recorded in `result.json`. SDKROOT and
the Xcode compiler/linker plus LLVM archiver were explicitly selected after
the shell's SDK/PATH mismatch caused build failures. No build/test ran during
the app measurement. No guest suspend or native stack sampler was used.
Computer Use inspected the scene and restored its window before the selected
window; this run is **diagnostic, not an observer-OFF FPS comparison**. One
scene screenshot is not full temporal/visual or audio certification.

The app reports PASS, no failures and zero GPU command-buffer errors. The
trace closes without overflow or incomplete writers. Presents 2702–2718
provide 16 intervals over 625.955ms; every selected frame has scene draws.

## Wait and holder results

| Interior-window observation | Result |
|---|---:|
| Complete mark-ticket acquisition attempts | 1,372 |
| Acquisition elapsed sum | 17.113ms (1.070ms/Present) |
| Acquisition thread CPU sum | 2.224ms (0.139ms/Present) |
| Same-mutex, other-thread measured holder overlap | 1.739ms |
| Encode submit held-body overlap | 1.604ms |
| Encode dequeue held-body overlap | 0.135ms |
| StateBlock queue-space waits | 0 |

These are observer-inclusive acquisition attempts, including uncontended
calls, not 1,372 proven blocking waits. Only 10.16% of acquisition elapsed is
covered by measured other-thread held bodies. The remaining 15.374ms is
**unattributed**, not proved scheduler delay or resource marking CPU. Several
CommandQueue critical sections remain uninstrumented; segments intentionally
exclude the gap to actual unlock and reacquisition scheduling. Nested holder
coverage is unioned, not added twice. No complete attempt/holder endpoints
were missing in the full trace.

The longest attempt is 0.677ms, original mark ticket 6432. Of that, 0.284ms
overlaps the encode thread's `run_encode_loop/submit` hold containing
`EncodeEnd(6430)` and `Submit(6430)`; subsequent dequeue segments bring measured
overlap to 0.319ms. Thus the same-mutex holder is directly observed, rather than
inferred from unrelated publication timestamps. Ticket 6432 is not a raw ID.

`src/dxmt9/dxmt9_queue.cpp`, `runEncodeIteration` calls `enqueueSubmission`
while holding the submission mutex. That reaches `submit`, including the
command-buffer commit path. This names a concrete next split point but does
not prove the driver commit itself owns all 1.604ms. Pending-completion
bookkeeping and submit preparation remain inside the same measured body.

No queue-space wait occurred in this narrow window. That does not invalidate
H381's StateBlock backpressure samples from a different, longer window; it
means the burst was not reproduced here.

## Supply context and decision

App/replay/encode CPU envelopes are respectively 37.055 / 20.356 / 22.761ms
per Present, with different endpoint coverage and nested scopes. They must
not be added. Replay queue is empty for 51.42% of the interior window; replay
input wait is 18.206ms/Present, and encode unpublished-input gap is
15.672ms/Present. Average upstream starvation still coexists with contention.

Next split the remaining acquisition time using additional uninterrupted
CommandQueue held scopes, and split encode submission into preparation,
pending-completion locking and driver commit. Separately select a window
that actually reproduces StateBlock saturation. Do not increase queue
capacity or move commit outside the lock solely from this partial evidence.

Validation: native and x86_64 replay-offload/supply-trace tests passed,
Python analyzer suite passed (33 tests), Unix install-name audit passed and
the TLA verification target passed. A new analyzer regression test ensures
`queue_mutex_*` events cannot be mistaken for replay occupancy `queue_*` rows.
