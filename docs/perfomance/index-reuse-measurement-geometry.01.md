---
domain: index-reuse-measurement
subcategory: geometry
order: 01
title: Geometry Amplification Audit + Draw Geometry Signature Instrumentation
date: 2026-06-01
type: measurement
status: tooling
source: specs/perfomance.plan.md#L3340-L3431
---

# Geometry Amplification Audit + Draw Geometry Signature Instrumentation

**Question / hypothesis.** Two sub-questions before measuring geometry shape:
(a) is dxmt inflating GT1 geometry through its indexed-draw expansion path? and
(b) is the ~1.6 GiB VS bucket driven by many *distinct* geometry submissions or
by dxmt *replaying the same* geometry/pipeline shape repeatedly? Sets up the
dedup and size-histogram probes.

**Method.** Geometry amplification audit reads run-level cumulative counters.
Then `DXMT9_PERF_ENCODER_BREAKDOWN=1` is extended with an encoder-local geometry
signature hashing primitive args, indexed/expanded path, stream and IB
handles/offsets/strides, current PSO/shader/VSOut identity, selected render
states, cull/fill mode, and scissor state — exposed as
`draw_geometry_signature_samples/_unique/_unique_overflows/_duplicates/_consecutive_duplicates/_last`.
Carried through `summarize_3dmark05_perf.py`, `summarize_xcode_encoder_counters.py`,
`analyze_vs_buffer_scaling.py`.

**Result (amplification audit).** `draw_calls 978,461`, `draw_indexed 978,461`,
`draw_expanded_indexed 0`, `expanded_indexed_draws 0`,
`primitive_count / triangle_estimate = 1,481,985,970 / 1,481,985,970`,
top-3 transient V/I `0 / 0 B`. No indexed-expansion inflation, no fan/list
multiplication, hot frame not dominated by UP/fallback upload.

**Verdict.** Tooling/measurement. The "disable indexed expansion" route is moot —
it is already absent in the hot path. The signature is a geometry/backend-shape
diagnostic, not a final-pixel correctness proof (excludes full cbuf payload and
some texture handles), so a high duplicate ratio means "same backend shape
re-rendered", not "deletable redundant draw". Enables
[[index-reuse-measurement-geometry.02]].

**Related.** [[index-reuse-measurement]] · feeds dedup result
[[index-reuse-measurement-geometry.02]] and size histogram
[[index-reuse-measurement-geometry.03]] · owner still
[[hidden-backend-storage]] · CPU side stays [[state-churn-encode]] /
[[const-upload]].
