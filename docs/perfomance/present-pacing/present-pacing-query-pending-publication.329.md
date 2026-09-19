---
domain: present-pacing
workload: STALKER CoP Day
title: "H329: covered-Pending census and bounded publication stamps"
type: evidence
status: partial
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-covered-pending-off-r654-day/result.json; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r655-day/result.json; experiments/output/app-d3d9-stkcop-bench-covered-pending-off-r654-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r655-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-covered-pending-off-r654-day/actual.png; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r655-day/actual.png
related: present-pacing-query-covered-pending-wild.328.md; present-pacing-query-mutex-owners.327.md
---

# H329: implementation and measurement qualification are separate

## Initial matched-image OFF/ON pair: not promotion evidence

R654/R655 use the five identical staged images from H328, HEAD `9e98c9ed`,
Sikarugir cx24.0.7, Day High, `renderer_r2.5`, 1280x720, perf profile,
validation OFF and health counters ON. Actual occlusion and asynchronous
fences are explicitly ON; END coalescing and all query/receipt/backend/split,
queue-mutex and frame observers are OFF. Covered-Pending is the only launch
policy difference. No build or TLC process ran during the pair.

| Run | Covered Pending | Frames | Reconstructed seconds | FPS | Publication resource mark ms / submitted Present |
|---|---:|---:|---:|---:|---:|
| R654 | OFF | 2,314 | 139.971 | 16.532 | 0.953 |
| R655 | ON | 2,325 | 139.947 | 16.613 | 0.964 |

Both runners return PASS/0, all eight health counters are present and zero,
and staged sizes/hashes match. The nominal +0.49% is **not an established
speedup**. Both `actual.png` files show Orca rather than the benchmark;
foreground residency and visible rendering are unqualified. Neither capture
proves the cause of the lower FPS versus H328. These runs must not update
README or justify default promotion. The late slowdown remains in both result
files; reconstructed time windows are not shared scene identities.

## Implemented investigation boundary

- Backend census includes covered-Pending fast returns and slow returns.
  Previously it silently omitted fast returns. Timing for a fast return does
  not include the already-evaluated atomic eligibility predicate.
- Immutable receipt results join the existing END/publication/encode/submit/
  completion timeline by queue/query/issue/device epoch. No mutable Query
  reread or global replay drain is added. Output values are reported only when
  successfully copied; a status-only poll does not invent result bytes.
- `DXMT9_QUERY_OBSERVER_END_STRIDE` spreads the fixed 128-row END store across
  the run. Sampling omissions, capacity saturation and missing joins must be
  reported. Query time is not a benchmark-frame clock.
- `DXMT9_PUBLISH_RESOURCE_DEDUP=1` enables a fixed 64-entry exact texture/
  surface stamp cache for one locked Slot publication. Full handles and kinds
  are compared. Collisions re-mark. Buffer backing selection, captured-buffer
  flush fences, visitor order, lifetime pins and Arena publication are unchanged.
  The selector remains OFF; this is not yet measured publication-cost reduction.

See [the publication contract](../../../specs/backend/publication-marking.md).

## Verification

Full native, canonical x86_64 Unix (including install-name fixup), PE64 and
PE32 builds pass. The first focused set passes 10/10. After adding the actual
Pool and repeated-clear oracle, the final focused set passes 6/6, and the
x86_64 cache/pending/GPU/install-name set passes 4/4. Full existing TLA passes
in 237.76 seconds. Cache-specific bounded refinement executes 839,808
length-seven traces and checks each prefix, plus max-stamp/reclaim truth
tables. It calls production predicates, not a duplicate policy implementation.

The GPU fixture checks real Pool texture/alias max-stamps, three ordered
same-surface clears at one publication, gated Pending behavior and exact green
readback. It does not exhaust texture/surface retirement and generation races;
the cache relies on the existing locked publication and source-retention
contract. Wild qualification and measured marking reduction remain open.

## R656: partial route evidence; no completed tail attribution

`experiments/output/app-d3d9-stkcop-bench-covered-pending-census-r656-day/`
preserves `result.json`, `run-policy.json`, `dxmt9.log`,
`xrEngine_dxmt9.log` and the loading-screen capture. Covered-Pending is ON,
publication dedup OFF, backend/receipt observers `counts`, query timeline ON
with END stride 512. Runner reports process exit 1 after 344.613 seconds,
`timed_out=false`, no benchmark `.result`. The observed health counters are
zero but do not qualify this failed run. `OA not initialized` also occurs in
both successful controls, so that message alone does not explain failure.

The last bounded periodic census (not a process-final census) contains:

| Backend calls | Covered Pending | Slow path | Pending results | Complete results |
|---:|---:|---:|---:|---:|
| 262,144 | 258,111 | 4,033 | 258,501 | 3,643 |

Thus 98.46% of backend calls in this observed prefix bypass the large queue
mutex; 99.85% of Pending results use coverage. There are 312 new submission
requests and 78 already-submitted requests; none rejected. This proves route
use, not saved wall time or FPS. No terminal timeline report survives, so it
does **not** identify the late slowdown. The observer was subsequently hardened
to emit at most one live timing row on each retained END's first S_OK, retaining
missing-stage bits rather than manufacturing completion. Final summaries remain.

## Final verification and external runtime blocker (2026-09-20)

After live-row persistence and the production immutable-receipt join test,
all four canonical builds pass and the final native focused set passes 8/8
(`build/meson-logs/h329-final.txt`). The receipt fixture's fake backend has no
explicit completion event: it correctly proves first-S_OK identity join while
leaving the result-value/completion evidence unknown.

The final x86_64 rerun cannot start: `Bad CPU type in executable` (errno 86).
This also reproduces with `arch -x86_64 /usr/bin/true` and
`arch -x86_64 /usr/bin/uname -m`, independent of dxmt9. The binaries are
Mach-O x86_64; host is arm64. `oahd` is not running and pkgutil reports no
RosettaUpdateAuto receipt. The reason the host stopped supporting x86_64 is
not established. Earlier x86_64 results above are historical checkpoints,
not a passing final-image rerun. No system installation or service reset was
performed. Restore host x86_64 execution before another Wine measurement.

Pending FPS benefit, publication cost reduction and scene-qualified late-tail
causality remain open. Neither experimental selector is promoted; README is
unchanged.

### Runtime recovery (2026-09-20)

After the user installs Rosetta, `arch -x86_64 /usr/bin/uname -m` returns
`x86_64`, the RosettaUpdateAuto package receipt is present, and the selected
Wine reports `wine-9.0 (SikarugirCX 24.0.7)`. Without rebuilding or changing
runtime code, the final-image x86_64 focused set passes 7/7: publication cache,
publication GPU oracle, covered-Pending GPU oracle, production receipt timeline,
backend observer, timeline observer and install-name audit. Log:
`build-x86_64-builtin/meson-logs/h329-rosetta-restored.txt`.
The host execution blocker is resolved. This does not retrospectively qualify
R654/R655 or complete R656; new matched measurements are still required after
the OS upgrade.
