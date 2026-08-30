---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview"
type: domain-overview
status: current
updated: 2026-08-30
source: docs/perfomance/present-pacing/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/present-pacing/present-pacing-current-bottleneck-pe-symbol.236.md; docs/perfomance/present-pacing/present-pacing-cpu-ready-next-source-intent.239.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `present-pacing.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **CPU/sync side** of GT1 wallclock: why
`completion_wait_ms` (the time the completion handler thread spends in
`MTLCommandBuffer.waitUntilCompleted()`) reached 28-31 s while
`gpu_command_buffer_time_ms` was only ~4 s, where that gap actually lives,
and which production-safe knobs can recover the slack without breaking
visual sync.

Every finding here moves *wallclock fps* directly, not GPU frame time. The
GPU frame-time story is owned by [hidden-backend-storage](../hidden-backend-storage/index.md) /
[index-cache-locality](../index-cache-locality/index.md); the per-CB encode story by [state-churn-encode](../state-churn-encode/index.md).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H211 | Readonly cache stacks with the promoted pair | accepted FPS promotion | [present-pacing-readonly-cache-stacking.198](present-pacing-readonly-cache-stacking.198.md) corrects H197's config-confounded reading (the probe wrapper pinned the promoted pair off, so the cache alone was already an offload-class `+11.2%`), then stacks all three mechanisms in one run: **`2,271` presents** (`+26.2%` cumulative vs `1,800`), zero GPU errors, `54.4` locks/present, offload replay `8.961ms`, `169.5` reordered hits/present. Worker idle drops `44.3 -> 39.0ms/present` — still producer-bound. |
| H212 | Post-cache producer re-sample: the wall is now the guest blob | accepted attribution | [present-pacing-postcache-resample.199](present-pacing-postcache-resample.199.md) re-samples promoted HEAD (`2,220` presents, trio live): guest blob `73.5%` (`~36ms/present`), wow64 layer `19.8%` (`~9.8ms` — the remainder is the game's own win32/syscall traffic, not dxmt9 calls), winemetal unix `1.4%`. dxmt9-named producer cost is `~3ms/present` of a `~49ms` wall; the only remaining dxmt9 CPU question is the PE `d3d9.dll` share inside the guest blob. |
| H213 | Decimated PE stats size the recorder core at ~8.5ms/present | accepted attribution | [present-pacing-decimated-pe-stats.200](present-pacing-decimated-pe-stats.200.md) verifies the perturbation-free `DXMT9_PE_STATS_DECIMATION` instrument: recorder core `8.5-8.9ms/present` = `16-18%` of the producer wall, with the const chain at `~5.1-5.6ms/present` giving an inline-const-delta wire change a `~+10%` FPS ceiling. |
| H214 | Inline const delta proves mechanism but lands inside the noise band | accepted mechanism confirm; FPS below gate; kept opt-in | [present-pacing-inline-const-delta.201](present-pacing-inline-const-delta.201.md) lands R-BACK-2.52 (`DXMT9_PE_INLINE_CONST_DELTA`, ABI v2): mechanism exact (append events `1,377 -> 743`/present, flush `4,456 -> 56`, recorder core `-2.8ms` measured), off-path tax nil, but FPS `+1.6%` — inside the noise band. Second instance of the H193 pattern: Rosetta-measured PE CPU-ms over-credits wall value by `~3x`. Kept opt-in default-off. |
| H215 | Archive prewarm hardening closes the startup-flake class | accepted reliability fix | [present-pacing-archive-prewarm-hardening.202](present-pacing-archive-prewarm-hardening.202.md) implements R-BACK-3.9..3.11 (`30bee79b`): async full prewarm with compile fallback, `DXMT9_ARCHIVE_MAX_PREWARM_MB` size guard, locked milestone save. The preserved `125MB` archive that deterministically self-aborted 3DMark05 now runs `status=pass`; probe FPS baselines are freed from hidden archive state. |
| H216 | The promoted trio becomes the engine default | accepted default promotion | [present-pacing-engine-default-trio.203](present-pacing-engine-default-trio.203.md) flips `DXMT9_OFFLOAD_COMMIT_REPLAY` to engine-default ON with the index-cache default following it (`d45af067`), after all three blockers fell (per-present boundary suppression + ordinal latency cap `72132513`, R-BACK-2.51(g)/(h); the native-spec harness drain gap that masqueraded as heap corruption `cad446ce`). Pure engine-default proof: pair env set nowhere, `2,220` presents, trio counters live, `present_boundary_skipped` exactly `1.0/present`, zero GPU errors. |
| H217-H220 | Rejected experiment lanes removed from the tree | accepted cleanup waves | Four removal waves delete every rejected carrier whose reopen premise died with the engine-default offload (H195) and the H212 game-CPU attribution: open-CB/tail-present/split-present family + `DXMT9_FS_HALF_PRECISION` (`6379d5c8`, orphan sweep `a083bc8f`), draw-run preflush merge/mixed-carrier (`92047c4e`), chunk-end carry + the `AndRun`/`WithResourceMarking` carrier family + chunk-end flush probe (`570a5cde`, `04c9a827`), compact uniform submission carrier, canonical draw-run fast path, legacy publish-time PSO prefetch pair, and PE flush-after-clear/draw pacing probes (`bb1bec1d`, `c33d250a`, `8d16f290`, `f1224bdf`). Cooled GT1 smokes stay at `2,280` presents / zero GPU errors with the trio live. Every opt-in lane left is a live-default diagnostic A/B switch or an open frontier. See the [log](log.md) H217-H220 rows for per-wave detail. |
| H221 | Probe-wrapper defaults aligned with the promoted engine defaults | accepted policy change | `run_3dmark05_perf_probe.sh` now pins `DXMT9_OFFLOAD_COMMIT_REPLAY` / `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` to `1` by default like the engine (`e5129346`); env `0` is the off switch. Probe `result.json` baselines after 2026-07-12 are trio-on by default; reproduce historical default-off recipes by passing `0` explicitly. |
| H236 | Current GT2 first ceiling is the saturated producer thread, but PE `d3d9.dll` is only 10.6% | accepted current attribution | [present-pacing-current-bottleneck-pe-symbol.236](present-pacing-current-bottleneck-pe-symbol.236.md) joins a clean current-cap run, a ten-second Time Profiler interval, a Metal stage sidecar, and the Tier 2 PE sampler. Producer utilization is one full core versus encode `60%` and replay `57%`; PE module shares are game `64.3%`, `winemetal.dll` `13.9%`, and `d3d9.dll` `10.6%`. The PE module has no single dominant remaining leaf. |
| H237 | CPU-ready Tape now completes but still misses strict locality | promotion rejected; implementation remains default off | [present-pacing-cpu-ready-tape-promotion-gate.237](present-pacing-cpu-ready-tape-promotion-gate.237.md) records a clean native/formal/build/triangle gate and a completed same-build GT2 pair. Ready depth moves `1.000 -> 2.200`, but CB/pass/tile rise `0.31%`/`0.73%`/`2.93%`; the next owner is incomplete retained cross-source suffix attribution, not capacity or another FPS-only run. |
| H238 | Active-session lookahead closes every locally provable hold, but not the locality gate | implementation and proof complete; promotion rejected | [present-pacing-cpu-ready-active-head-locality.238](present-pacing-cpu-ready-active-head-locality.238.md) adds active-seed-preserving whole-head retention, carried terminal-suffix ownership, native tests, and a bounded TLA model. The final GT2 pair still raises CB/pass/tile by `0.25%`/`1.21%`/`4.70%`. Exact rejection conservation attributes all non-held heads to Present (`1,245`) or absence of a future Writing identity (`227`); the next owner is an atomic next-source intent contract, not another local wait policy. |
| H239 | Exact replay-FIFO next-source intent closes the identity contract, but not locality | implementation and proof complete; promotion rejected | [present-pacing-cpu-ready-next-source-intent.239](present-pacing-cpu-ready-next-source-intent.239.md) binds adopted raw, Direct disposition, cancellation, generation-stamped publication, and wake progress. GT2 selects the intent zero times and still raises CB/pass/tile by `0.34%`/`1.15%`/`4.40%`. Exact late-action accounting resolves `10,770/11,038` unknown actions to Store, moving the remaining owner to bounded replay/store-action equivalence. |

## Current Frontier

The engine-default trio is promoted and the dead lanes are gone; the honest
remaining branches are:

- **Producer thread (dominant, but mostly outside PE):** H236 measures one full
  producer core. The game executable owns `64.3%`; PE `d3d9.dll` owns `10.6%`
  and has no remaining single large leaf. Broad recorder-local tuning is closed
  as an FPS lane.
- **Bridge/resource updates:** PE `winemetal.dll` owns `13.9%`. Keep only
  structural work that removes buffer lock/unlock/upload traffic or bridge
  crossings; another local cache needs a new exact hot symbol first.
- **Replay and encode second ceilings:** replay and encode use approximately
  `57%` and `60%` of separate cores. The live dxmt9 branches are replay
  snapshot/materialization elimination and safe CPU-stage overlap. Parallel
  render encoding remains non-default after duplicated CPU cost outweighed its
  chunk-wall reduction.
- **CPU-ready Tape:** the publication, admission, session, active-head, and
  next-source-intent mechanisms are implemented and bounded by native/TLA
  evidence, but H239 still fails CB/pass/tile locality. The remaining design
  question is frame-wide sealing versus an incremental store-action
  equivalence proof; the provider remains default off.

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [present-pacing-cpu-ready-next-source-intent.239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality](present-pacing-cpu-ready-next-source-intent.239.md)
- [present-pacing-cpu-ready-active-head-locality.238 - Active-Session Lookahead Closes the Local Hold Gap but Not Tape Locality](present-pacing-cpu-ready-active-head-locality.238.md)
- [present-pacing-cpu-ready-tape-promotion-gate.237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate](present-pacing-cpu-ready-tape-promotion-gate.237.md)
- [present-pacing-current-bottleneck-pe-symbol.236 - Current GT2 Ceiling Is The Producer Thread; PE d3d9.dll Is 10.6%](present-pacing-current-bottleneck-pe-symbol.236.md)
- [present-pacing-engine-default-trio.203 - The Promoted Trio Becomes The Engine Default](present-pacing-engine-default-trio.203.md)
- [present-pacing-archive-prewarm-hardening.202 - Archive Prewarm Hardening Closes The Startup-Flake Class](present-pacing-archive-prewarm-hardening.202.md)
- [present-pacing-inline-const-delta.201 - Inline Const Delta Proves Mechanism But Lands Inside The Noise Band](present-pacing-inline-const-delta.201.md)
- [present-pacing-decimated-pe-stats.200 - Decimated PE Stats Size The Recorder Core At ~8.5ms/present](present-pacing-decimated-pe-stats.200.md)
- [present-pacing-postcache-resample.199 - Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob](present-pacing-postcache-resample.199.md)
- [present-pacing-readonly-cache-stacking.198 - Readonly Cache Stacks With The Promoted Pair For +26% Cumulative](present-pacing-readonly-cache-stacking.198.md)
- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
