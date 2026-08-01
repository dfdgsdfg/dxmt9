---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 11
title: The SWVP Hoist Is +29% GT2 Scene FPS — And CPU Removed Converts 1:1 To Wall Clock
date: 2026-08-01
type: experiment-run
status: accepted-runtime-win
source: experiments/output/app-d3d9-3dmark05-swvp2-base-r{1,2,3,4}; experiments/output/app-d3d9-3dmark05-swvp2-head-r{1,2,3,4}; experiments/output/app-d3d9-3dmark05-swvp2-aa-{1,2}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.09.md; docs/perfomance/frame-lifecycle.md
---

# The SWVP Hoist Is +29% GT2 Scene FPS — And CPU Removed Converts 1:1 To Wall Clock

**Question / hypothesis.**
[.09](state-churn-encode-append-decomposition.09.md) measured `12.02 ms/present`
— `22.6%` of the GT2 frame — spent in two SWVP fallback probes that read the
whole index buffer twice before checking whether software vertex processing is
enabled. It deliberately made no FPS claim, because this repository has
established twice that CPU removed and wall clock are different currencies and
that the conversion ratio is unidentified, believed somewhere in `[0.3, 1.5]`.
A removal this large is the first candidate big enough to measure that ratio
instead of assuming it. Does it convert?

**Method.** `83a0b085` hoists the shared conjunct
`if (!softwareVertexProcessing_) return S_FALSE;` into all eight probes.
Interleaved A/B, GT2, `perf`, `--keep-frontmost`, `--no-gputrace`, `120 s`
timeout, `180 s` caffeinated cooldown, both sides staged from prebuilt
worktrees via `--build-root`. Order is **ABBA then BAAB**, 4 runs/side —
position sums `26` vs `26`, so no side sits systematically in the warmer slot.
An **A/A pair** (same build, both sides of the harness) runs first. Metric is
frame-sampled scene fps per the GT2 rule (H231), reported as both the harmonic
mean `1000/mean(wall_ms)` and the median.

**Result.**

| pos | run | frames | mean ms | fps (hmean) | fps (median) |
|---:|---|---:|---:|---:|---:|
| 1 | A/A-1 | 1,123 | `55.00` | `18.18` | `18.72` |
| 2 | A/A-2 | 1,162 | `53.23` | `18.79` | `19.38` |
| 3 | base r1 | 1,167 | `53.34` | `18.75` | `19.32` |
| 4 | head r1 | 1,491 | `41.65` | **`24.01`** | `24.80` |
| 5 | head r2 | 1,507 | `41.58` | **`24.05`** | `25.00` |
| 6 | base r2 | 1,163 | `53.48` | `18.70` | `19.14` |
| 7 | head r3 | 1,486 | `42.01` | **`23.80`** | `24.27` |
| 8 | base r3 | 1,129 | `54.65` | `18.30` | `18.94` |
| 9 | base r4 | 1,107 | `54.73` | `18.27` | `18.85` |
| 10 | head r4 | 1,478 | `42.35` | **`23.61`** | `24.32` |

| | base | head | delta |
|---|---:|---:|---:|
| scene fps (harmonic mean) | `18.503` ± `0.254` | `23.869` ± `0.201` | **`+29.00%`** |
| scene fps (median) | `19.065` | `24.599` | **`+29.03%`** |
| frame time | `54.05 ms` | `41.90 ms` | **`-12.15 ms/present`** |

Welch `t = 33.1`. Within-side CV is `1.38%` (base) and `0.84%` (head) against a
`26%` separation — the two populations do not overlap at any run.

## The conversion ratio is 1.01

`.09` predicted `12.02 ms/present` of producer CPU removed. The frame got
**`12.15 ms/present`** shorter. **`c = 1.01`.**

That is the top of the `[0.3, 1.5]` band and the first time this ratio has been
measured rather than assumed. It should not be generalized: it holds *here*
because GT2's producer thread is the frame-setting thread
([frame-lifecycle](../frame-lifecycle.md) §3) and the removed work sat directly
on it, inside the D3D9 call, with nothing else contending for the freed time.
A removal on the encode worker (which idles `~39 ms/present`) or on the GPU
would convert at nothing like `1.0`. What this establishes is that **for
producer-thread CPU on a producer-bound workload, `c ≈ 1`** — which retires the
"CPU removed is not wall clock" caveat for that specific class and leaves it
standing everywhere else.

## Why this is a real speedup and not a rendering regression

A `+29%` frame-rate jump is exactly what a dropped-work bug looks like. Three
checks say it is not:

