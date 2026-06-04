---
domain: state-churn-encode
subcategory: stream
order: 01
title: Encoder Stream Breakdown Re-run
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L4132-L4231
---

# Encoder Stream Breakdown Re-run

**Question / hypothesis.** Is the stream/IB draw-run failure an offset-only
compatibility problem or true handle churn? Emit `[dxmt9-perf-encoder-stream]`
rows per used stream to split per-stream samples / binds / handle / offset /
stride changes.

**Method.** GT1 with `DXMT9_PERF_ENCODER_BREAKDOWN=1` extended to stream rows.
Output: `experiments/output/app-d3d9-3dmark05-encoder-stream-breakdown/{dxmt9.log,result.json,encoder-stream-breakdown-summary.md}`.
`present_encoded=1260`, `draw_calls=913714`, `render_pass_begin=14673`,
`gpu_command_buffer_time_ms=3630.387`.

**Result.** `14948` encoder rows, `18006` stream rows (only streams 0 and 1
used). Stream totals: `1230347` samples, `1083437` Metal binds, `1007089`
handle changes (`81.9%` of samples), `93182` offset (`7.6%`), `69574` stride
(`5.7%`). IB: `928724` samples / `922989` binds, `756672` handle changes
(`81.5%`). Per-stream split:

| Stream | Samples | Binds | Handle | Offset | Stride |
|---:|---:|---:|---:|---:|---:|
| 0 | `928724` | `813948` | `755388` | `69858` | `1284` |
| 1 | `301623` | `269489` | `251701` | `23324` | `68290` |

Argbuf cbuf `4631819248` (~4.63GB); transient vertex `1038672288` (~1.04GB).

**Verdict.** Tooling. Removes the offset-only ambiguity: both streams have
handle-change rates near draw frequency; stream 1 owns almost all stride churn.
Confirms a draw-run model must handle per-draw *resource handle* changes, not
just offsets. Top cbuf and top stream/IB handle-churn encoders coincide
(ordinal 4, seq 342..351).

**Related.** [[state-churn-encode]] · next: [[state-churn-encode-stream.02]] ·
[[state-churn-encode-encoder.01]] (first breakdown run) ·
[[state-churn-encode-churn.01]] (the handle-churn hypothesis) ·
[[const-upload]] (4.63GB cbuf coincides in the same encoders).
