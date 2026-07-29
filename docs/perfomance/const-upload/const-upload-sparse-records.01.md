---
domain: const-upload
workload: 3DMark05 GT2
subcategory: sparse-records
order: 01
title: Splitting Sparse Const Records Is A Null — GT2's Constant Cost Is Call Count, Not Bytes
date: 2026-07-29
type: experiment-run
status: rejected-fps-lever
source: experiments/output/app-d3d9-3dmark05-gt2-sparseconst-base; experiments/output/app-d3d9-3dmark05-gt2-sparseconst-cand; experiments/output/app-d3d9-3dmark05-gt2-const-setter-r1
related: docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.03.md; docs/perfomance/const-upload/overview.md
---

# Splitting Sparse Const Records Is A Null — GT2's Constant Cost Is Call Count, Not Bytes

> **Corrected on 2026-07-29 by [attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md).** The null verdict stands, but its stated reason does not: the constant path is `0.8%` of the frame, not `13%` — the difference was instrument bias — so there was never enough there to win. The `84.7%` redundant-register figure was also misread as an opportunity; `touchConstShadow` already skips unchanged registers, so that number is the existing filter working.

**Question / hypothesis.**
[attribution.03](../present-pacing/present-pacing-post-defselect-cpu-attribution.03.md)
put dxmt9's PE-side recording at `30.5%` of GT2's critical thread, of which the
constant scopes are `6.92 ms` — `13%` of the frame — over `31,775` events per
present. A `DXMT9_PERF_VS_CONST_SETTER_RANGE` run then showed two separate
inefficiencies. Does closing the second one help?

**The two inefficiencies, per present.**

| `phase=call` (application setters) | |
|---|---:|
| setter calls | `20,022` |
| registers written | `59,542` |
| registers whose value actually changed | **`9,090` (`15.3%`)** |

| `phase=flush` (upload) | |
|---|---:|
| flush events | `912` |
| registers covered | `52,615` |
| registers changed | `9,090` |
| `changed_span_regs` | `52,615` — **`5.8x` the changed set** |

`changed_span_regs` equals `range_regs` exactly at flush, and equals
`changed_regs` at call: the whole `5.8x` inflation is the min/max merge that
turns a scattered set of `9,090` changed registers into one contiguous span.

**Method.** Paired GT2 A/B on the same build, `--split-sparse-const-records`
(`DXMT9_SPLIT_SPARSE_CONST_RECORDS=1`) against default, frame sampling only.
The flag's presence in the child environment was confirmed through the
wrapper's dry-run env line.

**Result.**

| Lane | fps | presents |
|---|---:|---:|
| default | `18.98` | `1,156` |
| split sparse records | `18.85` | `1,149` |

`-0.7%`, inside noise. Sweeping every counter that differs by `>=5%` per present
between the lanes surfaces only `_max_ms` single-sample maxima and one batching
redistribution (`commit_chunk_draw_submission_batch_size_17_32`,
`12.55 -> 28.54`); nothing in the const-record or const-byte family moves,
because those counters
(`commit_chunk_draw_run_break_type_const_upload*`,
`commit_chunk_const_upload_cpu_ms`) read `0` on both lanes — they instrument the
unix-side `commit_chunk` path, and this knob is a PE-recorder experiment.

**Verdict.** REJECTED as an FPS lever on GT2, and the reason redirects the
work. The knob attacks *bytes per record*, and the `5.8x` span inflation is
real, but bytes are not what the constant path costs here. The measured cost is
per-call: `touchConstShadow` is `4.76 ms` over `21,570` calls (`~0.22 us` each)
and `flushConstShadow` `2.16 ms` over `10,205`. Splitting one merged record into
several changed-register runs reduces bytes while *raising* record count — it
moves the wrong axis, which is what
`agents/rules/environment_variables_encoder.rules.md` already warns about when
it says the knob "may trade fewer const bytes for more const records."

**The axis that is worth attacking** is the first table: `84.7%` of the
registers the application writes carry the value they already had. Filtering
those would cut call count directly, which is the quantity that costs. Whether
a 16-byte compare is cheaper than the `~0.22 us` it would avoid is unmeasured
and is the next question.

**What this run does not establish.** The flag reached the process, but no
counter available on this path proves the split actually occurred, so this is a
null result on FPS rather than a proof that a working split does not help. A
mechanism proof would need PE recorder stats, which perturb throughput by
about a third and so cannot share a run with the FPS measurement.

**Related.**
[attribution.03](../present-pacing/present-pacing-post-defselect-cpu-attribution.03.md) ·
[const-upload](index.md) · [present-pacing](../present-pacing/index.md)