| check | base | head |
|---|---:|---:|
| **scene duration** (GT2 is a fixed animation) | `60.6-62.2 s` | `62.1-62.7 s` |
| **draws per present** | `1,663` | `1,666` (`+0.2%`) |
| `gpu_command_buffer_errors` | `0` | `0` |

Both sides render the **same fixed-length timeline** and issue the **same number
of draws per frame**; head simply fits `~28%` more frames into it. Nothing was
skipped. The zero error count also covers the out-of-bounds-index concern the
hoist opens (below): no malformed draw reached Metal in `~10M` indexed draws.
Image metrics stay in band (`mean_luma 37.4-39.2`, `variance 1500-1594`), though
the two sides capture at different animation frames so that is a smoke check,
not a pixel gate.

## The harness had to be rebuilt first, and the first attempt was a phantom

**The first A/B run was invalid and its result was `+29.7%`** — a number that
happens to sit almost on top of the real one, obtained for entirely the wrong
reason. The baseline worktree had been configured with meson's *default*
buildtype (`debugoptimized`, `-O2`, `debug=True`, asserts live) against head's
`release` (`-O3`, `NDEBUG`). It measured build flags. Two tells were visible and
missed: the staged x86 `d3d9.dll` was `5.3 MB` against head's `938 KB`, and
`winemetal.so` differed by `6.5%` despite the commit not touching it. Those runs
are quarantined as `experiments/output/INVALID-buildconfig-swvp-ab-*`.

A result that cannot be distinguished from a configuration artifact is not
evidence, even when it later turns out to agree. The corrected harness therefore
preflights, and aborts loudly rather than degrading:

- **build-config parity** across all three build dirs (`90/90/81` options), plus
  `winemetal.so` byte-identity — the strongest single tell, since the hoist
  cannot touch it.
- **both trees ninja-clean.** `--build-root` does *not* merely copy artifacts:
  staging calls `install_heroic_wine.sh`, which runs `ninja`. An unclean tree
  compiles mid-experiment. This also cost the second attempt its first run, when
  the background shell lacked `~/llvm-mingw/bin` on `PATH` and staging failed.
- **per-run gates** on `result.json` (`status`, `profile == perf`, `>= 800`
  scene frames). The first harness used `grep || true`, which would have turned
  a crashed run into a silently missing sample.
- **A/A pair first.** It would have caught the config bug immediately, and it
  supplies the noise floor: `18.18` / `18.79`, a `3.26%` spread — wider than
  either side's own within-side CV, and the first run of a session reads low.

Thermal drift is present and symmetric: base falls `18.75 -> 18.27` across the
session and head `24.01 -> 23.61`, both `-2.6%`. The balanced order cancels it.

## What this does not settle

- **The behaviour change is still unpinned.** The hoist skips validation that
  ran before the applicability test, so on a hardware-VP device a malformed
  indexed draw is now recorded instead of rejected with `D3DERR_INVALIDCALL`.
  `appendDrawIndexedPrimitiveRecord` has no equivalent bounds check and
  `recordedDrawIndexedPrimitives` hands `indexCount`/`indexBufferOffset`
  straight to Metal, which does not bounds-check index fetches — so the failure
  mode is out-of-bounds GPU index reads, not a clean error. Retail D3D9 does not
  validate this outside the debug runtime, so the new behaviour is the more
  faithful one, and `gpu_command_buffer_errors = 0` across `~10M` draws says
  nothing pathological happens in practice. But `.09` required this be pinned by
  a test and it still is not.
- **The conformance evidence proves the wrong direction.**
  `visual_mvp_software_vp_policy` runs its draws on a SWVP device, where the
  hoisted gate evaluates true and falls through to bit-identical code. It
  validates the no-op direction and **cannot detect the behaviour change at
  all**. `test_visual_max_index16_draw_policy` is the one test near the changed
  path and it accepts both `S_OK` and `D3DERR_INVALIDCALL`, so nothing pins the
  new outcome either.
- **The commit's safety enumeration is incomplete** — see
  [.09](state-churn-encode-append-decomposition.09.md), corrected: two further
  pre-`describe` failure escapes exist beyond the three first listed.
- **GT2 only.** GT1, GT3 and SFIV have different draw mixes and different
  producer-boundedness; neither the `29%` nor `c = 1.01` transfers.

**Scope.** Four runs per side plus two A/A, one machine, one thermal window,
`N=64`-free (no PE instrumentation). The `20-200 ms` sample filter is
data-dependent at the top edge, so a hitch tail could in principle cross it
asymmetrically; the median and harmonic mean agreeing to `0.03 pp` says it did
not here.

**Related.**
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[append-decomposition.10](state-churn-encode-append-decomposition.10.md) ·
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
