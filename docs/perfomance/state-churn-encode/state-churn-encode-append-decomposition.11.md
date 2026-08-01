---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 11
title: The SWVP Hoist Is +29% GT2 Scene FPS — And CPU Removed Converts ~1:1 To Wall Clock
date: 2026-08-01
type: experiment-run
status: accepted-runtime-win
source: experiments/output/app-d3d9-3dmark05-swvp2-base-r{1,2,3,4}; experiments/output/app-d3d9-3dmark05-swvp2-head-r{1,2,3,4}; experiments/output/app-d3d9-3dmark05-swvp2-aa-{1,2}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.09.md; docs/perfomance/frame-lifecycle.md
---

# The SWVP Hoist Is +29% GT2 Scene FPS — And CPU Removed Converts ~1:1 To Wall Clock

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

## The conversion ratio is ~1.0, and not to three figures

`.09` predicted `12.02 ms/present` of producer CPU removed. The frame got
**`12.15 ms/present`** shorter. That is `c = 1.01` arithmetically, and quoting it
that way would be false precision on both sides of the ratio.

**The denominator is soft.** `12.02` came from the decimated instrument on the
*drawphase* build, one `N=64` run, in a different session whose frame
(`53.14 ms` instrumented) was itself faster than today's uninstrumented base
(`54.05`). `.09`'s own cross-rate spread is `1.4-2.2%` — taking its `N=16`
figure gives `c ≈ 0.99` — H213 bounds cross-site agreement at `±12%`, and
[.10](state-churn-encode-append-decomposition.10.md) records the neighbouring
`append` scope failing to replicate at fixed `N` by `17.7%`.

**The numerator is not purely removed CPU.** From these runs' own counters,
producer-side *waiting* also fell:

| ms/present | base | head |
|---|---:|---:|
| `offload_drain_fence_wait_ms` | `2.69` | `1.43` |
| `completion_wait_ms` | `3.83` | `3.67` |

`~1.4 ms/present` of the `12.15` is reduced blocking rather than removed work.
It is not a separable term — the producer waits less *because* it is faster —
but it means the two-digit agreement is partly offsetting components, not a
clean identity.

**Honest statement: `c ≈ 1.0`, uncertain by roughly `±0.1-0.15`.** That is still
the top of the `[0.3, 1.5]` band and still the first time the ratio has been
measured rather than assumed. It should not be generalized: it holds *here*
because GT2's producer thread is the frame-setting thread
([frame-lifecycle](../frame-lifecycle.md) §3) and the removed work sat directly
on it. A removal on the encode worker or on the GPU would convert at nothing
like `1.0` — and even on GT2, `c ≈ 1` only holds while the producer remains the
binding thread, which each successive removal moves closer to not being true.

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
skipped.

The zero error count is weaker evidence than first written here: the failure
mode the hoist opens is a *silent* out-of-bounds GPU index read, so `errors = 0`
shows no fault, not no out-of-bounds read. For GT2 the conclusion is safe —
it issues no malformed draws — but the argument does not generalize. Equally,
`draws/present` agreeing to `0.2%` is within same-build variance (the A/A pair
alone spans `1,661-1,667`), so it could not have detected a handful of
newly-admitted draws per frame. **No check in this A/B can rule out a subtle
rendering change**; that is what the conformance pin is for.
Image metrics stay in band (`mean_luma 37.4-39.2`, `variance 1500-1594`), though
the two sides capture at different animation frames so that is a smoke check,
not a pixel gate.

## The harness had to be rebuilt first, and the first attempt was a phantom

**The first A/B run was invalid and its result was `+29.7%`.** The baseline
worktree had been configured with meson's *default* buildtype
(`debugoptimized`, `-O2`, `debug=True`, asserts live) against head's `release`
(`-O3`, `NDEBUG`).

It is tempting to say it "measured build flags"; the data says otherwise. The
debugoptimized base ran `18.05-18.38 fps` against the valid release base's
`18.27-18.75` — **the build-flag confound was worth `~1.5%`, not `29%`**. The
phantom was mostly the real effect plus a small contamination. What made it
unusable was not its magnitude but that at the time nothing distinguished it
from a configuration artifact, and the two tells that should have raised the
alarm were visible and missed: Two tells were visible and
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
- **A/A pair first.** It validates harness symmetry and staging. It would *not*
  have caught the config bug — that was a worktree-configuration asymmetry, and
  a same-build A/A is structurally blind to it; only the parity preflight
  catches that. Its `18.18` / `18.79` spread (`3.26%`) is `n=2` and includes the
  cold first run of the session, so the right noise estimate for the delta is
  the within-side CV (`1.38%` / `0.84%`, positions 3-10), which is what the
  Welch `t` uses.

Thermal drift is present in both sides: base falls `18.75 -> 18.27` (`-2.6%`)
and head `24.01 -> 23.61` (`-1.7%`). Equal mean position (`26` vs `26`) cancels
a linear drift exactly. The `20-200 ms` filter excludes `6-10` tail samples per
base run against `4-6` per head run, which flatters base — so the reported delta
is if anything conservative.

## What this does not settle

- **The behaviour change is real, and now pinned.** The hoist skips validation that
  ran before the applicability test, so on a hardware-VP device a malformed
  indexed draw is now recorded instead of rejected with `D3DERR_INVALIDCALL`.
  `appendDrawIndexedPrimitiveRecord` has no equivalent bounds check and
  `recordedDrawIndexedPrimitives` hands `indexCount`/`indexBufferOffset`
  straight to Metal, which does not bounds-check index fetches — so the failure
  mode is out-of-bounds GPU index reads, not a clean error. Retail D3D9 does not
  validate this outside the debug runtime, so the new behaviour is the more
  faithful one, and `gpu_command_buffer_errors = 0` across `~10M` draws says
  nothing pathological happens in practice. `.09` required this be pinned by a
  test; it now is (above).
- **The conformance evidence was void, and is now fixed.** The structural point
  always stood — `visual_mvp_software_vp_policy` runs on a SWVP device, where
  the hoisted gate evaluates true and the code is bit-identical — but an attempt
  to add a real pin found the suite **could not observe any change to
  `d3d9.dll` at all**. Root cause: the builtin lane's PE DLLs are postprocessed
  to carry Wine's `"Wine builtin DLL"` signature, so Wine resolves them from
  `$WINE_ROOT/lib/wine/<arch>-windows/` regardless of the path `LoadLibrary` was
  given. Neither the exe-adjacent copy nor the prefix `system32` copy is ever
  loaded — and both md5-matched the build output, which is why checking them
  read as confirmation. Nothing in `run_d3d9_conformance.py` wrote the Wine-root
  file; it was last written by unrelated **3DMark wild-run staging**, so
  conformance runs silently tested whichever tree last ran a wild experiment.

  Fixed: `stage_builtin_pe_dlls()` in `run_d3d9_conformance.py` now stages the
  built DLLs into the Wine root and verifies the copy took. With that in place
  `visual_indexed_draw_out_of_range_hwvp_policy` is a **real pin**, verified in
  both directions: `D3D_OK` with the hoist, `D3DERR_INVALIDCALL` (`0x8876086c`)
  with the eight guards removed. That also independently confirms the behaviour
  change documented below is real.

- **The commit's safety enumeration is incomplete** — see
  [.09](state-churn-encode-append-decomposition.09.md), corrected: two further
  pre-`describe` failure escapes exist beyond the three first listed.
- **GT2 only.** GT1, GT3 and SFIV have different draw mixes and different
  producer-boundedness; neither the `29%` nor `c ≈ 1.0` transfers.

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
