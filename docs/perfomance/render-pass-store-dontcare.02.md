---
domain: render-pass-store
subcategory: dontcare
order: 02
title: Color Next-Clear StoreActionDontCare Run
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L4779-L4835
---

# Color Next-Clear StoreActionDontCare Run

**Question / hypothesis.** Will the conservative color next-clear look-ahead proof
actually fire on GT1 and reduce the re-entry Store/Load preservation budget?

**Method.** First `StoreActionDontCare` implementation, conservative color
look-ahead: if the next later record that touches a color handle is a color Clear
of the *same* handle, the previous pass may use `StoreActionDontCare`. Any
intervening draw target, texture sample, present, readback, copy, stretch, or
color-fill blocks the proof. Color does not use the broader dead-at-end/no-present
proof (color surfaces are commonly presented/sampled/reused across chunks).
Validation: `tests/native/backend/render_pass_actions_spec.cpp` (allow case +
draw-target/texture-sample/present blocking cases). GT1 run:
`experiments/output/app-d3d9-3dmark05-color-next-clear-dontcare/`.

**Result.** Compared to the re-entry baseline ([[render-pass-store-reentry.01]]):

| Metric | Re-entry baseline | Color next-clear | Delta |
|---|---:|---:|---:|
| `present_encoded` | `1260` | `1260` | `0.00%` |
| `draw_calls` | `916519` | `916211` | same class |
| `render_pass_begin` | `14684` | `14694` | same class |
| `render_pass_store_action_dontcare` | `0` | `0` | **no GT1 hits** |
| `render_pass_tile_preservation_bytes` | `167714828288` | `167844290560` | same class |
| `render_pass_same_key_reentry` | `2788` | `2786` | same class |
| `render_pass_same_key_reentry_preservation_bytes` | `62344134656` | `62209916928` | same class |
| `gpu_command_buffer_time_ms` | `3625.665` | `3637.668` | same class |

**Verdict.** Rejected as a GT1 lever (kept as a safe general optimization). The
proof is correct and tested but fires `render_pass_store_action_dontcare=0` times
on GT1: the same-key re-entry is **preservation-before-load**, not
preservation-before-clear — contents are later Loaded, not discarded by a clear.
The next render-pass fix must attack pass ordering/coalescing under explicit
dependency checks, not expect simple `StoreActionDontCare` to fire.

**Related.** [[render-pass-store]] · prior: [[render-pass-store-dontcare.01]]
(the design) · next: [[render-pass-store-passchain.01]] (split that confirms most
switches change both attachments) · [[render-pass-store-reentry.01]] (baseline).
