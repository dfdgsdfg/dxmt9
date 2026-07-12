---
domain: state-churn-encode
workload: 3DMark05 GT1
title: "State-Churn Encode — the CPU encode path and draw-run batching - Current Overview"
type: domain-overview
status: current
updated: 2026-07-12
source: docs/perfomance/state-churn-encode/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/state-churn-encode/index.md; docs/perfomance/state-churn-encode/log.md
---

# State-Churn Encode — the CPU encode path and draw-run batching - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `state-churn-encode.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **CPU encode side** of GT1: why the importer almost never
batches draws into draw-runs, what state actually breaks the runs, and what the
binding-override fix bought. It introduces the per-encoder breakdown
instrumentation (`DXMT9_PERF_ENCODER_BREAKDOWN=1`), measures stream/IB handle
churn, decomposes the draw-run state-delta taxonomy down to the exact stream+IB
pair, lands the `DrawBindingOverride` payload that lets stream/IB-only changes
batch, rechecks after submission batching, and tests disabling auto-expand-indexed.
Every finding here is CPU-throughput. None of them move the GPU frame-time
bottleneck — that is owned by [hidden-backend-storage](../hidden-backend-storage/index.md).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H206 | The remaining draw-batch-submit row is mostly an unmeasured outer-submit gap | rejected by derived reanalysis | [state-churn-encode-encode-phase.197](state-churn-encode-encode-phase.197.md) extends the summary tool with parent-minus-child residual rows and re-summarizes H225. Existing children explain `89.96%` of `commit_chunk_draw_batch_submit_cpu_ms`; append alone is `76.63%` of the parent. Inside append, uniform is `51.51%`, state is `26.00%`, and append residual is `9.73%`. | Treat append uniform/state materialization as the next local submit branch. Do not chase a broad outer-submit unknown or `.gputrace` from this CPU-only evidence. Any append-width win still needs P4/no-enqueue movement before FPS promotion. |
| H207 | Uniform append is mostly just payload lookup and payload-record append | rejected by derived reanalysis | [state-churn-encode-encode-phase.198](state-churn-encode-encode-phase.198.md) adds derived uniform-append CPU rows and re-summarizes H225. The parent is `0.664ms/present`, but payload lookup is only `0.152`, payload append storage is only `0.101`, and the known child share is `38.02%`. The residual is `0.411ms/present`, likely stage-level find/append/vector maintenance around vertex/pixel constant payloads. | Do not optimize only the final payload-record append copy. The next local branch is stage-level uniform append materialization or N-1 state/uniform materialization elision. This remains a local CPU branch until replay/encode/P4 rows move under the visual-safe no-gputrace gate. |
| H208 | The remaining uniform-append residual needs component-level attribution before mutation | instrumentation accepted; runtime gate completed | [state-churn-encode-encode-phase.199](state-churn-encode-encode-phase.199.md) adds behavior-neutral counters for fixed/VS/PS component find and append scopes inside `appendDrawUniformPayload()`, plus summary rows for known-with-components share and component residual. Existing H225 data predates the counters, so the new rows correctly report `n/a` rather than zero. | The follow-up run in H209 makes fixed-payload find the first local cleanup target. Keep the component split as attribution tooling; it is not `.gputrace` evidence by itself. |
| H209 | Fixed-payload handle carry reduces the targeted component but does not break the FPS wall | accepted local cleanup; not FPS proof | [state-churn-encode-encode-phase.200](state-churn-encode-encode-phase.200.md) stamps submissions with `uniformFixedPayloadGeneration` and lets `appendDrawRunBatch()` reuse the previous slot-local fixed handle when the generation is unchanged and the record hash still matches the current fixed payload. The targeted row moves: `uniform_component_fixed_find_cpu_ms_per_present` `0.229 -> 0.150`, and total component find `0.323 -> 0.257`. | Keep the carry path. Do not promote it as a wall-breaker: `uniform_append_parent_cpu_ms_per_present` is flat (`0.882 -> 0.880`), sampled FPS is noisy/regressed (`16.170 -> 14.261`), and no-enqueue completion wait remains dominant. The next FPS-facing branch remains P4/no-enqueue overlap or larger replay/encode materialization elision. |
| H210 | Uniform append residual after fixed-handle carry is bounded local cleanup | accepted direction | [state-churn-encode-encode-phase.201](state-churn-encode-encode-phase.201.md) audits H209's current run and the `appendDrawUniformPayload()` source. The remaining parent is `0.880ms/present`; known scopes plus component scopes explain `77.75%`, leaving `0.196ms/present` residual. VS stage append is the largest named remaining component (`0.116ms/present`) because `661,640` VS stage records are appended (`0.833` per payload append), while full uniform generation reuse remains `0` and full payload hash reuse is only `3,970 / 672,993` adjacent payloads. | Do not spend `.gputrace` on uniform append residual alone. A VS-stage split or stage-handle tweak is optional local cleanup with a small ceiling. The FPS branch remains P4/no-enqueue overlap or a larger replay/encode materialization change that moves serial rows under the visual-safe no-gputrace gate. |

## Current Status After The Engine-Default Offload And The Dead-Lane Cleanup

The commit-replay offload is engine-default ON since `d45af067`
(H216 in [present-pacing](../present-pacing/index.md)), so this domain's
whole replay/submit cost class now runs on a device-owned worker that idles
`~39.4ms/present` — worker-side CPU wins are FPS-flat until the worker stops
having idle headroom, and the residual FPS wall is the game's own CPU (H212).

The rejected replay-carrier lanes whose history lives in this domain's leaves
and [log](log.md) were removed from the tree in the H217-H220 cleanup waves:
chunk-end carry plus the entire `AndRun`/`WithResourceMarking` carrier family
and the chunk-end flush probe (`570a5cde`, `04c9a827`), the draw-run preflush
merge/mixed-carrier lanes (`92047c4e`), the compact uniform submission carrier
(`bb1bec1d` — the H132 accepted always-on `ChunkSlot` stage-constant
byte-arena storage remains), the canonical draw-run fast path (`c33d250a`),
and the legacy publish-time PSO prefetch pair (`8d16f290`). Treat those leaf
documents as historical evidence, not open work. The domain frontier is
encode-side P4 overlap and pass-streaming (R-BACK-2.39/2.40/2.43, still open),
plus the live-default diagnostic A/B switches (encode-slot PSO memos,
unpublished-slot PSO prefetch probe, sparse const records).

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [state-churn-encode-encode-phase.201 - Uniform Append Residual After Fixed Handle Carry](state-churn-encode-encode-phase.201.md)
- [state-churn-encode-encode-phase.200 - Uniform Fixed Payload Handle Carry](state-churn-encode-encode-phase.200.md)
- [state-churn-encode-encode-phase.199 - Stage-Level Uniform Append Split Counters](state-churn-encode-encode-phase.199.md)
- [state-churn-encode-encode-phase.198 - Append Uniform CPU Residual Reanalysis](state-churn-encode-encode-phase.198.md)
- [state-churn-encode-encode-phase.197 - Draw Batch Submit Residual Reanalysis](state-churn-encode-encode-phase.197.md)
- [state-churn-encode-encode-phase.196 - Queue Lock Attribution Runtime](state-churn-encode-encode-phase.196.md)
- [state-churn-encode-encode-phase.195 - Current Wall Review and Next Owner Split](state-churn-encode-encode-phase.195.md)
- [state-churn-encode-encode-phase.194 - Forced Resource-Marking Flush Attribution](state-churn-encode-encode-phase.194.md)
