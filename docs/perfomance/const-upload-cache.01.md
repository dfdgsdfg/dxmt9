---
domain: const-upload
subcategory: cache
order: 01
title: Cbuf Slice Cache Experiment
date: undated
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L4907-L4961
---

# Cbuf Slice Cache Experiment

**Question / hypothesis.** Keep the fresh resource-array argbuf table per draw,
but when the uniform payload hash is unchanged and no cbuf dirty bits are
pending, point the new table at cached low-level cbuf slices instead of
re-uploading the same host structs. Does encoder-local downstream reuse cut the
cbuf write bucket?

**Method.** Hash-keyed encoder-local repoint: on (same uniform hash AND no
pending dirty cbuf bits), repoint table entries to cached cbuf slices; otherwise
upload dirty/all host structs and refresh the cache. GT1 run. Output:
`experiments/output/app-d3d9-3dmark05-cbuf-cache/dxmt9.log`.

**Result.** vs baseline: argbuf cbuf bytes `4643320552→4618735224` (only
~`0.5%`); `transient_upload_bytes` `5641482380→5605850188` (~`0.6%`);
`encode_draw_cpu_ms` `18570.558→18435.036` (no meaningful shift);
`commit_chunk_draw_run_break_type_const_upload` `885557→883446` (still
draw-frequency); stream/IB deltas unchanged. Argbuf *table* bytes unchanged
(still one fresh table per resource-array draw).

**Verdict.** Inconclusive / minor. Structurally valid as a guard against
redundant table reopens but only ~`0.5%` reduction. The dominant condition is
that the recorder/importer creates const-upload boundaries at nearly draw
frequency (~`883k` records), so the payload is genuinely dirty from the
encoder's view. The real target is UPSTREAM record coalescing, not downstream
reuse.

**Related.** [[const-upload]] · related downstream-vs-upstream framing:
[[const-upload-sparse.01]] · [[state-churn-encode]] (const-upload records as
draw-run barriers) · [[hidden-backend-storage]].
