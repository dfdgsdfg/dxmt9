---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 24
title: Crossing Decomposition — Lock Round-Trips, A 78 µs Getter, And Refcount Churn Over The Bridge
date: 2026-08-20
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--bridge-decomp-r2; experiments/output/app-d3d9-3dmark05--bridge-opcodes-r1
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.23.md
---

# Crossing Decomposition — Lock Round-Trips, A 78 µs Getter, And Refcount Churn Over The Bridge

**Question.** [.23](state-churn-encode-append-decomposition.23.md) attributed
16.8% of the game thread (~6.2 ms/present) to `winemetal.dll`, interpreted as
PE→unix crossing cost. Which calls own it?

**Method.** The `[dxmt9-bridge-perf]` per-class report existed but was
atexit-only, so a SIGKILLed GT2 run lost it (H231 in bridge form). Two small
bridge changes: `DXMT9_BRIDGE_PERF_PERIODIC_COMMITS` emits the cumulative
report every N `commit_chunk` crossings (commits are the trigger because
Present rides the chunk as a record and never appears as a standalone bridge
call — the first attempt triggered on present-class calls and never fired),
and a per-opcode bucket table reports the top rows as
`[dxmt9-bridge-perf-opcodes]`. GT2, promoted cadence, ~1,653 frames.

**Class picture** (bridge_total `7.6 ms/present`, consistent with [.23]'s
sampler view): resource class `5.20` (68%), commit_chunk `1.44`, present
`0.66`, shader `0.18`. The resource class hides two very different
populations, which the opcode table separates:

| opcode | calls/present | µs/call | ms/present | nature |
|---|---|---|---|---|
| `dxmt9c_buffer_unlock` | 21.7 | 73.5 | 1.59 | real round-trip |
| `dxmt9c_device_commit_chunk` | 15.9 | 93.5 | 1.49 | post-promotion seal residue |
| `dxmt9c_buffer_lock` | 21.7 | 52.5 | 1.14 | real round-trip |
| `dxmt9c_texture_get_surface_level` | 14.3 | 78.4 | **1.12** | **pure getter crossing at full round-trip price** |
| `dxmt9c_device_get_swap_chain` | 1.0 | 617.9 | 0.64 | getter + the [.17] fence residual |
| `dxmt9c_surface_lock_rect` | 0.5 | 1,333.8 | 0.62 | few calls, enormous each — own investigation |
| `dxmt9c_buffer_addref`+`release` | 663+663 | ~0.35 | 0.49 | **refcount churn over the bridge** |
| shader/texture addref+release | ~293×2 | ~0.4 | 0.19 | same churn class |
| `dxmt9c_factory_adapter_count` | 191.9 | 0.3 | 0.06 | a constant queried 192×/present |

The [.23] mystery of "one resource crossing per draw" resolves to the cheap
churn population: ~1,327 buffer addref/release crossings per present plus
adapter-count polling — not one expensive op per draw.

**The reducible ledger this opens** (semantics-preserving candidates):

1. **PE-side getter caching** — `GetSurfaceLevel` returns a stable object per
   (texture, level), `GetSwapChain` per index, adapter count is constant:
   ~`1.8 ms/present` of crossings that need not exist after first resolution.
2. **PE-local refcount aggregation** — forwarding every AddRef/Release across
   the bridge costs ~`0.7 ms/present`; a PE-side count that crosses only on
   0↔1 transitions removes nearly all of it.
3. Lock/unlock round-trips (`2.7 ms`) and `surface_lock_rect`'s `1.3 ms/call`
   remain the harder, semantics-laden share.

Items 1+2 total ~`2.5 ms/present` ≈ +6-7% GT2 fps if fully harvested, on the
producer-saturated critical path where savings have been shown to convert
([.21]). One methodological note for the record: two compile failures were
initially masked by piping `meson compile` through `tail -1` (pipeline exit
is tail's); always check the compile's own status.

**Verdict.** The crossing vein decomposes into a cacheable-getter lane
(~1.8 ms), a refcount-aggregation lane (~0.7 ms), and a lock round-trip lane
(~2.7 ms, hard). Next increments in value order: texture_get_surface_level
PE cache, buffer refcount aggregation, then the surface_lock_rect deep-dive.
