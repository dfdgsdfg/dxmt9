---
domain: present-pacing
workload: serialized query Metal oracle
title: "H415: Retain range writer across serialized occlusion release"
type: evidence
status: native-activation-verified-default-off
updated: 2026-09-29
source: traces/query-ranges-h415/{native-regression.txt,native-repeat.txt,native-release-final.txt,x64-final.txt,x64-pre-fixup.txt,tla.log}
related: present-pacing-query-range-replay-boundary.414.md; ../../../specs/backend/query-scope-ranges.md
---

# H415: Serialized query ranges now retain their writer

Preceding fixture commit: `2ff845bf`. H414 identified that ordered-control
release published before the range owner saw BEGIN/END. The previous fixture
reproduced that gap without a long STALKER run. This change narrows only the
publication cut; it does not remove ordered release synchronization.

## Implementation

- `src/d3d9/device_c_ordered_control.hpp`, `orderedControlWriter`: validated
  locator, enabled policy, resolved GPU occlusion type, exact BEGIN/END flags.
  Event/Timestamp, invalid flags and other control families retain the full cut.
- `src/d3d9/device_c_chunk_replay.cpp`, `replayResolvedChunk`: derives query
  eligibility from the retained resolved object and owning device.
- `src/dxmt9/dxmt9_session_release.hpp`, `retainWriterForSessionRelease`:
  retention requires DirectObservation/ClosePass plus enabled range policy.
- `src/dxmt9/dxmt9_command_queue.cpp`,
  `releaseCpuReadySessionBeforeOrderedControl`: retains the unpublished writer,
  but posts the same FIFO release, wakes Encode, and waits for acknowledgement
  of the already-published sequence prefix. No completion is inferred from ack.
  Query dispatch records the deferred range boundary; ordinary FLUSH publishes,
  binds the real sequence and requests submission.

No PE wire ABI, completion callback or default policy change. No changes to
Readback/UpdateTexture submission actions. No new runtime knob.

## Evidence

Apple M1, Xcode 27. The real Metal fixture has a populated older source, then
serializes `Draw -> BEGIN -> Draw -> END -> Draw` through production import and
replay. The first and last draws must not contribute to the query.

| Policy | New sources before polling | New sources after query FLUSH completion | Query samples |
|---|---:|---:|---:|
| Ranges OFF | 2 | 2 | 1,024 |
| Ranges ON | 0 | 1 | 1,024 |

Counts exclude setup and the later readback, which can publish the reference's
unqueried suffix. Both policies produce exact green RGBA readback. ON must
return S_FALSE with unchanged bytes before FLUSH, independent of GPU speed.

- Contiguous/segmented × range OFF/ON × offload OFF/ON, repeated three times:
  **24/24**. ON's activation assertion is part of every normal matrix entry,
  not an expected-failure test.
- Event/Timestamp END still publishes its preceding writer; a serialized new
  issue supersedes the old receipt, and sequential reset invalidates its epoch
  without writing output bytes.
- Query/receipt/tile/session/planner focused regression: **28/28**.
- x86_64/Rosetta equivalent regression plus install-name audit: **29/29**.
  The first target-only build omitted unixlib install-name fixup; the loader
  and audit failed while other tests passed. The default `meson compile` target
  repaired the dependencies and the complete set was rerun successfully.
- Pure policy truth tables cover every 8-bit flag for each control family.
  Bounded release/receipt composition enumerates **10,080** event permutations,
  including rejected prefixes, and **49** complete legal schedules. It checks
  prior-prefix coverage, exactly-once acknowledgement, distinct writer sequence,
  initializer completion, terminal dominance, cancellation and stale ownership.
- Existing TLA suite: **65 safety + 5 progress + 157 expected counterexamples**,
  exit zero. These unchanged models remain prerequisites, not a new full
  concurrent queue/range/callback proof.

## Limits and next gate

This is an activation/correctness result, not an FPS or CPU-cost result.
The bounded test assumes fair execution of enabled completion/initializer
events; it does not model arbitrary concurrent reset, all allocation failures,
driver execution, or callback-retained aggregate storage. Range policy remains
OFF by default. Rebuild/restage the complete Wine runtime set, verify STALKER
scene output and nonzero deferred-END/marker observations, then compare clean
observer-OFF runs in both orders. H414's slowdown cannot be reused as a timing
measurement of this changed path.
