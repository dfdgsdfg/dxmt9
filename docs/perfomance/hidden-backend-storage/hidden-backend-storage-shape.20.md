---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: shape
order: 20
title: Final-Writer Replay Oracle Is Now an Automated Gate
date: 2026-06-06
type: validation
status: accepted-gate
source: scripts/tools/summarize_3dmark05_perf_gates.py; tests/scripts/test_summarize_3dmark05_perf_gates.py; scripts/tools/README.md; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-perf-gates-final-writer-replay-full.md; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-perf-gates-final-writer-replay-full.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-next-experiment-queue-final-writer-replay-full.csv; traces/app-d3d9-3dmark05-post-visualfix-frame60-60-2-textureinput-r1/analysis/mini-replay-depth-read-no-blend/semantic-gate-real-texture-r1/semantic-gate-summary.json; traces/app-d3d9-3dmark05-post-visualfix-frame60-60-2-rank2-texture-r1/analysis/mini-replay-depth-read-no-blend/semantic-gate-real-texture-r1/semantic-gate-summary.json; traces/app-d3d9-3dmark05-post-visualfix-frame60-60-2-rank3-texture-r1/analysis/mini-replay-depth-read-no-blend/semantic-gate-real-texture-r1/semantic-gate-summary.json; traces/app-d3d9-3dmark05-post-visualfix-frame60-60-2-rank4-texture-r1/analysis/mini-replay-depth-read-no-blend/semantic-gate-real-texture-r1/semantic-gate-summary.json; docs/perfomance/index-cache-locality/index-cache-locality-screenblend.10.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.19.md
---

# Final-Writer Replay Oracle Is Now an Automated Gate

**Question / hypothesis.** [[index-cache-locality-screenblend.10]] says the
only remaining large locality bucket is sample-visible and needs a
final-color/final-writer oracle before another Xcode capture. Can the current
real-texture mini-replay summaries be attached to the full perf gate so that
sample-visible locality is blocked by measured final-writer evidence, not just
by a generic "oracle required" note?

**Method.**

1. Add `--semantic-replay-summary-json` to
   `scripts/tools/summarize_3dmark05_perf_gates.py`.
2. Accept multiple `run_3dmark05_semantic_replay_gate.py`
   `semantic-gate-summary.json` files.
3. Sum their LRU32 movement by replay verdict:
   `fail-final-writer-hazard`, `masked-final-writer-hazard`, and
   owner-safe exact pass.
4. Emit `final-writer-replay-oracle` and propagate it into the implementation
   track and next-experiment queue.

```mermaid
flowchart TD
  Summaries["semantic-gate-summary.json\nrank1..rank4 real-texture replay"]
  Gate["final-writer-replay-oracle"]
  Summaries --> Gate
  Gate --> Fail{"any final color +\ncanonical final writer change?"}
  Fail -- "Yes" --> BlockFail["blocked-final-writer-hazard\nno locality Xcode spend"]
  Fail -- "No" --> Mask{"color exact but\ncanonical owner changed?"}
  Mask -- "Yes" --> BlockMask["blocked-owner-masked\ncolor exact is not an oracle"]
  Mask -- "No" --> Candidate["candidate-final-writer-safe\nvalidate wider replay"]
  BlockFail --> Queue["next experiment queue\nblocked-final-writer-replay"]
  BlockMask --> Queue
  Candidate --> Ceiling["compare safe LRU32\nagainst semantic ceiling"]
```

**Result.**

| Gate | Verdict | Evidence |
|---|---|---|
| `final-writer-replay-oracle` | `blocked-final-writer-hazard` | `4` replay summaries; fail LRU32 `-14,593`, color pixels `2`, owner pixels `7`, color+owner pixels `2`; masked LRU32 `-9,113`, owner pixels `878`; owner-safe LRU32 `0` |
| `overall` | `semantic-safe-locality-only` | keeps accepted opaque-depth locality, but the current same-input real-texture replay does not prove final-writer safety |
| depth-read proxy rows | `blocked-final-writer-replay` | the queue now says the current same-input real-texture replay reports `blocked-final-writer-hazard` and the current backend-shape/PSO candidates are rejected |

```mermaid
stateDiagram-v2
  [*] --> SampleVisibleLocality
  SampleVisibleLocality --> ReplaySummaries: attach real-texture semantic gate
  ReplaySummaries --> FinalWriterFail: rank1 changes color + owner
  ReplaySummaries --> OwnerMasked: rank2..4 color exact, owner changed
  FinalWriterFail --> Blocked
  OwnerMasked --> Blocked
  Blocked --> NewOracle: different final-color/final-writer proof
  Blocked --> NewBackend: primitive-order-preserving backend mechanism
  NewOracle --> [*]
  NewBackend --> [*]
```

**Verdict.** Accepted as a gate/tooling improvement. The current rank1..rank4
real-texture replay set does not provide the oracle needed to spend another
locality Xcode capture. It proves the opposite: the only large sample-visible
movement includes a true final-writer hazard, and the color-exact rows are
owner-masked rather than owner-safe. The next locality experiment must provide
a different final-color/final-writer oracle with enough safe LRU32 movement, or
the next GPU experiment should switch to a non-reorder backend denominator
mechanism.

**Related.** [[hidden-backend-storage]] ·
[[index-cache-locality-screenblend.10]] ·
[[mini-replay-bisection-texture.02]] ·
[[mini-replay-bisection-texture.04]] ·
[[mini-replay-bisection-texture.05]] ·
[[mini-replay-bisection-texture.06]] ·
[[hidden-backend-storage-shape.19]] · [[overview-3dmark05-gt1]].
