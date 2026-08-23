---
domain: present-pacing
workload: SFIV Benchmark (D3D9)
title: "Present-Pacing #205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait"
type: leaf
status: current
updated: 2026-08-23
outdated: evidence-missing
source: experiments/output/app-d3d9-sfiv-benchmark-latency1-probe-r3-quiet-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-novsync-probe-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-force-frag-color-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-texwhite-probe-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-shader-attrib-r1-20260712/result.json; traces/app-d3d9-sfiv-benchmark-20260712-gpuintervals
related: docs/perfomance/present-pacing/present-pacing-sfiv-scene-pass-stall.204.md; docs/perfomance/overview-sfiv.md
---

> Historical artifact notice: this leaf is retained for context, but its cited source evidence is unavailable (`outdated: evidence-missing`).

# Present-Pacing #205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait

## Question

Leaf .204 left the ~88ms scene-pass instances attributed to a GPU-side wait
with cross-frame WAR as the leading hypothesis. Which is it — wait or work —
and what is the wait/work target?

## Probe chain (all cooled, perf profile, ~1,500-present baselines)

| Probe | presents | scene-pass slow cluster | Verdict |
|---|---|---|---|
| baseline (lat 3) | 1,500-1,560 | 26/112 at 88-96ms | reference |
| `DXMT9_MAX_FRAME_LATENCY=1` (quiet desktop, r3) | 1,500 | 36/117, same ~88ms cluster | **WAR-with-in-flight-frame REFUTED** — 1-deep pipeline changes nothing |
| `DXMT9_DISABLE_VSYNC=1` | 1,560 (`present_schedule_immediate=1560`) | — | present pacing REFUTED — fully immediate presents change nothing |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1` | **5,462 (3.6x)**, CB p50 110→1.2ms | **0/444 slow** — cluster gone | **real fragment shader work CONFIRMED** |
| `DXMT_FORCE_TEXTURE_WHITE=1` | 1,620, CB p50 227ms (worse) | — | texture-fetch cost refuted; cost is ALU/shader-structure (white samples likely defeat an early-out and add work) |

A first latency-1 run (r1) appeared to universalize the stalls; its trace
showed 4.9s of WindowServer fragment bursts (11ms CB trains inside the scene
windows) from the IDE streaming this session's own output — an environmental
confound; the quiet rerun (r3) normalized it. Baseline stall windows contain
only SFIV's own CB spans back-to-back.

## The pass and its shaders

The slow pass (`rt=0x300000100000006`, `depth=0x300000100000005`, `enc=1`) has
an **invariant shape in every frame**: `programmable_draws=11`,
`primitives=22` — eleven fullscreen textured quads (the ink/paper effect
composite), five shader variants (last-draw PS rotates among
`0x26d0eb834f89ee27`, `0x3fff60885678e08d`, `0xd36fb2fb94c44dae`,
`0xdd8e0f36dd0f816b`). Dumped MSL for all scene-pass FS is modest (12-15KB,
1-iteration init loops, 1-5 samples, a few transcendentals) — no static
pathology. Same shaders + same coverage swinging 0.2ms ↔ 88ms (440x) means
the cost is **data-dependent** (effect constants/texture content on pulsing
frames — denormal/special-value arithmetic is the classic suspect).

## Verdict

- SFIV's frame wall is real, data-dependent fragment-shader cost in the
  11-fullscreen-quad effect composite pass, active on 23-31% of frames.
- Proven FPS headroom: **+264% presents** (1,560 → 5,462) with fragment cost
  removed — an upper bound (force-frag-color is correctness-invalid).
- RT versioning/renaming is dead as a candidate (targets a refuted mechanism).

## Xcode + follow-up probes (same day)

Three `.gputrace` captures (frames 505/509/512, wine capture-layer wrapper)
all replay the scene pass at 104-110ms. Xcode findings, with a caveat: the
replay ran on an active desktop, and the Fragment timeline shows 7x ~11.4ms
"External Process" interleaves inside the pass (≈80ms) — replay GPU-time and
byte counters are contaminated by desktop work, so per-line SHAPE is trusted,
absolute splits are not.

- Top Shaders: one `dxmt9_fs` variant owns 96.02% of replay GPU time; the
  per-line profiler attributes 93.35% to the function-signature line
  (= prologue), split Sync Wait Memory 32.7% / Memory Load 19.7% / ALU
  Integer 26.6% / ALU Float 0.15% — **per-fragment cbuf loads, not math**.
  Line-pattern match identifies the file as
  `translated-fs-shader-2796994317804760615` (PS `0x26d0eb834f89ee27`) — a
  trivial 2-instruction shader whose prologue/epilogue still chases
  `abuf.psConsts`/`abuf.ffpPs` and loads alpha-test/fog state per fragment.
- Live discriminators: `DXMT9_ARGBUF_DIRECT_CBUF=1` no change (1,322
  presents) — the argbuf-table indirection per se is not the cost.
  `DXMT_DISABLE_ALPHA_TEST=1 DXMT_DISABLE_FOG=1` (strips the generated tail
  and its `ffpPs` loads) **halves CB GPU time (p50 110 -> 60.5ms)** at
  unchanged presents (1,383 — the wall shifts toward app CPU).

## Final mechanism

The scene pass's fullscreen effect quads run shaders whose per-fragment cost
is dominated by serialized dependent cbuf loads (pointer-chase into
`PsConsts`/`FfpPsConsts`, no compiler constant-preload), ~10-100x the actual
math. The frame-to-frame bimodality is invocation-count driven: on
effect-idle frames the quads cover ~nothing (11 near-empty draws, 0.2ms); on
effect frames they cover the screen (~6.5M invocations at 720p with 6.9x
overdraw) and the load latency bill comes due (~88ms).

## Fix directions (shader-codegen)

1. Compile-time shader variants keyed on alpha-test/fog enables — proven
   worth ~half the pathological GPU time by the strip probe.
2. Hoist/vectorize the remaining per-fragment cbuf loads (single wide
   preamble load, or direct `[[buffer]]` bindings verified to hit the
   constant-preload path — note the current direct-cbuf scout does not cover
   the FFP-tail loads).
3. Gate any change on an SFIV visual anchor and a no-gputrace presents A/B.
