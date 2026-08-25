---
id: present-pacing-bridge-crossing-decomposition.237
title: Present-Pacing #237 - Bridge Crossing Decomposition: Lock/Unlock/Upload/Commit
status: accepted attribution (unlock split measured; core-unlock upload admission is the owner)
date: 2026-08-25
source: experiments/output/app-d3d9-3dmark05-current-bottleneck-gt2-clean-cpu-r2-20260825 (existing-run analysis); experiments/output/app-d3d9-3dmark05-bridge-unlock-split-gt2-r1 (unlock-split run)
---

# Present-Pacing #237 - Bridge Crossing Decomposition: Lock/Unlock/Upload/Commit

## Question

#236 attributed `13.9%` of the saturated GT2 producer thread to `winemetal.dll`
residence and hypothesized — without a function-level decomposition — that it is
"buffer lock/unlock, upload, and `commit_chunk` crossings". Decompose that
residence by call class, and split the one class no instrumentation covers:
the unix side of `dxmt9c_buffer_unlock`.

## Existing-run analysis (no new code)

The #236 clean run already carries the PE-side per-opcode bridge round-trip
timers (`[dxmt9-bridge-perf]` / `[dxmt9-bridge-perf-opcodes]`,
`src/winemetal/winemetal_bridge.cpp` dispatcher span, harvested into
`result.json:dxmt9_bridge_counters`). Normalized to the `2,634` presents at the
final cumulative emission:

| class | calls/present | ms/present | us/call |
|---|---:|---:|---:|
| `dxmt9c_device_commit_chunk` | `11.49` | `1.473` | `128.2` |
| `dxmt9c_buffer_unlock` | `15.32` | `1.259` | `82.1` |
| `dxmt9c_buffer_lock` | `15.32` | `0.420` | `27.4` |
| COM addref/release (buffer/texture/shader) | `~508` | `0.254` | `~0.5` |
| other (factory display-mode polling, surface, lifecycle) | `~164` | `~0.15` | - |
| **bridge total** | `713.7` | `3.558` | - |

Cross-checks inside the same run:

- commit: PE round-trip `1.473` vs unix synchronous body
  (`offload_commit_app_cpu_ms`) `1.359` -> dispatch overhead `~9.9us/call`.
- lock: PE round-trip `0.420` vs unix body (`d3d9_buffer_lock_ms`) `0.335`
  (of which shadow pre-copy `0.082`, low4gb alloc negligible) -> dispatch
  `~5.5us/call`.
- Locked bytes: `5.567 GB/run` (`2.11 MB/present`), `40,360/40,361` locks
  dynamic, `30,200` NOOVERWRITE, `8,952` DISCARD, `0` readonly — the locked
  byte volume is effectively all upload-bound.

The unmeasured residual: `dxmt9c_buffer_unlock` unix side has no timing at all
(`src/d3d9/device_c_resources.cpp:1258-1294`). Its `82.1us/call` PE round-trip
is `3x` lock's, and the two candidate owners cannot be separated:

1. the wow64 shadow->native writeback `memcpy` (`:1266`) — the actual
   "upload" byte motion, expected ~`5.57 GB/run` scale, or
2. `b->obj->unlock(!readOnly)` (`:1281`) — the core unlock, which performs the
   dxmt9-side upload admission for the dirtied range.

## Harness spec

R-237.1: `dxmt9c_buffer_unlock` gains an unconditional perf-counter family
`d3d9_buffer_unlock_*` mirroring `countD3D9BufferLock`'s shape
(calls / cpu ms / max / p50 / p95 / p99), with two child splits that sum
into the parent minus untimed residue:
  - `writeback` (the shadow->native memcpy) with ms, max, and bytes;
  - `core` (the `b->obj->unlock()` call) with ms and max.
Plus classification counts: upload (non-readonly) vs readonly vs
shadow-active calls.

