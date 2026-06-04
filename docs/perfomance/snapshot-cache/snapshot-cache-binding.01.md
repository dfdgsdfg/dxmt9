---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: binding
order: 01
title: Binding-Agnostic Snapshot & Override Compatibility
date: 2026-06-04
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L1818-L1915
---

# Binding-Agnostic Snapshot & Override Compatibility

**Question / hypothesis.** The stream/IB-dominant miss profile motivated a
binding-agnostic snapshot: clear stream buffers / offset / stride / index buffer /
`DrawShaderLayoutContext.vertexDecl.streams` from the stable base draw state and
carry the real per-draw stream/IB binding in `DrawBindingOverride` (uniform hashes
stay in the base state for variant attribution). Expected: far higher cache hit
rate without correctness loss.

**Method.** No-gputrace A/B across three implementation checkpoints; cache
compatibility for batching now uses binding-agnostic semantic comparison via
`drawRunSubmissionStatesCompatibleForBatch()`.

**Result.** (hit rate / snapshot CPU / pipeline-lookup CPU / batch groups·records·max)
- `miss-reason-r2` (baseline): `16.52%` / `21142.122ms` / `863.918ms` / `356143·670726·32` — normal.
- `binding-agnostic-r1`: `46.83%` / `16379.858ms` / `1472.568ms` / `382552·727285·32` —
  cache works but **texture mapping broken** (PSO prefetch used stream-less base layout).
- `binding-agnostic-corrected-r1`: `46.42%` / `19036.759ms` / `6248.274ms` / `667680·667680·1` —
  texture restored (per-draw override forced live PSO lookup) but exposed a
  batch-compatibility bug (max records collapsed to 1).
- `binding-agnostic-compatible-r1`: `46.33%` / `19927.003ms` / `6680.072ms` / `370649·697798·32` —
  normal screenshot, `submit_draw_run_batch_max_records` back to `32`, records/group `1.883`.

**Verdict.** Inconclusive trade: hit rate ~tripled (16.52% → 46.33%) and batch
compatibility was fixed, BUT the prefetched PSO handle had to be bypassed whenever a
per-draw binding override exists (it was built from the stream-less base layout and
caused the texture regression). That raised `encode_draw_pipeline_lookup_cpu_ms`
from `863.918ms` to `6680.072ms`. Next fix: a second prefetch identity keyed by the
override-applied stream layout.

**Related.** [[snapshot-cache]] · prev [[snapshot-cache-snapshot.03]] · next
[[snapshot-cache-prefetch.01]] · binding-override mechanism [[state-churn-encode]] ·
[[overview]]
