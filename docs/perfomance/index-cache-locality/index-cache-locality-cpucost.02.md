---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 02
title: Rejected Lookup Fast-Path Probe
date: 2026-06-05
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L369-L425
---

# Rejected Lookup Fast-Path Probe

**Question / hypothesis.** Can the reordered-index-cache *lookup* cost (`104.6ms` over
`585k` cached decisions, with many rejected-key hits) be reduced by a faster lookup
structure than the per-source vector scan?

**Method.** Two code changes tried and measured: `hash_index` (per-source
`unordered_map<ReorderedIndexBufferCacheKey, entry_index>` alongside the owning vector)
and `last_lookup` (vector cache + last-hit index + move stale-entry pruning out of the
hit-only `find`). Paired `run_experiment.py` opt-in smokes; both changes removed after
measurement. All three runs `status=pass`, `returncode=143`, `present_encoded=1440`.

**Result.** `encode_draw_index_cache_lookup_cpu_ms`: vector `104.615`, `hash_index`
`165.316` (regression), `last_lookup` `108.352` (noise). `encode_draw_index_setup_cpu_ms`:
vector `851.791`, `hash_index `969.542`, `last_lookup` `922.181`. Lookup is only
~`0.18us` per cached decision in the original vector path. `unordered_map` overhead
dominates the small per-source cache; the last-hit micro-cache does not match the
3DMark05 access pattern and does not reduce the index setup bucket.

**Verdict.** Rejected — restore original vector cache. The rejected-hit count is large
but lookup itself is cheap; the remaining CPU side-effect is cold candidate build/gate
cost plus the broader indexed setup path, not lookup structure.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-cpucost.01]]
· next: [[index-cache-locality-cpucost.03]] (the build, the actual owner).