R-237.2: Counter-only observer — zero behavior change to the unlock path,
no new hot-path signature parameters or return values, cost limited to the
same steady_clock reads `countD3D9BufferLock` already pays on lock
(the perf-counter sink's internal gate is the only conditional). Passes
`audit_perf_counter_table.py` and `audit_perf_counter_callsites.py`.

R-237.3: The measurement run reports, per present: PE-observed unlock
round-trip (bridge opcode row) vs unix `d3d9_buffer_unlock_cpu_ms` vs its
writeback/core children, so dispatch overhead and the writeback/core split
are both derivable from one run.

## Method (planned)

GT2, perf profile, no-gputrace, 120s supervised probe with
`DXMT_3DMARK05_ARGS="-gt2 -nosplash -nosysteminfo -noscreens"`,
`DXMT_PERF_COUNTERS=1` (profile default), frame sampling for scene fps
anchoring. Same-build staging; unix provider rebuilt with the new counters;
PE DLLs unchanged (no ABI/schema change).

## Result

Run: `app-d3d9-3dmark05-bridge-unlock-split-gt2-r1` (GT2, perf profile,
no-gputrace, 120s supervised, status pass, `1,714` presents at final counter
harvest; note this window carries more bridge calls per present than the #236
clean run — `1,053` vs `714` — so cross-run per-present rates are not directly
comparable; all splits below are within-run).

Per present, PE-observed round-trip vs unix body vs children:

| class | PE round-trip ms | unix body ms | children (ms) | dispatch us/call |
|---|---:|---:|---|---:|
| `buffer_unlock` (`21.7` calls) | `2.018` | `1.981` | core `1.864` (94.1% of body), writeback memcpy `0.113` (5.7%) | `~1.7` |
| `commit_chunk` (`15.9` calls) | `1.252` | `1.085` | (phase split in `.37`) | `~10.5` |
| `buffer_lock` (`21.7` calls) | `0.673` | `0.499` | shadow pre-copy `0.123`, core (incl. DISCARD zero-fill + mapBuffer) `0.376` | `~7.8` |
| bridge total (`1,053` calls) | `4.587` | - | - | - |

**Verdict: the winemetal.dll bridge residence is owned by the core unlock
upload admission, not by byte motion across the wow64 shadow and not by
crossing overhead.** `Buffer::unlock`'s `b->obj->unlock(!readOnly)` span is
`3,194.6ms/run` = `94.1%` of the unix unlock body (`85.7us/call`); the
shadow->native writeback memcpy is `193.2ms` for `5.38GB` (`~28GB/s`, memcpy-
bound, healthy); the unix-call dispatch overhead is `1.7-10.5us/call` across
the three classes and is not a lever.

Two write-amplification mechanisms are visible in the same run
(`src/d3d9/core_buffer.cpp:56-79`):

1. **Full-buffer uploads.** `Buffer::unlock` takes the exact-range
   `uploadBufferDataRange` path only for Default-pool dynamic NOOVERWRITE
   locks; every other upload sends the whole `storage_`. The managed pool
   alone proves the amplification: `1,224` uploads moved
   `managed_buffer_upload_bytes = 8.93GB` (avg `7.3MB/upload`) from only
   `1,209` managed locks — more than `1.6x` the total locked byte volume of
   all `37,260` locks combined. Default-pool full uploads (DISCARD and plain
   locks) are not byte-counted yet.
2. **DISCARD zero-fill on lock.** `Buffer::lock` under DISCARD does
   `storage_.assign(max(size, desc.size), 0)` — a full-buffer zero write on
   each of the `8,952` DISCARD locks, inside the lock core span.

## Follow-ups

- Split the core unlock span: `uploadBufferData` vs `uploadBufferDataRange`
  vs `unmapBuffer`, with per-path call counts and bytes (default-pool upload
  bytes are the missing volume figure).
- Candidate mechanisms (bounded contracts, not yet licensed): range-limit
  uploads for DISCARD/plain locks (upload only the locked span), skip the
  DISCARD zero-fill and shadow pre-copy (DISCARD's prior content is
  undefined by contract), and managed-pool dirty-range tracking. Each needs
  its own correctness argument before an A/B.
