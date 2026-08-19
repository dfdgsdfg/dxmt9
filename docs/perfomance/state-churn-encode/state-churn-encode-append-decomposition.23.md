---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 23
title: In-Process PE Sampler First Light — Game 60%, Crossings 17%, Recorder 10%, And A Named Function List
date: 2026-08-19
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--sampler-t1b
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.19.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.17.md
---

# In-Process PE Sampler First Light — Game 60%, Crossings 17%, Recorder 10%, And A Named Function List

**Method.** The Rosetta wall (rules note, `1c3b0f97`) blocks xctrace from
attributing PE code, so this run used the new in-process sampler (merged
`3f360e5b`): `DXMT9_PE_THREAD_SAMPLER=1` + `DXMT9_PE_MODULE_MAP=1` at 250 Hz
over a full supervised GT2 run, sampling the game thread's true Win32 `Eip`
via SuspendThread/GetThreadContext and classifying against the probe-verified
module map. 13,806 samples, **zero suspend/context/resume failures** (the
suspend-window safety contract held in production), probe PASS. Caveats: the
run needed `DXMT_LOG_LEVEL=info`, so logging costs below are partly
diagnostic-run artifacts; a sampled PC is "around now" under Wine's
asynchronous suspension, a distribution rather than an exact trace; and a
sampler run is not a valid performance sample.

**Game-thread module split (first direct measurement):**

| module | share | ~ms of a 37 ms frame |
|---|---|---|
| `3DMark05.exe` (game logic) | **60.3%** | 22.3 |
| `winemetal.dll` | **16.8%** | 6.2 |
| `d3d9.dll` (our recorder) | 10.5% | 3.9 |
| `ntdll.dll` | 3.7% | 1.4 |
| CRT (`ucrtbase`+`MSVCR71`+`MSVCP71`) | 4.6% | 1.7 |
| rest (win32u/kernel32/MFC/unknown…) | ~4% | ~1.5 |

**Cross-validation.** `d3d9.dll` + `winemetal.dll` = 27.3% ≈ the decimation
estimate of the PE layer (~28%, [.17]) — two fully independent methods agree.
Interpretation of the surprising `winemetal.dll` share: while the 32-bit
thread executes a unix call, its saved Win32 PC rests at the bridge stub's
return site inside `winemetal.dll`, so that 16.8% is the game-thread cost of
**PE→unix crossings plus the synchronous unix-side work behind them** — and
it is larger than the entire recorder. The crossing lane is therefore the
next producer vein candidate, ahead of anything inside `d3d9.dll` itself.

**Tier 2 came free.** The release PE build already carries symbols, so the
self-PC buckets symbolize immediately (top-32 buckets = 46% of `d3d9.dll`
self):

| share of top-32 | function |
|---|---|
| 16.3% | `notePeDeviceCallAfterPresent` — post-Present call tracking that runs **on every hot entry unconditionally** (only its clock read is stats-gated); ~0.3 ms/present of ungated diagnostic scaffold — the first concrete trim candidate the sampler produced |
| 11.4% | `CommandChunkBuilder::reset()` |
| 7.9% / 4.2% / 3.0% | `fflush` / `dxmt9DeviceInfoLog` / `logLine` — info-level logging artifacts of this diagnostic run |
| 7.7% | `touchConstShadow` (consistent with decimation's ~0.42 ms/present) |
| 9.1% | `D3D9PePendingCommandRetainer` retain+release |
| 3.4% / 3.3% | `buildSparseState` / `appendRecord` |

**Verdict.** The Tier 1b/2 instrument works end-to-end on first light and
already reframed the map: the game's own logic is 60% (the eventual hard
ceiling ≈ `1000/22.3` ≈ 45 fps if all dxmt9 cost vanished), our reducible
share is ~27% split roughly 6.2 ms crossings vs 3.9 ms recorder-proper, and
the two named leads are the crossing lane and the ungated
`notePeDeviceCallAfterPresent` scaffold. Follow-ups: gate or slim the
call-tracking scaffold, decompose the crossing share (which unix calls own
it), and rerun the sampler at warn level once emission is decoupled from
info logging if a cleaner self-histogram is needed.
