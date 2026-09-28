---
domain: present-pacing
workload: bounded query membership fixture
title: "H409: Range admission prototype and deferred END contract"
type: evidence
status: native-prototype-production-and-gpu-open
updated: 2026-09-29
source: src/dxmt9/render/query_scope_range_policy.hpp; tests/native/backend/query_scope_range_spec.cpp; traces/query-ranges-h409/{native-testlog.txt,tla.log}
related: present-pacing-stalker-occlusion-ab.408.md; ../../../specs/backend/query-scope-ranges.md
---

# H409: Model the range boundary before changing the live queue

The [contract](../../../specs/backend/query-scope-ranges.md), R-CORE-8.2(c),
separates immutable membership ranges from physical source publication.
No runtime selector, PE ABI, default or queue/Metal path changes. The candidate
header is consumed only by a new native test, not by production.

`planQueryRangeAppend` checks writer identity, command/range limits, canonical
empty scope and membership generations; it returns unchanged state on refusal.
Equal adjacent membership may extend range metadata, not merge render passes.
`queryRangeMustSeal` distinguishes membership/END from FLUSH/Present/ordered
control/capacity. It is an arithmetic predicate, not an owned pin or an
asynchronous progress implementation.

The important integration constraint is **deferred END as well as ranges**.
Removing scope publication while leaving eager END publication simply moves
the cut. A future implementation needs a logical END offset plus an owned,
unbound EndMarker; actual seal binds once, and FLUSH must seal/request the owner
without waiting for the GPU. Existing initializer and failed-origin obligations
remain unchanged. No sequence may be predicted from a future writer.

## Native results

Focused Meson set passes 3/3: new query-scope-range, existing query-scope-CB and
query-end-marker specs. The new fixture checks 4,096 four-command membership/
failed-origin combinations, all command/range capacity limits from zero to
four, order/coverage, reissue identity separation, stale storage owner, invalid
scope and integer overflow. Actual OcclusionResult accumulation and EndMarker /
classifyEnd helpers are reused. Receipt checks include stale/double binding,
unbound Pending, initializer Pending, terminal precedence and wrong queue.

The fixed burst fixture demonstrates publication limits, not measured speed:
eight query draws, each preceded by an unqueried draw, have 16 reference scope
cuts; FLUSH after every 1/2/8 queries forces 8/4/1 candidate seals. No callbacks,
queue locks, physical allocation or Metal work are priced by this arithmetic.
This fixture cannot prove how much STALKER's actual END/poll pattern can batch.

The first Meson reconfigure failed to find SDK libc++/stdio. Re-running with
`SDKROOT` from `xcrun --sdk macosx --show-sdk-path` succeeds; the focused target
builds without compiler warnings. No compiler/global environment or staged
Wine binaries were changed.

The existing full `verify_tla.sh` inventory completes with exit 0, including
the expected failing counterexamples. This rechecks existing contracts; no new
TLA range/progress composition model was added, so this is not proof of the
future integrated range path.

## Remaining gates

Not implemented: frozen scope-table/retention owner, logical END offsets,
atomic physical admission, real FLUSH/queue wiring, range-aware Metal consumer,
range-plus-progress refinement, allocation-fault integration, GPU exact oracle
and same-input production CPU comparison. The value fixture is not proof of
these layers and does not establish an FPS gain. No wild run or commit.
