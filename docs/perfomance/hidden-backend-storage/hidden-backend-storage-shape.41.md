---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 41
title: GT2 Final R32F Pass Is Observationally Dead but Needs Cross-Chunk Scheduling
date: 2026-07-25
type: bottleneck-and-design-decision
status: accepted-structural-candidate
source: experiments/output/app-d3d9-3dmark05-gt2-r32f-subresource-liveness-source-frame279-r1-20260724/3dmark05-direct.log; experiments/output/app-d3d9-3dmark05-gt2-r32f-cross-chunk-dag-frame279-r1-20260725/result.json; traces/app-d3d9-3dmark05-gt2-r32f-cross-chunk-dag-frame279-r1-20260725/analysis/dag
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.39.md; specs/d3d9-renderer/spec.md
---

# GT2 Final R32F Pass Is Observationally Dead but Needs Cross-Chunk Scheduling

## Question

Is the final `2048x2048 R32F` pass merely a frame279 candidate, or does GT2
repeat a stable cross-frame pattern that can support a material DCE design?

## Whole-run liveness

The source-order run contains `503` target-bearing sequences (`seq=2..504`).
A canonical-alias scan reports:

| Observation | Count |
|---|---:|
| target-bearing sequences | `503` |
| sequences with two R32F write runs | `503` |
| first target access classified as overwrite | `503` |
| first target access classified as read | `0` |

The compressed per-sequence patterns include boundary Clears that can retain
the preceding sequence tag because the direct telemetry updates `seq` on the
first sequence-bearing record, not on every bind. Exact boundary ordering was
therefore checked independently with consecutive alias-aware DAGs.

The current-default observation run dumps frames/chunks `278`, `279`, and
`280`. All three post-opt DAGs are identical in the relevant region:

| Frame | Post-opt passes | Canonical R32F order | Draw references |
|---:|---:|---|---|
| `278` | `16 Render + 1 Present` | `P0 Clear -> P1 Read x133 -> P2 Clear` | `142 -> 159 -> 140` |
| `279` | `16 Render + 1 Present` | `P0 Clear -> P1 Read x133 -> P2 Clear` | `142 -> 159 -> 140` |
| `280` | `16 Render + 1 Present` | `P0 Clear -> P1 Read x133 -> P2 Clear` | `142 -> 159 -> 140` |

The first pass produces the R32F value consumed by the main-color pass. The
second R32F pass is the last access to canonical texture
`0x20000010000003e` in each chunk, and the next chunk begins by clearing the
same canonical subresource before any read.

The final pass also writes shared depth
`0x300000100000004`. In every consecutive DAG, pass `2` Clears that depth and
pass `3` immediately Clears it again; passes `4..8` continue with full Clears.
The final pass therefore leaves neither a live color value nor a live depth
value.

## Side-effect and scheduling checks

The current-default run completes `531` Presents. It records zero
query-issue draw-run breaks, zero readback draw-run breaks, zero render
readback splits, and no query/lock flags in the three target DAGs. This removes
the observed query/readback objection for GT2, but a general implementation
must keep those protections.

The remaining blocker is the encode window:

| Runtime observation | Value |
|---|---:|
| encode-dequeue samples | `531` |
| ready depth total / maximum | `531 / 1` |
| ready-depth `>1` samples | `0` |
| open-CB carrier activity | `0` |

The normal queue path dequeues one slot and calls
`FrameGraphBackend::onChunkReady` immediately. The backend builds and
optimizes only that slot. Existing DCE correctly preserves pass `2`: its depth
write is overwritten later in the chunk, but its persistent R32F color write
has no same-chunk overwrite proof.

The opt-in open-CB carrier can expose a batch through
`sessionLookaheadSources`, but that lookahead currently proves only attachment
Store actions. More importantly, the measured ready queue never contains a
second chunk when the encoder dequeues. A ready-only two-chunk planner would
therefore have zero GT2 volume.

## Performance scope

The earlier Xcode capture assigns `23.789-26.359ms` to each dominant R32F
pass. Removing one pass is directionally `15.9-17.6%` of the historical
`149.701ms` full-frame replay, implying a theoretical `1.19-1.21x` GPU-time
speedup. Applied only as an upper bound to the current `~8.15 FPS` band, that
is approximately `9.7-9.9 FPS`.

Those numbers are not a promotion result. The capture predates alias-hazard
normalization, so only its per-pass workload shape remains usable. A real
implementation needs a new alias-aware Xcode capture and runtime A/B.

## Design decision

This is an accepted structural optimization candidate, not a legal current
optimization and not a GT2-specific heuristic.

The safest first prototype is a queue-level two-chunk proof window:

1. Hold chunk `N` until `N+1` is available, or fail open when a flush,
   map/readback, query, producer wait, shutdown, or queue-pressure condition
   requires progress.
2. Build one canonical alias/resource history across both chunks.
3. Drop a pass from `N` only when every written subresource is unread after
   the write and the first access in `N+1` is a full overwrite.
4. Preserve Present, query, lock, debug-marker, resource-retention, and
   completion-waterline semantics.
5. Model the new hold/release state in TLA+ before a runtime A/B.

This design deliberately pays up to one chunk of planning latency to obtain a
real proof. It must expose present-ordinal and producer-wait release signals;
blindly waiting for the next chunk can deadlock when the producer is itself
paced on completion.

Deferring only the candidate pass across Present could avoid the whole-chunk
hold, but it is a riskier first design. It must retain the old slot's draw
state and resource generations, replay before a later read, and keep map,
release, query, and completion semantics correct after the original Present
has already been submitted.

## Verdict

GT2 is not at a demonstrated GPU-work ceiling. One of its two dominant R32F
passes is dead across the measured workload and is large enough to matter.
The opportunity cannot be reached by enabling current per-chunk DCE or by
reusing ready-only batching. The next meaningful experiment is a
fail-open, TLA-backed cross-chunk proof prototype followed by exact
framebuffer and alias-aware Xcode validation.
