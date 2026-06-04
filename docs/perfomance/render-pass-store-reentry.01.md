---
domain: render-pass-store
subcategory: reentry
order: 01
title: Same RT/Depth Re-entry Measurement Run
date: undated
type: measurement
status: accepted
source: specs/perfomance.plan.md#L4705-L4778
---

# Same RT/Depth Re-entry Measurement Run

**Question / hypothesis.** Does the repeated RT/depth re-entry seen in the Xcode
frame120 trace show up as a measurable run-level counter, and how much of the
estimated tile Store/Load preservation budget does it own?

**Method.** Added frame-local render-pass chain counters keyed on `(RT0 handle,
depth handle, sample count)`, reset at Present:
`render_pass_same_key_adjacent` (immediate repeat), `render_pass_same_key_reentry`
(same key reopened after an intervening different key), and
`render_pass_same_key_reentry_preservation_bytes` (estimated `2 * (RT0 + depth
surface bytes)` Store+Load footprint — a candidate budget, not a hardware
counter). Validation run:
`experiments/output/app-d3d9-3dmark05-pass-reentry/{dxmt9.log,result.json}`.

**Result.** GT1-class run: `present_encoded=1260`, `draw_calls=916519`,
`render_pass_begin=14684`, `gpu_command_buffer_time_ms=3625.665`. New pass-chain
signal:

| Counter | Value | Reading |
|---|---:|---|
| `render_pass_tile_preservation_bytes` | `167714828288` | total preservation budget (~167.73 GB) |
| `render_pass_same_key_adjacent` | `0` | NOT an immediate duplicate-reopen bug |
| `render_pass_same_key_reentry` | `2788` | ~2.21 re-entries per present |
| `render_pass_same_key_reentry_preservation_bytes` | `62344134656` | ~62.34 GB = 37.2% of tile preservation |

**Verdict.** Accepted as a real, actionable signal. Same RT/depth re-entry owns a
large fraction (~37.2%) of the estimated Store/Load preservation budget. Because
`same_key_adjacent=0`, the fix is not "avoid closing/reopening the same pass
immediately" — it requires legal pass coalescing/ordering or a color/depth
live-out `StoreActionDontCare` proof for contents dead before the next same-key
re-entry. This is the P1 GPU-memory track, secondary to the P0 hidden-backend
bucket.

**Related.** [[render-pass-store]] · next: [[render-pass-store-dontcare.01]]
(the DontCare design this measurement motivated) · [[baselines]] (frame120
reference where `rt=0x...c,depth=0x...` re-entry costs 24.643 ms / 73.32%) ·
[[hidden-backend-storage]] (the P0 owner this P1 track sits behind).
