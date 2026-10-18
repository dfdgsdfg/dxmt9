---
domain: present-pacing
workload: STALKER Day/high1280x720 late-window reserve recurrence
title: "Live reserve census rejected: diagnostic abort; durable non-fatal observer required"
type: evidence
status: live-rejected-observer-repair-native-qualified
updated: 2026-10-16
source: traces/stkcop-reserve-live-h587/{contract.md,build.json,preflight.json,run.py,observer.hpp,verify_failure.py,failure-verification.json}; traces/stkcop-reserve-live-h587/O0/{receipt.json,self.bin,stall.sample.txt,restore/restored.json}; traces/stkcop-reserve-recurrence-h588/{contract.md,trace.hpp,native.cpp,test_native.py,native.json}
related: present-pacing-reserve-owner.586.md; present-pacing-replay-census.585.md; present-pacing-sparse-cpu.584.md
---

# Reject H587's live population; do not blame product performance

The first H587 live episode stalled at game frame3535/completed3446, after
spatial knot3 and before knot4. It never reached the full selected interval.
The supervisor timed out, terminated only its identity-qualified engine,
and restored all fourteen shared files. Independent verification checks each
backup/target hash and confirms no remaining Wine/game processes. O1 was not
started. No FPS, warmed reserve cost or whole-thread owner conclusion is valid.

Four exact-image preflight controls had passed: original input observer OFF/ON
and ordinary queue observer OFF/ON. Original readbacks/query transcript and
670 wrapper lifetimes matched; ordinary queue completed sequence2 with API
readbacks. Those controls did not exercise the live observation volume or
Wine's abnormal-exit behavior, so they did not prove the live diagnostic safe.

One explicitly diagnostic one-second stack sample was taken after the stall.
Replay is in `submitDrawRunBatch`'s private observed reserve lambda followed
by libc `abort`; the app waits in `pushStateBlockApply`, while Encode waits
for ready work. This explains the stopped pipeline, not the normal tail's CPU
ownership. Sampling itself also disqualifies this episode as an uninterrupted
performance population.

The sampled return address joins one shared abort call in the exact image.
Disassembly shows the 16,384-row limit, thread-ID failure and libproc extent
checks all branch to that site. Without retained rows or branch-specific
evidence, **which guard fired is undetermined**. Do not claim proven overflow.
The reserve JSON is absent: export depended on a C++ static destructor, which
did not run before the supervisor's forced exit. Sparse mmap evidence and the
failed receipt remain intact. The initial controller request also took roughly
330ms; it is not silently treated as an instantaneous scene endpoint.

## Bounded repair, not a replacement run

H588 implements a separate private observation transport. It does not edit
product sources or change reservation arguments, capacity policy or ownership:

- mmap-backed rows/counters survive `_exit` without destructor export.
- Storage failure, foreign identity and row overflow invalidate evidence,
  but never abort or skip the original rendering call.
- The original call executes once and retains its true/false/exception result.
- All calls contribute count and inner wall duration; only actual capacity
  growth consumes a detailed 23-dimension row. The detail bound is 4,096.
- Per-reservation libproc sampling is removed. Existing H584 sparse worker
  endpoints retain whole-thread CPU; reserve wall is only a CPU upper envelope.
- Initialization occurs before the scene interval. Snapshot/aggregation cost
  remains outside the inner timer but inside the worker, so no zero-overhead
  or unobserved FPS claim follows.

Native tests pass on ARM64 and Rosetta x86_64 in six processes. A two-row
fixture forces four growth observations: two are retained and two dropped,
while all original calls complete. Absent/bad output is non-fatal; false return,
exception propagation, foreign identity and `_exit` durability are checked.
Incomplete/faulted fixtures remain explicitly invalid evidence, not fake passes
of the timing contract. Build logs have no warnings.

[H588](present-pacing-reserve-recurrence.588.md) subsequently passes four
exact-image preflight controls and its two predeclared live diagnostic episodes.
H587 remains rejected, not retried or replaced. H586's cold local CPU result
stands, but warmed ordinary reserve is not a major late owner. The overarching
CPU-owner/removal goal stays active; this diagnostic failure does not establish
a product regression or a blocker.
