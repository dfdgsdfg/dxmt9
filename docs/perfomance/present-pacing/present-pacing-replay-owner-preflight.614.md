---
domain: present-pacing
workload: STALKER H502/r5 fixed original V4 input
title: "H614 — source-qualified Replay observer passes effect controls"
type: experiment
status: preflight-only
updated: 2026-10-16
source: traces/stkcop-replay-owner-h614/{contract.md,build.json,native-tests.json,extra-tests.json,source-verified.json,preflight-verified.json,preflight-initial-rejected.json}; traces/stkcop-replay-owner-h614/original-observer1.bin
related: present-pacing-encode-census.613.md; present-pacing-queue-partition.603.md
---

# H614 — qualify Replay ownership observation before live attribution

The private observer passes native conservation/identity checks and original
pixel/query/lifetime controls. It does not establish live Replay ownership,
removable CPU or an FPS improvement. H613's comparable Replay/Encode totals
remain the reason to investigate Replay outside queue publication.

## Scope and identity

Only three private translation units change relative to the H602 baseline:
`device_c_replay_offload.cpp`, `device_c_chunk_replay.cpp`, and
`dxmt9_command_queue.cpp`. Full source reversal restores all baseline bytes;
archive member comparison verifies exactly those three objects changed.
Product sources and shared staging are untouched by this preflight.

The worker root starts after FIFO pop and the initial endpoint poll, and ends
after ledger settlement, wrapper release and `markReplayDone`, before each of
six success/failure endpoint polls. Dequeue, next-item replacement and final
local destruction remain outside it. Root CPU endpoints bound nested wall
scopes for raw replay, planned routing, resolved replay, record loop, queue
batch, legacy marking, mutation and StateBlock; root remainder is separate.
There are no per-record/draw timers. Inclusive parents are not summed with
children. Synchronous import/resolve before offload is explicitly outside scope.

Identity includes queue owner, lifetime domain, raw ordinal, producer range,
record count, FIFO kind and worker, plus process birth and thread identity.
Pointer equality alone is insufficient. Inactive children do nothing, rather
than classifying legitimate inline/app-thread calls as orphan worker events.

## Verification and corrected source-count assumption

Twelve native controls and 22 parser corruptions pass on arm64/x86_64. Four
additional controls exercise explicit/idempotent root and child finish,
inactive children, disabled observation, and raw/mutation/StateBlock identity.
Ten additional parser corruptions reject invalid kind and producer pairs.

Original observer OFF/ON executions preserve seven mapped final sources,
nine query issues, five polls, four results, five readiness checks, and 670
created/released wrappers. Source and output readbacks both retain SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Ordinary queue OFF/ON controls also pass. Their rootless child scopes do not
create a trace, as required.

The first original-ON qualification incorrectly equated seven mapped final
sources with seven provider raw calls. The trace actually contains 260 complete
inline roots, each with one planned/resolved/record-loop traversal and no
loss, orphan or fault. Final source cuts and provider raw calls are different
units. The rejected receipt is preserved. Qualification reparses the same
pixels/logs/trace; it does not repeat the original execution or select a more
favorable result. The provider adapter has a different entry/retirement boundary
and worker field from the actual FIFO worker, so it is an effects test only.

## Live gate

Do not launch whole-game all-raw census with H614's 32768-row capacity. Raw
call volume is much higher than H613's final Encode source volume, creating
an avoidable overflow risk. H615 restricts observation to the requested late
window, raises bounded storage, and repeats deterministic/effect controls
before its separately frozen OFF/ON population. Neither preflight authorizes
default-cost transfer or removal claims.
