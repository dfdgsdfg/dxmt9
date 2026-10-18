---
domain: present-pacing
workload: STALKER Day/high1280x720 late-window ordinary compatibility reserve
title: "Warmed reserve is not the major late CPU owner"
type: evidence
status: live-recurrence-qualified-major-owner-rejected
updated: 2026-10-16
source: traces/stkcop-reserve-recurrence-h588/{contract.md,trace.hpp,build.json,preflight.json,native.json,parser-tests.json,verify.py,verification.json}; traces/stkcop-reserve-recurrence-h588/{O0,O1}/{receipt.json,self.bin,reserve.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-reserve-live-failure.587.md; present-pacing-reserve-owner.586.md; present-pacing-replay-census.585.md; present-pacing-sparse-cpu.584.md
---

# Cold reserve growth does not recur as a large late-frame cost

Two fixed live diagnostic episodes complete on the same private H584 A image,
with the Clear candidate OFF and unchanged canonical PE images. O1 bounds the
entire ordinary compatibility reserve body at **0.146382ms/frame**, including
a conservative allowance for the unobserved initial scene interval. This
rejects recurring reserve/storage growth as the major explanation of roughly
30ms/frame Replay CPU. It does not price every allocation, Direct reservation,
all Slot construction, or an implemented optimization's removable savings.

## Protocol and qualification

H587 remains a rejected population. H588 uses a separate non-fatal mmap-backed
observer: count every call, retain 23-vector detail only when capacity grows,
and invalidate evidence on drop/fault/in-flight observations without aborting
rendering. The original reserve function executes once, with unchanged return
and exception behavior. No per-reserve libproc CPU read is added. Sparse H584
self-CPU endpoints bracket Replay and Encode; initialization/prefault precedes
the selected interval. Snapshot/aggregation overhead is outside the inner
reserve timer but inside worker CPU. This is not an observer-OFF FPS test.

Native ARM64/Rosetta six-process tests and parser positive/ten negative fixtures
pass. Four exact-image preflights pass: original input OFF/ON retains seven
source cuts, query transcript, 670 wrappers and original source/output BGRX;
ordinary queue OFF/ON retains completion sequence2 and API/readback controls.
Original BGRX SHA-256 is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Private Unix SHA-256 is
`a92636dc2e0cab7504b3c92e04d5560b4a0a37cd5a1554f534840114f296bdf9`.

Both live episodes reach all seven active spatial knots and exit naturally
with engine code0. Catalogue code1 has only the predeclared `missing_capture`
waiver; post-tail screenshots are retained and visually coherent, not an exact
whole-game pixel proof. Fourteen shared files are restored after each run.
Independent `verify.py` decodes raw reserve/sparse binary layouts and recomputes
growth totals and CPU bounds without importing either parser or estimator.
Input/image/report hashes, all 28 backup/target hashes, preflight log markers,
process/birth/thread/domain joins and absence of remaining Wine are verified.

## Observations

| Selected scene quantity | O0 | O1 |
|---|---:|---:|
| Completed-frame delta, knot1 to knot6 | 124 | 121 |
| Replay self CPU bounds, ms/frame | 27.769936–30.435300 | 29.622253–29.745134 |
| Encode self CPU bounds, ms/frame | 28.414036–31.104019 | 29.739596–29.991314 |
| Reserve calls | 33,165 | 33,574 |
| Growing / no-growth calls | 3 / 33,162 | 2 / 33,572 |
| Growth inner wall, total ms | 2.025749 | 0.003375 |
| No-growth inner wall, total ms | 9.423455 | 10.193680 |
| Added vector capacity, bytes | 7,150,848 | 12,800 |
| Live element footprint exposed to growth, bytes | 7,140,216 | 12,696 |
| Observed reserve wall / selected frames, ms/frame | 0.092332 | 0.084273 |
| Missing-start wall allowance, total ms | 321.872250 | 7.515166 |
| Whole-scene reserve CPU upper, ms/frame | 2.688076 | 0.146382 |

O0's slow initial request/ack widens both worker bounds and reserve's scene
upper. Preserve it; do not discard the run or subtract a guessed startup cost.
O1's Replay and Encode bounds narrowly overlap: no unique global Encode owner
is established. These parallel thread times must not be summed into frame cost.

O0 ordinal22712 grows run/PSO/state/layout/debug capacities from672 to1344,
with 671 live rows, taking 2.022666ms inner wall. The other two O0 growth rows
and both O1 rows grow only command headers. O1 header growth takes 1.625/1.750us.
Capacity changes are not malloc counts or measured memcpy bytes. All observer
drop/fault/reject counters are zero. Reserve queue domain2 and caller thread
join the selected Replay endpoint. Writing Slot `seqId=0` is retained as such;
these rows are not submission-qualified identities.

## Decision and next cost owner

Do not introduce a headroom policy based on H586's cold approximately5ms reserve
CPU per captured input. Live warmed ordinary reserve is small, and recurring
state/layout relocation does not explain the late-frame CPU load.

The next bounded target remains H585's approximately9.4ms **elapsed per captured
interval** compatibility-batch residual. Code inspection excludes a misleading
cleanup hypothesis: `DrawSubmitScratch` stores flat snapshots, borrowed spans,
pointer entries, bit vectors and handles; its scoped destructor clears retained
vectors, not nested resource owners. It still belongs to the outer Batch scope,
but must not be described as mass resource destruction.

Separate entry/pair preparation, initial mutex acquisition, binding capture,
segment admission, resource marking, outer append dispatch/argument work and
epilogue. `appendDrawRun` takes `CanonicalDrawState` by value before its inner
Append timer; compatible extensions take it by const reference. Thus that copy
exists only for new states, not all 2,343 appends, and cannot be assigned the
whole residual. Existing shutdown counters are whole-process elapsed totals,
not exclusive armed-window CPU. Require caller CPU alongside disjoint wall
scopes before choosing a removal. No product/default change or global goal
completion follows from H588.
