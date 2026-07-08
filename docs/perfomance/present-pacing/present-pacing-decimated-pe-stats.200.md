---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: producer-attribution
order: 200
title: Decimated PE Stats Size The Recorder Core At ~8.5ms/present
date: 2026-07-09
type: no-gputrace
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-pe-decimated-n64-r1-20260709/3dmark05-direct.log; experiments/output/app-d3d9-3dmark05-pe-decimated-n16-r3-20260709/3dmark05-direct.log; experiments/output/app-d3d9-3dmark05-pe-decimated-n64-r1-20260709/result.json; docs/perfomance/present-pacing/present-pacing-postcache-resample.199.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-pe-cost-verification.192.md; docs/perfomance/present-pacing/present-pacing-pe-const-overhead-cut.193.md
---

# Present-Pacing H213 - Decimated PE stats (H212 branch decision)

## Question

H212 left one question: how much of the `~36ms/present` Rosetta guest blob is
dxmt9's PE recorder core? Full `DXMT9_PE_RECORDER_STATS=1` cannot answer it
(H192: `34%` throughput loss embeds the instrument in the measurement).

## Instrument

`DXMT9_PE_STATS_DECIMATION=N` (commit `2fe9f8a6`): every-Nth-event RAII
timing on the four recorder hot scopes — `appendRecordDirect`,
`touchConstShadow`, `flushConstShadow`, `buildDrawPrimitivePacket` — with a
cumulative `[dxmt9-pe-decimated]` line every 60 presents (Info level; run
with `DXMT_LOG_LEVEL=info`). Estimator: `sampled_ms x N / presents`.

## Verification

- **Perturbation gate:** `N=64` run `2,293` presents, `N=16` run `2,272` —
  both inside/above the stacking population (`2,220-2,271`); the instrument
  is free (vs `-34%` for full stats).
- **Decimation-factor independence:** per-site estimates agree across a 4x
  sampling-density change within `+/-12%` (totals within `4%`).

| Scope | events/present | est ms/present (N=64 / N=16) |
|---|---:|---:|
| `appendRecordDirect` (incl. `~1.1ms` in-scope commit bridge) | `1,393` | `4.92` / `5.52` |
| `touchConstShadow` | `5,512` | `1.49` / `1.38` |
| `flushConstShadow` (740 draws x 6 shadows) | `4,521` | `1.77` / `1.64` |
| `buildDrawPrimitivePacket` | `749` | `0.35` / `0.34` |
| **recorder core total** | | **`8.5-8.9`** (PE-pure `~7.4-7.8`) |

## Verdict — H212 branch resolved to the Plan-B side

The recorder core is `~16-18%` of the `~49ms` producer wall — at the top of
H212's decision range. The const chain alone is `~5.1-5.6ms/present`
(setter `1.4` + flush `1.7` + the const-record share of append, which is
count-`46%` of appends and byte-heavy at `~205KB/present` const upload).
An inline-const-delta wire change (Plan B) that eliminates most of the
const chain has a real `~+10%` FPS ceiling, and H193's null result is now
explained: those cuts removed only fixed dispatch overhead (`~0.3-1ms`),
not the memcpy/walk bodies measured here.

## Startup-flake anatomy (method note)

Two more occurrences during verification (7th/8th), now with Info-level
logs: the game process completes the ABI handshake, prewarm, capabilities,
`layer_acquisition` (hwnd), creates the device, appends exactly one draw
packet + 6 const flushes, then **exits voluntarily with returncode 0 before
the first Present** — no dxmt9 error, clean destructor dumps. This is an
app-side benchmark self-abort (focus/mode class), correlated with
back-to-back relaunches; a 60s settle cleared it. Yesterday's
`mutex lock failed` teardown line (H196/r3) is a separate secondary symptom.
