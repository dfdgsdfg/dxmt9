---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: expand
order: 01
title: Disable Auto Expand Indexed Experiment
date: undated
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L5179-L5218
---

# Disable Auto Expand Indexed Experiment

**Question / hypothesis.** Indexed-draw auto expansion contributes the entire
~1.056GB per-encoder transient vertex-write bucket. Does disabling it remove that
amplifier, and does it move GPU time?

**Method.**
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT9_PERF_ENCODER_BREAKDOWN=1 DXMT_DISABLE_AUTO_EXPAND_INDEXED=1`.
Output: `experiments/output/app-d3d9-3dmark05-no-auto-expand-indexed`. The flag
gates `shouldAutoExpandIndexedDraw`; `DXMT_FORCE_EXPAND_INDEXED=1` still forces
diagnostic expansion.

**Result.** vs the unique-handle baseline:
`draw_expanded_indexed 5837 -> 0`; encoder `transient_vertex_bytes 1056136680 -> 0`;
`transient_upload_bytes 2122823492 -> 1064597340` (~half);
`encode_draw_fvf_decode_cpu_ms 800.333 -> 361.017`;
`encode_draw_cpu_ms 17174.496 -> 16631.666`;
`gpu_command_buffer_time_ms 3645.5 -> 3535.32` (modest);
`bind_vertex_buffer 1070989 -> 1059327`. Image metrics shifted:
`mean_luma 71.896 -> 53.659`, `variance 5974.561 -> 3637.538`.

**Verdict.** Inconclusive. Removes the whole transient vertex-write amplifier and
halves transient upload bytes, with modest whole-run GPU improvement — a real
contributor but not the sole bottleneck. The screenshot frame/time differs and
image metrics change materially, so visual correctness must be proven before
making it default. Treat as an experiment.

**Related.** [state-churn-encode](index.md) · next: [state-churn-encode-expand.02](state-churn-encode-expand.02.md) ·
[state-churn-encode-stream.03](state-churn-encode-stream.03.md) (flagged auto-expand as a separate amplifier) ·
[index-cache-locality](../index-cache-locality/index.md) (the accepted indexed-path GPU win is locality, not
expansion).
