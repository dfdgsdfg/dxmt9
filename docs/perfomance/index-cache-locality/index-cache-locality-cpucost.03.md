---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 03
title: Candidate Builder Split + Dense Adjacency
date: 2026-06-05
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L426-L490
---

# Candidate Builder Split + Dense Adjacency

**Question / hypothesis.** Within the cold candidate path, is the CPU owner the reorder
*builder* or the gain *gate*? And can the builder be sped up without changing which
candidates are selected?

**Method.** Split candidate bucket into four nested timers
(`encode_draw_index_cache_original_measure / candidate_build / candidate_measure / gate_cpu_ms`).
Replaced the sparse `unordered_map` adjacency with dense vertex adjacency for compact
index ranges (`<= 131072`), and switched the production gain-gate measure to LRU32-only
(full 16/32/64 + unique/span kept for diagnostic paths). Paired opt-in `run_experiment.py`
runs; all `status=pass`, `present_encoded=1440`.

**Result.** Cold candidate split: build `242.8ms` (`62.5%`) is the owner, original
measure `77.1ms`, candidate measure `68.1ms`, gate `0.01ms`. Dense adjacency:
candidate CPU `388.835→159.933` (`-58.87%`); candidate_build `242.812→132.265`
(`-45.53%`); original/candidate measure `~77→14.6ms` / `~68→12.4ms` (`-81%`, LRU32-only);
`encode_draw_index_setup_cpu_ms 870.371→623.709` (`-28.34%`);
`encode_draw_stream_bind_cpu_ms -8.92%`. Selection unchanged: same `125` candidates,
`67` reordered buffers, `1,745,724` candidate bytes. `*_miss16/64` are intentionally `0`
in production fast-measure; use `*_miss32`.

**Verdict.** Accepted — the candidate CPU owner was the dense builder, now nearly halved.
Production gain-gating needs only LRU32. Remaining candidate owner is the `~132ms`
builder, not measurement or gate cost. Feeds the fast-measure proof.

**Related.** [index-cache-locality](index.md) · prev: [index-cache-locality-cpucost.02](index-cache-locality-cpucost.02.md)
· next: [index-cache-locality-cpucost.04](index-cache-locality-cpucost.04.md) · [index-cache-locality-opaque.06](index-cache-locality-opaque.06.md)
(the smoke that used this speedup) · [index-reuse-measurement](../index-reuse-measurement/index.md) (LRU model).
