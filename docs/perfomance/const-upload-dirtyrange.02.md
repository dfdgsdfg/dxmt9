---
domain: const-upload
subcategory: dirtyrange
order: 02
title: Dirty Range Reset Xcode Frame Capture
date: 2026-06-01
type: validation
status: rejected
source: specs/perfomance.plan.md#L4635-L4704
---

# Dirty Range Reset Xcode Frame Capture

**Question / hypothesis.** After the dirty-range reset removed the multi-GB cbuf
amplification ([[const-upload-dirtyrange.01]]), does the GPU frame bottleneck
move? Take a programmatic Metal capture of GT1 frame60 with the perf profile +
encoder breakdown and read Xcode encoder counters.

**Method.** `frame60.gputrace` capture, Xcode replay + encoder-counter export,
joined with dxmt attribution. Output:
`experiments/output/app-d3d9-3dmark05-capture-debug-frame60/` and
`traces/app-d3d9-3dmark05-20260601-capture-debug-frame60/analysis/frame60-{performance.gputrace,counters-xcode.csv,counters-summary.csv,xcode-dxmt-joined-summary.csv}`.

**Result.** Frame60: `36.58ms` GPU, `10` render encoders, `396` draws. Top
three encoders (≈`98.4%` of frame GPU, ≈`1.64GiB` device-memory writes):
encoder `2` `20.75ms`/`981.2MiB` buffer write (`187` draws, `271` stream / `160`
IB handle changes); encoder `1` `9.37ms`/`421.4MiB` (`156` draws); encoder `0`
`5.90ms`/`225.4MiB` (`42` draws). dxmt-attributed argbuf cbuf writes after the
reset are only `163KiB`/`111KiB`/`175KiB` per top encoder.

**Verdict.** Rejected (as GPU fix). The top-pass GPU cost is render-pass /
device-memory write pressure plus stream/IB churn inside the same heavy passes
— NOT the former multi-GB cbuf upload amplification, which is now hundreds of
KiB per encoder. Confirms cbuf upload is a CPU amplifier, not the GPU limiter.
Hands the bottleneck to render-pass coalescing/store proof and stream/IB
bind coalescing.

**Related.** [[const-upload]] · prev: [[const-upload-dirtyrange.01]] · the GPU
owner it exposes → [[hidden-backend-storage]] · [[render-pass-store]] (RT/depth
re-entry, store) · [[state-churn-encode]] (stream/IB churn in top passes).
