---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality"
type: leaf
status: current
updated: 2026-08-30
source: experiments/output/app-d3d9-3dmark05-cpu-ready-intent-h239-off-gt2-20260830; experiments/output/app-d3d9-3dmark05-cpu-ready-intent-h239-on-gt2-20260830; experiments/output/app-d3d9-3dmark05-cpu-ready-present-window-h239-on-gt2-20260830
related: docs/perfomance/present-pacing/present-pacing-cpu-ready-active-head-locality.238.md; specs/backend/encode-scheduling/gap.md; specs/verification/spec.md
---

# Present-Pacing #239 - Replay-FIFO Intent Is Safe but Does Not Recover Tape Locality

## Result

The replay worker now exposes a bounded next-source intent only for an
already-adopted immediate raw FIFO successor. Successful Direct replay stages
the raw ordinal; Ready publication advances it to the exact generation-stamped
predecessor/source identity; non-Direct disposition cancels it before effects.
The queue wake algebra, native truth tables, and
`CpuReadyActiveHeadLookahead.tla` cover adoption, cancellation, replacement,
hold/restore, seed preservation, and progress. Empty FIFO and immediate control
items never promise a source.

The implementation is safe, but the production GT2 stream does not use it.
Every intent-selected and intent-successor counter is zero. Whenever a promise
could have participated, the successor was already Ready and the ordinary
Ready path won. The strict same-build GT2 pair therefore still rejects Tape
promotion:

| Metric | Tape off | Tape on | Delta |
|---|---:|---:|---:|
| Presents | 1,858 | 1,839 | - |
| Command buffers / Present | 3.9995 | 4.0131 | +0.34% |
| Render passes / Present | 15.7691 | 15.9505 | +1.15% |
| Tile preservation MiB / Present | 103.537 | 108.089 | +4.40% |
| GPU command-buffer ms / Present | 2.012 | 2.135 | +6.13% |

Both lanes completed with zero GPU command-buffer errors and chunk rejects.
`DXMT9_CPU_READY_TAPE` remains default off. The command-buffer, pass, and tile
gates take precedence over a single-run FPS value, so a wider GT1/GT3/SFIV
promotion matrix is not justified.

## Exact attribution

The Tape-on run conserves all 3,383 retained-head attempts:

| Outcome | Count |
|---|---:|
| Held | 2,195 |
| Reject: Present head | 955 |
| Reject: no Writing successor or usable intent | 233 |
| Total | 3,383 |

Fresh holds account for 1,671 and active-session holds for 524. The replay
worker armed 5,053 intents and canceled 310, but
`retained_head_intent_selected` and `retained_head_intent_successor` remain
zero. This refutes missing producer identity as the remaining GT2 locality
owner.

The residual instead appears after a bounded replay window exhausts while a
render pass still has unresolved future Store actions:

- `render_pass_no_lookahead_suffix_exhausted = 11,568` (`6.290/Present`)
- late unknown color/depth actions = `4,655 + 6,383 = 11,038`
- late resolution to Clear/DontCare = `268`
- late resolution to Store = `10,770`

The late-action accounting is exact: `11,038 = 268 + 10,770`. Thus 97.6% of
the actions whose future was unknown at the bounded edge eventually require a
Store under the actual replay order. This is no longer a local wait or source
identity problem; it is a deterministic planning-equivalence problem between
the bounded streaming view and the legacy frame-wide replay view.

## Rejected boundary widening

A bounded experiment admitted the final Present into the same selected window
and submitted the planned fragments only after the whole window completed. It
still measured `4.0080` command buffers, `15.9534` passes, and `108.342 MiB` of
tile preservation per Present, while suffix exhaustion only moved to
`5.870/Present`. The change widened a semantic boundary without recovering
locality and was reverted.

## Next decision

The remaining honest alternatives are:

1. seal and plan a deterministic frame-wide replay interval before encoding,
   matching legacy future knowledge but surrendering most producer/replay/
   encode overlap; or
2. define and prove an incremental replay/store-action equivalence contract
   that permits bounded publication without changing pass actions or command-
   buffer shape.

More local retention, longer heuristic waits, or another source-identity
carrier is not supported by the H239 evidence.
