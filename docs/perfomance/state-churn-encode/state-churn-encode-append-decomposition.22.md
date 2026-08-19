---
domain: state-churn-encode
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: append-decomposition
order: 22
title: Cadence Promotion — All Gates Green, Default Flipped To 256 Records / 1.25 MiB
date: 2026-08-19
type: experiment-run
status: promoted
source: experiments/output/app-d3d9-3dmark05--dab-*; experiments/output/app-d3d9-3dmark05--promo-*; scratchpad promo-matrix sfiv pair
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.21.md
---

# Cadence Promotion — All Gates Green, Default Flipped To 256 Records / 1.25 MiB

**Gates and results.** Continuing [.21](state-churn-encode-append-decomposition.21.md)'s
promotion candidacy for the 4x chunk cadence
(`DXMT9_PE_CHUNK_MAX_RECORDS=256` + `DXMT9_PE_CHUNK_MAX_BYTES=1310720`):

| gate | result |
|---|---|
| GT2 performance | median **+2.0%** (27.31→27.86, non-overlapping across 3+3 ABBA), mean +0.4% |
| GT1 coverage | median **+3.5%**, mean +3.3% (28.15→29.14 / 30.11→31.09) |
| GT3 coverage | median **+2.5%**, mean +1.0% (62.74→64.29) |
| SFIV coverage | no regression: p10/p25/p75/p90 all equal-or-better (22.2/28.1/132.7/174.8 vs 21.9/27.8/127.8/169.0), mean equal; the raw median delta (−23%) is a bimodal-cliff artifact — the whole-run median sits between the ~25 fps scene hump and the ~130 fps phase hump and slides with phase composition (block means confirm run-to-run phase variance), so percentile pairs are the honest metric for this workload |
| locality conservation | byte-identical: CB/present 4.000 = 4.000, pass/present 15.861 vs 15.867, subCB 3.000 = 3.000 |
| encode pacing | conserved: source-wait p50 11.46→11.66 ms, stage wall p50 19.50→18.78 ms (improved) |
| tail attribution | slow-frame (>60 ms) composition identical in kind between arms — draw-heavy scene spikes (submit_draw ~1,860 vs ~1,690 body) with elevated encode CPU, no cadence-specific mechanism signature; count difference (43 vs 59 of ~4,800) is within scene-phase noise |
| visual | GT1 candidate screenshot inspected frame-exactly normal (Return to Proxycon, no black polygons/artifacts); GT3 and SFIV pairs luma-identical (163/162, 62/63) |
| GPU errors | zero across all fourteen runs |

One matrix invalidation is recorded for method honesty: the first GT1/GT3/SFIV
matrix ran against a locked desktop (direct `run_experiment.py` calls bypass
the probe wrapper's `DXMT_3DMARK05_REQUIRE_UNLOCKED` guard) and failed with
immediate exits and a black SFIV capture; it was rerun after an unlock-wait.

**Promotion.** `kDefaultMaxPendingCommandRecords`/`kDefaultMaxPendingCommandBytes`
in `src/d3d9/d3d9_pe_device.cpp` flip from 64 / 256 KiB to **256 / 1,310,720**.
The env vars remain the rollback path
(`DXMT9_PE_CHUNK_MAX_RECORDS=64 DXMT9_PE_CHUNK_MAX_BYTES=262144` restores the
pre-promotion cadence exactly). The two values must continue to move together:
the frozen legacy sizeHints make the byte precheck bind near 53 draw records
at the old byte cap, so raising only the record cap is inert. Mechanism
recap: 44→8 chunk seals/present returns ~1.2 ms/present of game-thread CPU
on the producer-saturated critical path ([.20] per-thread confirmation).
