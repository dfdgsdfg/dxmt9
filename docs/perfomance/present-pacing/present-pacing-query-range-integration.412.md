---
domain: present-pacing
workload: native 32x32 query DrawRun/Clear and 16-draw eight-query burst
title: "H412: Query ranges through the actual queue and Metal consumer"
type: evidence
status: native-qualified-default-off
updated: 2026-09-29
source: traces/query-ranges-h412/qualified-testlog.txt; traces/query-ranges-h412/tla.log; traces/query-ranges-h412/binary-sha256.txt; traces/query-ranges-h412/final-bench-a1.log; traces/query-ranges-h412/final-bench-a2.log; traces/query-ranges-h412/final-bench-a3.log; traces/query-ranges-h412/final-bench-b1.log; traces/query-ranges-h412/final-bench-b2.log; traces/query-ranges-h412/final-diag-a1.log; traces/query-ranges-h412/final-diag-a2.log; traces/query-ranges-h412/final-diag-b1.log; traces/query-ranges-h412/final-diag-b2.log
related: present-pacing-query-range-clear-transaction.411.md; ../../../specs/backend/query-scope-ranges.md
---

# H412: Working bounded queue/Metal path, not another metadata-only fixture

Based on `ec64e452`, this change connects the existing range owner to the actual
compatibility DrawRun appender, CpuReadyTape publication, END/FLUSH polling and
Metal visibility consumer. `DXMT9_QUERY_SCOPE_RANGES=1` is experimental and
requires `DXMT9_METAL_OCCLUSION=1` plus asynchronous fences. Defaults and PE ABI
are unchanged. No Wine binaries were staged or wild FPS measured in this step.

## Implementation and evidence

- `appendQueryRangeDraw` reserves all DrawRun/state/uniform/byte/lookup physical
  storage before mutating lookup links or committing logical records. It reuses
  the real batch reservation, uniform interning and SoA append code. Failed
  preparation rolls metadata back and retains only accounted physical capacity.
  This is not rollback of external resource-marking effects: the queue still
  owns its ordinary segment admission and fail-stop boundary.
- Membership changes freeze command prefixes without publishing. A spare
  scope/range entry allows a complete suffix to seal under capacity pressure.
  ENDs share the source's marker; FLUSH publishes the matching writer without
  waiting for GPU completion. Binding occurs after actual Tape publication,
  never using a predicted sequence. Existing deferred ordinary markers are
  published before range adoption, not overwritten.
- The range receipt aliases the retained EndState marker, including cancellation.
  Only explicitly cancellable range markers grant immutable snapshot polling;
  the older coalescing marker retains its fallback contract. Backend polling
  retains locked identity, initializer, reset and actual-completion checks.
- Encode selects each command's frozen membership. Membership and END offsets
  preserve pass boundaries; preserving attachment stores and originating-CB
  completion callbacks remain intact. Callback aliases keep the range table
  alive through completion. Direct/Arena, session joining, framegraph DCE/
  reordering and parallel encoding are deliberately excluded.

Focused tests pass **13/13**. DrawRun tests inject **90 allocation failures**
each with uniform dedup ON and OFF, including populated lookup growth. They
check every schema dimension's logical length, lookup chain contents, scope
refs, END offsets, retained physical bytes and successful retry. The existing
4,096 membership cases, 15,625 owner schedules and six Clear failure cases
remain passing. Pure marker tests also pin independent-poll authority and
cancellation after receipt adaptation.

Actual Metal tests, with API validation, run candidate and reference under tile
OFF/force. They check 1024 visible / zero occluded samples, overlap sums,
retained depth, alpha rejection, exact green pixel bytes, blocked-GPU Pending
sentinels, FLUSH without successors, reset/reissue/release and immutable polling
receipts. Seventy query generations exercise bounded table publication/retry.
The candidate also tests an ordinary deferred Event marker before range adoption.

Two integration defects were exposed during verification:

1. Final membership could be empty while earlier commands were queried. The
   source-level PSO prefetch guard then selected forced tile, breaking the alpha
   query result. Publication now supplies a conservative whole-source query
   guard; Encode still uses the actual per-command membership. Both tile modes
   pass after the fix.
