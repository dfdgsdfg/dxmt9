---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 18
title: Chunk-Seal Cadence A/B — 2 ms Of Producer CPU Removed, Zero FPS, And No Stage Owns The 37 ms Frame
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--cadence-base-a; experiments/output/app-d3d9-3dmark05--cadence-cand-a; experiments/output/app-d3d9-3dmark05--cadence-cand-b; experiments/output/app-d3d9-3dmark05--cadence-base-b
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.17.md
---

# Chunk-Seal Cadence A/B — 2 ms Of Producer CPU Removed, Zero FPS, And No Stage Owns The 37 ms Frame

**Question.** [.17](state-churn-encode-append-decomposition.17.md) attributed
`~2.9 ms/present` of the producer wall to once-per-64-records chunk seals
(`68 µs` × ~42/present). `DXMT9_PE_CHUNK_MAX_RECORDS`/`_MAX_BYTES` are env
tunables; does quadrupling both (256 / 1,310,720 — both must move together
because the frozen legacy sizeHints make the byte precheck bind near 53
draw records) convert that CPU into fps? ABBA on one build (`0d30c273`),
decimation N=64 plus frame sampling on all four runs.

**Mechanism: confirmed, cleanly.** Seals fall `44-51 → 8-9.6` per present;
per-seal cost rises sublinearly (`66-68 → 181-192 µs`); net seal CPU
`2.91/3.47 → 1.45/1.84 ms/present`, and per-record encode also eases
(`2.17/2.19 → 1.79/1.82`). Total producer-CPU reduction **≈2 ms/present**,
zero GPU errors, normal screenshots on all four runs.

**FPS: flat.** Anchored per-frame extraction (the [.17] regex trap avoided):
base mean `26.70/26.77`, candidate `26.80/26.78`; medians `26.6-27.1` both
ways. A real 2 ms cut out of a 37 ms frame should read as ~+5%; the
within-config spread is ~0.3%. The cut did not reach the frame.

**The resulting puzzle is the finding.** With the candidate cadence, the
encode thread's source wait is unchanged (`wait_to_encode_dequeue` p50
`11.2 ms` both sides — despite 5x fewer publishes, so the wait is shaped by
the frame's last commit, not by intermediate cadence), the encode stage wall
eases slightly (`18.5 → 17.7 ms` p50), and the GPU is nearly idle
(`gpu_command_buffer_time` ≈ `1.9 ms`/frame, p50 `2.26 ms`). Inventory of the
37 ms frame: GPU `1.9`, encode busy `~18` (with `11` of wait), PE layer
`~8` (post-cadence), fences `0.3`, present acquire `0.06`,
`present_boundary_wait 0.000`. **No measured stage is close to owning
37 ms.** Either the game's own logic fills `~25 ms` of the game thread — in
which case a repeatable 2 ms removal should still have surfaced as ~5% — or
the game thread itself is not saturated and something paces the loop
(a Present-side block, an internal app timer, or frame-latency lockstep)
absorbing any producer-side savings as idle.

**Bridge-side Present/frame-wait counters could not arbitrate**: GT2 ends by
SIGKILL at timeout, so the PE bridge-perf exit emission is lost (the H231
lesson in counter form).

**Verdict.** Keep the default cadence (the knob is proven safe and the CPU
win is real but unconvertible today); do not spend further effort on producer
CPU until the pacer is identified. The next diagnostic is game-thread
saturation attribution: an xctrace CPU sidecar
(`run_3dmark05_system_trace_sidecar.sh`, needs ~4 GiB free disk and an
unlocked session) showing whether the game thread is ~100% busy (game logic
owns the frame; producer cuts should convert and the flat fps needs a
different explanation) or substantially idle (a pacing wait owns the frame,
and the wait chain — Present block, frame-latency boundary, app timer — is
the real remaining lever).
