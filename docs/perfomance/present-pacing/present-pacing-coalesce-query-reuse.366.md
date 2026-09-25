---
domain: present-pacing
workload: STALKER CoP Day fixed optimizer inputs
title: "H366: Reuse graph queries without changing coalescing decisions"
type: evidence
status: qualified-cpu-reduction
updated: 2026-09-25
source: traces/app-d3d9-stkcop-optimizer-h366/final/{result.json,0.csv,1.csv,2.csv,3.csv,0.stderr,1.stderr,2.stderr,3.stderr}; traces/app-d3d9-stkcop-optimizer-h365/qualified/result.json; experiments/output/app-d3d9-stkcop-bench-coalesce-h366/{result.json,daybenchmark.result,actual.png,dxmt9.log}
related: present-pacing-optimizer-fixture.365.md; present-pacing-encode-residual.364.md
---

# H366: The failed-search spike was repeated reachability work

Base `bba48fd8` commits H364/H365 before this implementation. No new merge
policy, command ordering, attachment action, or resource-lifetime rule is
introduced. The optimizer reuses CSR adjacency and lazy reachability bitset
rows within one immutable graph incarnation. A successful merge destroys the
query owner. Cache admission is bounded to 4096 passes and 65536 edges;
oversize input retains exact traversal. Queries are not allocated until an
intervener actually needs dependency classification.

Pass validity/draw-only summaries avoid repeated command scans. Hard-boundary
prefix rejection preserves diagnostic attribution by applying only with
return diagnostics disabled and without an active-seed left endpoint. Query,
lock, and marker flags are not newly treated as barriers. Production return
diagnostics now require `DXMT9_FRAMEGRAPH_RETURN_DIAGNOSTICS=1` as well as perf
counters; ordinary perf collection no longer enables identity-window tracking.

## Same-input native CPU

The four H365 inputs, source boundaries and graph values are unchanged.
The final x86_64 probe uses two warmups and sixteen alternating OFF/ON pairs,
with fresh input copies outside timing for whole and split execution. No
build, test, or Wine run overlaps this measurement.

| Source | H365 OFF optimizer | H366 OFF optimizer | H366 OFF coalesce | H366 ON optimizer |
|---|---:|---:|---:|---:|
| 5877 | 7.775ms | 0.071ms | 0.064ms | 0.923ms |
| 5897 | 27.602ms | 0.133ms | 0.122ms | 3.412ms |
| 5899 | 26.157ms | 0.183ms | 0.118ms | 3.575ms |
| 5901 | 26.129ms | 0.183ms | 0.118ms | 3.606ms |

All measured and warmup graph outputs equal the frozen `bba48fd8` reference,
not merely the candidate's own OFF output. Every named optimizer statistic
also matches the reference for the corresponding diagnostic setting. All
four inputs still merge zero passes. Enabled diagnostic candidate/rejection
counts are unchanged. Lifetime remains about 0–0.002ms and load/store about
0.007–0.065ms. The remaining 0.85–3.42ms ON overhead belongs to the diagnostic,
not the ordinary coalescer.

The historical baseline is a different binary, not a contemporaneous ABBA
FPS comparison. The final receipt authenticates the executable, actually
loaded optimizer dylib and every input before/after the run. Probe SHA-256:
`b7b9c81d29f32fae0996817307536ea42881250fe476e58c6963b31d170b2748`;
optimizer dylib:
`08fd47687150e7eefec316158962d001d2fc7c7ded1b7046e899a173c9a9b0bf`.
The earlier H366 `qualified/` screening predates the final all-statistic
comparison; `final/` is the primary receipt.

## Refinement and integration

`tools/reference_passcoalesce.cpp` freezes the previous algorithm in the
probe executable only; there is no runtime legacy provider. The bounded
checker calls production `runPassCoalesce`, compares all graph fields,
statistics and active-seed witnesses in 102,414 cases. It covers exhaustive
small DAG/cyclic domains, hard boundaries, semantic flags, commandless and
non-draw passes, repeated merges, bitset word boundaries and oversized-cache
fallback. See [the contract](../../../specs/backend/encode-scheduling/coalesce-query-reuse.md)
for explicit bounds and malformed-input exclusions. Native focused tests
pass 4/4; x86_64 fixture/install-name tests pass 2/2; both full builds pass.

The observer/counters-OFF catalogue Day/high/1280x720 Sikarugir run passes.
Its captured scene shows rendered buildings, road and trees; one screenshot
does not prove every frame. The benchmark-owned result is 3089 frames /
122.728s = **25.169 FPS**, versus H364's historical 24.822 FPS (+1.4%). This
single run does not prove an FPS improvement. The 90–100s window is still
9.30 FPS, with a 661.6ms maximum frame; late stalls remain.

The staged Unix SHA prefix is `829f3d304b314d38`, matching the final runtime
artifact. PE32/PE64 stay `4d30258d6e7d0c3e` / `cae268981dc6cc85`.
Counters are deliberately disabled, so no zero GPU-error counter claim is
made. No new STALKER GPU exact/Tape result is claimed. The optimization is a
graph-output refinement; full workload pixel evidence and paired FPS remain
separate from its demonstrated CPU savings.

## Next gate

Recapture the late slow source after removing this planning spike, and split
remaining encodeDraw/driver work from upstream supply waits. Do not attribute
the entire late slowdown to the old 26ms failed-search cost or to the disabled
return diagnostic. Keep exact output comparison when extending this cache or
changing candidate selection.