2. Immutable poll snapshots intentionally rejected ordinary deferred markers.
   Applying that restriction to the new owner returned NOTAVAILABLE instead of
   Pending. A retained-cancellable marker capability now distinguishes the new
   receipt, without relaxing the old marker policy or dropping cancellation.

The existing TLA suite passes: 65 safety models, five progress configurations,
157 expected failing counterexamples. **No new composed range/queue/callback
refinement model was added**; these are prerequisites, not an end-to-end proof.

## Same-input native experiment

One executable/provider image, Apple M1, Xcode 27, native arm64 debugoptimized
build. Each burst records eight identical pairs of unqueried Draw followed by
BEGIN/Draw/END, then polls all eight results. Resolution is 32x32. Every result
must be exactly 1024 and final readback exactly green. Two warm-ups and 16
measured bursts per process. No perf counters or Metal validation for timing.
Execution order: **A1, A2, B1, B2, A3**; A is source-split reference, B is ranges.

| Run | Synchronous preparation median, µs/burst | Completion-poll wall median, µs/burst | Actual published sources/burst |
|---|---:|---:|---:|
| A1 | 196.958 | 3482.520 | 16 |
| A2 | 255.792 | 3406.165 | 16 |
| B1 | 57.230 | 1305.670 | 1 |
| B2 | 55.624 | 1349.000 | 1 |
| A3 | 218.583 | 3450.585 | 16 |

A/A preparation variance is material. Nevertheless both candidate samples are
below all three controls and publication reduction is asserted, not inferred.
Preparation includes synchronous core/queue work; it is elapsed wall time, not
isolated thread CPU. Completion polling includes worker scheduling and GPU
progress, not just GPU execution. The intervals must not be interpreted as
STALKER frame costs or independent additive CPU stages.

Separate diagnostic runs enable existing perf counters, in A1/B1/B2/A2 order:

| Diagnostic | Publication preparation scope median, µs/burst | EncodeChunk scope median, µs/burst |
|---|---:|---:|
| A1 | 5.874 | 623.105 |
| B1 | 3.021 | 467.333 |
| B2 | 3.062 | 463.000 |
| A2 | 5.855 | 604.626 |

These are existing elapsed scopes read at quiescent iteration boundaries,
including observer cost. Publication's measured scope excludes the new prefix/
seal work before its timer, so it is not the complete publication cost. Encode
scope excludes some outer worker/planning work. Do not subtract these values
from observer-OFF wall times to manufacture an exclusive residual.

Final SHA-256:

- Fixture: `4ea7f78647279e7a3ec2cb0404ff0b11eb9d1117cad7c0f98c1809cb258ae393`
- Native library: `a9e18a2246592196431021e94778adb420260612da0da77134cdd7b34bbaf329`
- Unix provider: `2e00eeb217ea41e16f81d027e10292058fe6183dff9fc16cee956b0974b6597d`

Reproduce with the built `dxmt9-query-occlusion-metal-spec --range-benchmark`,
`DYLD_LIBRARY_PATH=build/src:build/src/winemetal/unix`, Metal occlusion and async
fences ON, CPU-ready tape ON, tile OFF, CB join/coalescing/counters/validation
OFF; change only `DXMT9_QUERY_SCOPE_RANGES=0/1`. Use counters ON only for the
separate diagnostic. The test itself owns warm-up, iteration count, result,
readback and source-count gates.

## Remaining qualification

This proves a useful mechanism on a deterministic native input, **not** the
STALKER twofold gap. Remaining work is composed temporal refinement, complete
all-family/queue fault coverage, strict aggregate ownership accounting for
callback-retained range metadata, x86_64/Wine integration and actual STALKER
END-to-FLUSH burst distribution/capture qualification. Immediate FLUSH per END
has much less publication headroom than this deliberately batched input.
The fixed scope table and bounded scans also need workload cost qualification.
Default promotion and reference removal are not authorized by these results.
