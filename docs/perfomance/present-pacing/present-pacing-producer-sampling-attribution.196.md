---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: producer-attribution
order: 196
title: Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm
date: 2026-07-08
type: xctrace-cpu
status: accepted-attribution
source: traces/app-d3d9-3dmark05-cpu-attrib-producer-r4-20260708/analysis/time-profile.xml; traces/app-d3d9-3dmark05-cpu-attrib-producer-r4-20260708/analysis/xctrace-cpu-thread-summary.md; experiments/output/app-d3d9-3dmark05-cpu-attrib-producer-r4-20260708/result.json; docs/perfomance/present-pacing/present-pacing-pe-const-overhead-cut.193.md
related: docs/perfomance/present-pacing/index.md; agents/rules/metal_debugging.rules.md
---

# Present-Pacing H196 - Producer sampling attribution (post-consolidation)

## Question

After the offload promotion, the producer thread owns the ~58ms frame wall
(worker idle `44ms/present`, GPU busy `3.3ms/present`). H193 showed our
arithmetic PE attribution is unreliable. What does non-perturbing external
sampling say the producer actually runs?

## Method

Healthy promoted-pair run (`r4`, presents `1980`, status pass), parallel
`xcrun xctrace record --template 'Metal System Trace' --all-processes`
(75s delay, 25s window), `--no-encoder-breakdown`. Producer thread
auto-selected by highest weight (`0x1943654`, `23.9s/25s` running, 0
blocked). Frames classified leaf-outward to the first attributable owner
because this Wine build is x86_64-under-Rosetta end to end: leaf PCs sit
in the translation cache (`0x2xxxxxxxx`), only stack return addresses
resolve to original modules.

Two invalid attempts are method lessons: `r1` ran with an encoder-breakdown
seq window covering the xctrace window — the encode thread saturated at
`25.5s/25s` and presents collapsed `2040 -> 660` (`132.4ms/present` encode
CPU), so never point per-encoder breakdown at a window whose CPU shares you
intend to read. `r2`/`r3` hit the startup flake back-to-back (5th/6th
occurrence) — `r3` produced its first concrete signature: `bridge_factory=53`
then death before device creation with
`libc++abi ... mutex lock failed: Invalid argument` at teardown.

## Producer decomposition (share of 23.9s window; ~58ms/present)

| Owner | Share | ms/present |
|---|---:|---:|
| Rosetta-translated guest code (game x86 + 32-bit PE DLLs incl. our `d3d9.dll`; host-side unsplittable) | `70.6%` | `~41` |
| Wine wow64 / win64-PE layer (`0x7ff8` returns; `5.9%` of total sits directly above `winemetal.so` bridge calls) | `21.7%` | `~12.6` |
| `wine.real` | `4.2%` | `~2.4` |
| `winemetal.so` unix side (nearest-owner; top symbols `dxmt9p_buffer_lock`, `thunk_wow64_dxmt9c_buffer_lock`, `dxmt9p_device_commit_chunk`, `CommandQueue::mapBuffer`, `retainWrappersForOffload`) | `2.2%` | `~1.3` |
| other native | `~1%` | `~0.7` |

Stack-contains view: `winemetal.so` appears on `11.2%` of producer time
(`~6.5ms/present`) — consistent with the counter-measured unix wall below.

## The named lever: buffer-lock bridge storm

`result.json` counters for the same run:

| Counter | /present |
|---|---:|
| `d3d9_buffer_lock_calls` (= `map_buffer_calls`, every lock crosses the bridge) | **`1,478.7`** |
| `d3d9_buffer_lock_readonly` | `1,466.7` (**99.2%**) |
| `d3d9_buffer_lock_managed_pool` / `_dynamic` | `1,467` / `1,469` |
| `d3d9_buffer_lock_full_resource` | `1,350` |
| `d3d9_buffer_lock_bytes` (= shadow bytes, re-shadowed per lock) | `14.78 MB` |
| `d3d9_buffer_lock_ms` (unix wall) | `5.005 ms` |
| `map_buffer_mutex_wait_ms` (producer contends with worker/encode) | `2.716 ms` |
| `offload_commit_app_cpu_ms` (for comparison) | `1.07 ms` |

The game issues ~`1,467` READONLY managed-pool full-buffer locks per present;
each one crosses PE->wow64->unix (`thunk_wow64_dxmt9c_buffer_lock` visibly
leafs in the profile), takes the map mutex against the offload worker/encode
threads, and re-copies the shadow. Direct unix wall is `~5ms/present`, plus a
share of the `12.6ms/present` wow64-transition bucket (`>=3.4ms` sits
directly above winemetal bridge frames).

## Verdict

The next dxmt9-owned bottleneck is the **readonly managed-pool buffer-lock
bridge path**, not further PE-record or commit tuning: estimated reachable
`5-9ms/present` (`+9-18%` FPS ceiling) by serving repeat readonly locks of
unmodified managed buffers without a bridge crossing (guest-visible cached
shadow + PE-side generation/dirty check), or minimally by removing the map
mutex contention and per-lock re-shadow for unchanged buffers. The remaining
`~41ms/present` of translated guest code needs guest-side decimated stats to
split game-vs-PE-d3d9, but the lock storm is strictly larger than anything
H192/H193 attributed to the PE const chain and is directly measurable by
paired scouts.
