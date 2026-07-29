---
domain: index-cache-locality
workload: 3DMark05 GT1
title: "Index-Cache Locality — the only accepted production GPU win - Historical Log"
type: domain-log
status: historical
updated: 2026-07-08
source: docs/perfomance/index-cache-locality/index.md
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/overview.md
---

# Index-Cache Locality — the only accepted production GPU win - Historical Log

> Full historical detail moved from the former top-level `index-cache-locality.md` overview.
> Keep [overview](overview.md) current and compact; append long-running chronology,
> rejected paths, and detailed synthesis here only when it is not already captured in
> one-experiment leaf documents.

---

# Index-Cache Locality — the only accepted production GPU win

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Scope & question

This domain owns the **one accepted production optimization** of the whole GT1
investigation: a cached, semantic-safe post-transform index-cache reorder that
lowers VS invocations — and therefore the hidden TVB / parameter-buffer write
bucket — for **opaque depth-writing triangle lists**. It covers the production
flag `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` (+ `_MIN_GAIN_PCT`), the
explicit-tolerance-only screen-blend variant `DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE`,
min-gain threshold tuning, the no-mutate identity scouts that fed candidate
selection, the CPU-cost optimization of the cache path, and the remaining `50/2`
bottleneck triage. The mechanism behind why this works is proven separately by
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md): TVB write ≈ `VS invocations × per-vertex VSOut bytes`.

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | Hot indexed rows carry large reducible post-transform LRU32 locality (ceiling) | accepted (model) | index-cache-locality-opaque.01 |
| H2 | A cached LRU32 reorder for **opaque depth-writing** triangles reduces Xcode VS invocations, VS write, and GPU time on target rows | **accepted (production WIN)**; refreshed frame60 proxy rows now have attached opaque proof input | index-cache-locality-proofinput.01, [index-cache-locality-opaque.08](index-cache-locality-opaque.08.md), index-cache-locality-opaque.07, index-cache-locality-opaque.03 |
| H3 | The opt-in is correctly scoped (opaque rows only; 50/2 untouched) and is not a no-op on the current tree | accepted | index-cache-locality-opaque.02, index-cache-locality-opaque.04 |
| H4 | The CPU side-effect can be cut and attributed without changing candidate selection (dense adjacency, LRU32-only, source-resolve split; remaining build owner is candidate selection volume) | accepted | index-cache-locality-cpucost.03, index-cache-locality-cpucost.05, index-cache-locality-cpucost.06, index-cache-locality-cpucost.08, index-cache-locality-cpucost.09, index-cache-locality-cpucost.10 |
| H5 | A faster lookup structure or simpler pool lookup scan reduces lookup CPU | rejected | index-cache-locality-cpucost.02, index-cache-locality-cpucost.07 |
| H6 | The screen-blend index-cache reduces 50/2 / 60/2 VS invocations / write | mechanism confirmed on target rows, but current full-frame proof is **not promotable**: `60/2` GPU `-3.55%`, VS invocations `-10.76%`, VS write `-10.84%`, while top GPU fails `+0.97%`; follow-up shows this is target-only movement plus non-target replay variance, not reordered-cache mutation on `60/0+60/1`; semantic ceiling is now automated and says no more locality Xcode spend until a final-color/final-writer oracle or broader safe selector exists | [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md), [index-cache-locality-screenblend.09](index-cache-locality-screenblend.09.md), index-cache-locality-screenblend.08, index-cache-locality-screenblend.07, index-cache-locality-screenblend.06, [index-cache-locality-screenblend.05](index-cache-locality-screenblend.05.md), [index-cache-locality-screenblend.04](index-cache-locality-screenblend.04.md), index-cache-locality-screenblend.03 |
| H7 | Lowering min-gain `10→0` improves Xcode counters | rejected (weaker avg gain, no hardware movement) | index-cache-locality-mingain.01 |
| H8 | Texture/fragment material is the first-order owner of the residual `50/2` cost | rejected; owner stays hidden vertex-stage storage | index-cache-locality-triage.01 |
| H9 | A simple fixed candidate frontier cap cuts candidate-select CPU | rejected; cap 32/64 reduces slots but not select CPU | index-cache-locality-cpucost.11 |
| H10 | A heap-backed lazy priority frontier cuts candidate-select CPU while preserving quality | rejected; scored work falls but CPU and miss32 regress | index-cache-locality-cpucost.12 |
| H11 | A cached-vertex-count bucketed selector cuts candidate-select CPU | rejected; scored work falls but bucket maintenance regresses CPU | index-cache-locality-cpucost.13 |
| H12 | A unique-count upper-bound pre-gate avoids building impossible candidates | rejected; rejects 76 candidates but candidate CPU regresses | index-cache-locality-cpucost.14 |
| H13 | A missing persistent rejected verdict is the remaining opaque-depth CPU blocker | rejected; already implemented and amortizing | index-cache-locality-cpucost.15 |
| H14 | A missing draw-shape prefilter before reordered-index-cache lookup is the remaining CPU blocker | rejected; non-scope draws are already gated before lookup | [index-cache-locality-cpucost.16](index-cache-locality-cpucost.16.md) |
| H15 | Strict no-duplicate LRU simulation inside the candidate builder improves candidate quality or CPU enough to change the default | rejected; candidate miss32 worsens by `+46`, CPU gain is too small/noisy | index-cache-locality-cpucost.17 |
| H16 | Selected `60/2 depth-read + no-alpha-blend` windows can use `cache-opt-lru32` without same-input color movement | mixed; rank2/3/4 color-exact but owner-masked | [mini-replay-bisection-semantic.02](../mini-replay-bisection/mini-replay-bisection-semantic.02.md) had `0` changed pixels with clear and D24X8 depth input, LRU32 `-14,593`; [mini-replay-bisection-texture.02](../mini-replay-bisection/mini-replay-bisection-texture.02.md) rank1 real-texture replay changes `2` pixels and canonical primitive-id replay shows `7` final-writer pixels changed; [mini-replay-bisection-texture.04](../mini-replay-bisection/mini-replay-bisection-texture.04.md) rank2 real-texture replay has `0` changed pixels, LRU32 `-5,937`, and `809` owner-changed pixels; [mini-replay-bisection-texture.05](../mini-replay-bisection/mini-replay-bisection-texture.05.md) rank3 has `0` changed pixels, LRU32 `-2,452`, and `52` owner-changed pixels; [mini-replay-bisection-texture.06](../mini-replay-bisection/mini-replay-bisection-texture.06.md) rank4 has `0` changed pixels, LRU32 `-724`, and `17` owner-changed pixels |
| H17 | Who owns the residual `50/2` / refreshed `60/2` (`~1.49–1.60 GiB` hidden) GPU cost | **OPEN** | index-cache-locality-triage.01, [hidden-backend-storage-shape.04](../hidden-backend-storage/hidden-backend-storage-shape.04.md), [mini-replay-bisection-semantic.02](../mini-replay-bisection/mini-replay-bisection-semantic.02.md) |
| H18 | Primitive-conflict owner/depth/UV metrics can make scoped depth-read reorder production-safe | rejected; only final color separates fail/pass | [mini-replay-bisection-texture.07](../mini-replay-bisection/mini-replay-bisection-texture.07.md) |
| H19 | Existing D3D9 occlusion query can be reused as the scoped depth-read no-final-color oracle | rejected; it resolves primitive count; diagnostic Metal visibility is separate sample-count triage, not final-color proof; current zero-sample rows are not the hot LRU owner | [mini-replay-bisection-texture.08](../mini-replay-bisection/mini-replay-bisection-texture.08.md), [mini-replay-bisection-texture.09](../mini-replay-bisection/mini-replay-bisection-texture.09.md), [mini-replay-bisection-texture.10](../mini-replay-bisection/mini-replay-bisection-texture.10.md) |
| H20 | The current continued experiment is still useful after the bottleneck model is known | accepted as proof gating; refreshed frame60 opaque proof passed, screen-blend was demoted from "missing movement" to "target movement pass, aggregate GPU fail, likely replay variance", and semantic/visibility joins now prevent another low-ROI locality gputrace without a final-color oracle or broader safe selector | [mini-replay-bisection-texture.11](../mini-replay-bisection/mini-replay-bisection-texture.11.md), [index-cache-locality-screenblend.09](index-cache-locality-screenblend.09.md), index-cache-locality-proofinput.01, [index-cache-locality-opaque.08](index-cache-locality-opaque.08.md), index-cache-locality-screenblend.08, index-cache-locality-screenblend.07 |
| H21 | Positive Metal visibility can promote scoped depth-read locality | rejected; rank2 has positive samples with no final color, while rank1/rank3 are both positive but fail/pass diverge | [mini-replay-bisection-texture.11](../mini-replay-bisection/mini-replay-bisection-texture.11.md) |
| H22 | The current perf gate can keep the locality semantic ceiling attached to the next Xcode queue | accepted (gate) | [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md) (`locality-semantic-ceiling=oracle-required`; color-exact/zero-sample buckets are too small, sample-visible bucket needs final-color/final-writer proof) |
| H23 | Current real-texture semantic replay summaries provide the missing final-writer oracle | rejected by gate | [hidden-backend-storage-shape.20](../hidden-backend-storage/hidden-backend-storage-shape.20.md) (`final-writer-replay-oracle=blocked-final-writer-hazard`; fail LRU32 `-14,593`, masked LRU32 `-9,113`, owner-safe LRU32 `0`) |
| H24 | Gate/class/primitive-shape telemetry can classify the remaining opaque-depth CPU side-effect before another Xcode spend | accepted; frame60 hot rows have `102/102` candidate gate-pass and `0` gate-fail, so the blocker is valid candidate construction/cache lookup, not hot-row failed-gate waste | [index-cache-locality-cpucost.18](index-cache-locality-cpucost.18.md) |
| H25 | The commit-replay offload absorbs the candidate/lookup CPU tax at FPS parity | accepted; with `DXMT9_OFFLOAD_COMMIT_REPLAY=1` the opt-in runs at `1999 -> 1980` presents (`-0.95%`, noise) while applying `333,283` reordered-buffer hits (`~168` draws/present, `67` buffers created) — the `~0.24ms/present` build/select/lookup cost lands on worker/encode threads with idle headroom, so the runtime promotion blocker is gone; remaining formal gate is a paired offload+opt-in `.gputrace` proof | [index-cache-locality-offload-synergy.19](index-cache-locality-offload-synergy.19.md) |
| H26 | The paired offload+opt-in `.gputrace` proof passes every promotion gate | accepted; frame60 finalizer verdict "all requested requirement gates were satisfied" vs the June baseline: target rows `60/0+60/1` GPU `-7.39%`, VS buffer write `-16.54%`, VS invocations `-14.12%` (identical to the historical proof), `175` candidate draws with miss32 `582,658 -> 450,807`; stable-frame, PSO-attribution, and coverage gates all pass, so the opt-in's evidence is complete — its default remains coupled to the offload because the CPU tax is only absorbed there | [index-cache-locality-offload-promotion-proof.20](index-cache-locality-offload-promotion-proof.20.md) |

## Verification methods

- **`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` + `_MIN_GAIN_PCT=10`** — the
  production opt-in (wrapper `--optimize-opaque-depth-index-cache
  --optimize-opaque-depth-index-cache-min-gain-pct 10`). Submits cached LRU32
  reordered IBs only for opaque depth-writing triangle lists clearing the gain gate.
- **`DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE` + `_MIN_GAIN_PCT`** —
  explicit-tolerance-only variant for strict screen-blend rows; destination-dependent
  and allowed only when the run carries an exact/`lsb1` semantic-image policy.
  Historical screen-blend numbers are not enough for a fresh promotion; the
  current automated gate must carry screen-blend Xcode movement, semantic CSV,
  and a top-hot GPU decrease; target-row movement alone is not sufficient.
- **`--require-opaque-depth-index-cache-proof`** — production preset: stable-frame
  gates, target cache-opt/effective LRU32 decrease, positive target reordered-cache
  hits, target VS write/invocation decrease. Used instead of the diagnostic
  generic-LRU gate because production cached-prelookup leaves generic telemetry zeroed.
- **`--require-screen-blend-cache-proof` / `--require-target-index-cache-opt-miss32-decrease`,
  `--require-target-reordered-index-cache-hits`, `--require-target-vs-invocations-decrease`** —
  the screen-blend / target-row mechanism gates.
- **Proof-input discipline for current frame60 rows** — high hidden-backend
  proxy size alone does not promote a row. The refreshed opaque-depth rows now
  have attached Xcode movement proof for `60/0+60/1`; the current screen-blend
  rank-1 window now has a same-input `lsb1` mini-replay semantic CSV and target
  `60/2` Xcode movement, but failed the aggregate top-GPU gate. See
  index-cache-locality-proofinput.01 for the current recipes and proof
  status.
- **LRU32 telemetry** — `indexed_cache_opt_candidate_*_miss32`,
  `candidate_miss_delta32` (production uses miss32; miss16/64 are `0` in
  fast-measure), plus `indexed_cache_opt_candidate_gate_{pass,fail}`,
  `indexed_cache_opt_candidate_{opaque_depth,screen_blend}_draws`, and
  `indexed_cache_opt_candidate_primitive_bucket_*` for the CPU side-effect
  shape.
- **Reordered-cache hit counters** — `reordered_index_cache_{lookups,hits,rejected_hits,created}`,
  `runtime_applied_draws` prove the path is active and conservatively scoped.
- **No-mutate identity scout** — `DXMT9_MEASURE_INDEX_REUSE=1` +
  `--measure-index-cache-opt-candidate` emit per-draw identity / candidate ceiling
  without mutating order, feeding candidate selection.
- **Diagnostic candidate-builder variants** —
  `--index-cache-candidate-frontier-cap`,
  `--index-cache-candidate-lazy-frontier`,
  `--index-cache-candidate-bucketed-select`,
  `--index-cache-candidate-strict-lru`, and
  `--index-cache-candidate-upper-bound-gate` are hypotheses, not default
  changes. Judge them first with no-gputrace CPU/miss32 counters, then require
  same-input image proof or a stable visual gate; `v0.0.3` PNG diffs are useful
  for broad corruption triage but not raw pixel-percent correctness gates.

## Experiment dependency graph

```mermaid
flowchart TD
  Identity1["index-cache-locality-identity.01\nno-mutate draw identity\n664 rows (tooling)"]
  Identity2["index-cache-locality-identity.02\ngputrace-backed scout\nhidden 2236.981MiB"]
  Preflight["index-cache-locality-opaque.01\ncandidate ceiling\ntop3 LRU32 -24.39%"]
  OptNo["index-cache-locality-opaque.02 / .04\nopaque opt-in no-gputrace\n102 applied, 50/2 untouched"]
  OptXcode["index-cache-locality-opaque.03\nXcode replay\ntarget GPU -8.33%, global +0.84%"]
  Smoke["index-cache-locality-opaque.05\nnon-diag smoke\nGPU -0.16%, CPU +25.37%"]
  Cpu1["index-cache-locality-cpucost.01\nCPU split: index_setup owner"]
  Cpu2["index-cache-locality-cpucost.02\nlookup fast-path REJECTED"]
  Cpu3["index-cache-locality-cpucost.03\ndense adjacency -58.87%"]
  Cpu4["index-cache-locality-cpucost.04\nsetup scope correction"]
  Cpu5["index-cache-locality-cpucost.05\nprefix repair + CPU gap refresh\nsource resolve flat"]
  Cpu6["index-cache-locality-cpucost.06\nno-encoder default-policy smoke\nCPU +215.6ms"]
  Cpu7["index-cache-locality-cpucost.07\npool lookup single-scan REJECTED\nlookup +3.4ms"]
  Cpu8["index-cache-locality-cpucost.08\ncandidate build split\nselect 75.4%, adjacency 19.1%"]
  Cpu9["index-cache-locality-cpucost.09\ncache-position table\nselect -7.1ms, total CPU flat"]
  Cpu10["index-cache-locality-cpucost.10\nselect volume\n302k calls, 2.06M scored"]
  Cpu11["index-cache-locality-cpucost.11\nfrontier cap REJECTED\nslots -20.3%, select +5.0%"]
  Cpu12["index-cache-locality-cpucost.12\nlazy heap REJECTED\nscored -81%, select +21%"]
  Cpu13["index-cache-locality-cpucost.13\nbucketed select REJECTED\nscored -72.6%, select +32.5%"]
  Cpu14["index-cache-locality-cpucost.14\nupper-bound gate REJECTED\n76 skipped, candidate CPU +8.5%"]
  Cpu15["index-cache-locality-cpucost.15\npersistent rejected verdict refresh\n401k rejected hits / 143 misses"]
  Cpu16["index-cache-locality-cpucost.16\ndraw-shape prefilter audit\nnon-scope already skipped"]
  Cpu17["index-cache-locality-cpucost.17\nstrict LRU diagnostic REJECTED\nmiss32 +46, total CPU +36.9ms"]
  Cpu18["index-cache-locality-cpucost.18\ngate/class/bucket scout\nframe60 pass 102/fail 0\ncandidate+lookup 0.33ms/present"]
  ScopedDepth["mini-replay-bisection-semantic.02/.texture.02/.texture.04/.texture.05/.texture.06/.texture.07\n60/2 depth-read/no-blend\nrank1 visible fail\nrank2/3/4 color-exact owner-masked\nnon-color selector rejected"]
  OcclusionGate["mini-replay-bisection-texture.08/.09\nD3D9 query primitive-count only\nMetal visibility scout sample-count only"]
  VisibilityPositive["mini-replay-bisection-texture.11\npositive visibility is not final color"]
  FastGate["index-cache-locality-opaque.06\nfast-measure smoke\nGPU -0.58%, setup +309ms"]
  Proof"index-cache-locality-opaque.07\nFAST-MEASURE PROOF\nGPU -9.50%, target VS inv -14.12%\nmechanism via [tvb-mechanism-proof"]
  ProofCurrent["index-cache-locality-opaque.08\npre-proof gate noted missing input\nsuperseded by proofinput.01"]
  ProofInput["index-cache-locality-proofinput.01\ncurrent experiment purpose\nopaque proof passed\nscreen-blend demoted"]
  SbScout["index-cache-locality-screenblend.02\n50/2 scout: 66 hits"]
  SbXcode["index-cache-locality-screenblend.03\n50/2 GPU -4.64%\nmechanism proof"]
  SbGate["index-cache-locality-screenblend.04\ncombined GPU -11.89%\nexplicit lsb1 only"]
  SbGateCurrent["index-cache-locality-screenblend.05\ncurrent gate missing movement/proof CSV\nneeds-screen-blend-gate-input"]
  SbGateSemantic["index-cache-locality-screenblend.06\ncurrent semantic lsb1 prepared\nmissing-xcode-movement"]
  SbFull["index-cache-locality-screenblend.07\ncurrent full proof\ntarget pass, top GPU fail"]
  SbVariance["index-cache-locality-screenblend.08\nfailure split\ntarget-only + replay variance"]
  SbCeiling["index-cache-locality-screenblend.09\nsemantic ceiling\nno new locality Xcode without oracle"]
  SbCeilingGate["index-cache-locality-screenblend.10\nsemantic ceiling automated gate\noracle-required"]
  SbOrder["index-cache-locality-screenblend.01\nindex-order opt -3.35% GPU\nVS write unchanged"]
  MinGain["index-cache-locality-mingain.01\nmin-gain 0 REJECTED\nglobal GPU +0.04%"]
  Triage["index-cache-locality-triage.01\n50/2 owner OPEN\ntexture not owner"]

  Identity1 --> Identity2
  Identity2 -->|"candidate input"| Preflight
  Preflight -->|"ceiling for"| OptNo
  OptNo --> OptXcode
  OptXcode --> Smoke
  Smoke -->|"CPU cost"| Cpu1
  Cpu1 --> Cpu2
  Cpu2 -->|"real owner"| Cpu3
  Cpu3 --> Cpu4
  Cpu4 --> Cpu5
  Cpu5 --> Cpu6
  Cpu6 --> Cpu7
  Cpu7 --> Cpu8
  Cpu8 --> Cpu9
  Cpu9 --> Cpu10
  Cpu10 --> Cpu11
  Cpu11 --> Cpu12
  Cpu12 --> Cpu13
  Cpu13 --> Cpu14
  Cpu14 --> Cpu15
  Cpu15 --> Cpu16
  Cpu16 --> Cpu17
  Cpu17 --> Cpu18
  Cpu3 --> FastGate
  FastGate --> Proof
  Proof --> ProofCurrent
  ProofCurrent --> ProofInput
  OptXcode -->|"50/2 left"| SbScout
  SbScout --> SbXcode
  SbXcode --> SbGate
  SbGate --> SbGateCurrent
  SbGateCurrent --> SbGateSemantic
  SbGateSemantic --> SbFull --> SbVariance --> SbCeiling --> SbCeilingGate --> ProofInput
  SbGate --> Triage
  Triage --> ScopedDepth
  ScopedDepth --> OcclusionGate --> VisibilityPositive
  SbOrder -.->|"earlier order-only attempt"| SbScout
  Proof -->|"threshold tuning"| MinGain
  ScopedDepth -->|"scoped only"| OpenOwner["residual 50/2 / 60/2\nhidden vertex/tiler storage"]

  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  class Preflight,OptNo,OptXcode,Smoke,Cpu1,Cpu3,Cpu4,Cpu5,Cpu6,Cpu8,Cpu9,Cpu10,FastGate,Proof,Identity1,Identity2 accepted
  class Cpu2,Cpu7,Cpu11,Cpu12,Cpu13,Cpu14,Cpu15,Cpu16,Cpu17,MinGain,OcclusionGate rejected
  class ScopedDepth accepted
  class Cpu18,SbCeilingGate accepted
  class SbScout,SbXcode,SbGate,SbOrder,Triage,OpenOwner,ProofInput open
  class SbFull,SbVariance,SbCeiling rejected
```

## Results synthesis

**Settled.** This domain is the payoff of the whole GT1 investigation. After
almost every other hypothesis was rejected as "not the first-order owner," the
single safe, real GPU win is **reducing VS invocations via post-transform index
locality on opaque depth-writing triangles**. The chain is closed: the no-mutate
identity scouts (index-cache-locality-identity.01, index-cache-locality-identity.02)
exposed per-draw shape and the candidate ceiling
(index-cache-locality-opaque.01, top-3 LRU32 `-24.39%`); the opt-in proved
correctly scoped to opaque rows with `50/2` untouched
(index-cache-locality-opaque.02, index-cache-locality-opaque.04); and the
fast-measure Xcode proof (index-cache-locality-opaque.07) PASSED every strong
gate — top GPU `-9.50%`, target rows `50/0+50/1` GPU `-18.39%`, VS invocations
`536,583→460,839` (`-14.12%`), VS write `-16.79%`, with attribution showing the
**primary mover is invocation count, not bytes per invocation**. The refreshed
post-stream/IB frame60 proof (index-cache-locality-proofinput.01) then
reattached that mechanism to current rows `60/0+60/1`: target GPU
`13.800ms→12.331ms` (`-10.64%`), VS invocations `536,583→460,839` (`-14.12%`),
VS write `646.173MiB→537.842MiB` (`-16.77%`), and top-3 GPU
`33.614ms→32.501ms` (`-3.31%`). That is exactly the prediction of
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md). The CPU side-effect was understood and cut (dense
adjacency / LRU32-only, candidate CPU `-58.87%` in
index-cache-locality-cpucost.03), while the lookup-structure rewrite and the
min-gain-0 relaxation were both rejected as non-improvements.

**Open.** Two things remain. (1) The opt-in stays **opt-in**, not a shared `perf`
default, because index setup still adds meaningful candidate/lookup + draw-path CPU
cost; the narrow source-resolve counter showed base IB resolve is not the owner
(index-cache-locality-cpucost.04, refreshed in
index-cache-locality-cpucost.05 after repairing the direct-prefix wrapper;
bounded without diagnostic rows in index-cache-locality-cpucost.06 at
`encode_draw_cpu_ms +215.588ms`, with source-resolve still flat).
The first follow-up implementation attempt, simplifying the pool lookup scan,
was rejected because it did not reduce the explicit lookup bucket
(index-cache-locality-cpucost.07). The next attribution split
(index-cache-locality-cpucost.08) shows the remaining candidate-build owner
is not source index read/write:
`encode_draw_index_cache_candidate_select_cpu_ms=99.187ms` is `75.4%` of
`encode_draw_index_cache_candidate_build_cpu_ms`, while adjacency is `19.1%`.
The follow-up cache-position table (index-cache-locality-cpucost.09) cut the
select bucket to `92.121ms` without changing miss32/created counts, but total
`encode_draw_cpu_ms` stayed flat. The remaining CPU problem is now candidate
rescoring volume, not raw cache-position lookup: index-cache-locality-cpucost.10
measured `302,538` select calls and `2,061,493` scored candidates with no skipped
stale slots. A hard frontier cap was then rejected in
index-cache-locality-cpucost.11: cap 32 cut scored slots by `20.33%`, but
`encode_draw_index_cache_candidate_select_cpu_ms` still increased
`91.635ms→96.232ms`, proving that bounding the worst-case vector width is not
enough. The first heap-backed lazy frontier was also rejected in
index-cache-locality-cpucost.12: it cut scored work by `80.97%`, but
`candidate_select_cpu_ms` regressed `91.635ms→111.247ms` and candidate LRU32
misses worsened `418,033→434,791`. A viable follow-up needs a cheaper
domain-specific frontier or candidate construction change, not a generic heap.
The next attempt, a cached-vertex-count bucketed selector
(index-cache-locality-cpucost.13), kept quality neutral and cut scored work
by `72.61%`, but still regressed select CPU `91.635ms→121.378ms` because
`190,647` bucket moves replaced the scan work. That closes the generic active-
frontier family for now: the next CPU path must reduce candidate calls or avoid
building low-value candidates, not maintain a smarter candidate container. The
first such pre-gate attempt (index-cache-locality-cpucost.14) used the
theoretical bound `candidate_miss32 >= unique` to skip impossible candidates.
It did reject `76` candidates and preserved the accepted cache set
(`reordered_index_cache_created=67`), but unique-count work moved
`encode_draw_index_cache_original_measure_cpu_ms` `15.146ms→24.301ms` and total
candidate CPU `152.117ms→165.050ms`, so this form is also rejected. A
post-visualfix refresh then checked the persistent verdict idea directly:
index-cache-locality-cpucost.15 shows rejected verdict caching is already
implemented and active (`401,681` rejected hits against `143` cold misses).
The follow-up code audit ([index-cache-locality-cpucost.16](index-cache-locality-cpucost.16.md)) then rejects a
missing broad draw-shape prefilter as the next blocker: non-scope draws are
already gated before `findReorderedIndexBuffer()`, so the `687,387` lookups are
eligible-key decisions, not unrelated draw traffic. The next local builder
diagnostic (index-cache-locality-cpucost.17) normalized the simulated LRU
miss path to the same no-duplicate update used by the LRU32 measurement helper,
but that also failed as a default-change reason: candidate miss32 worsened
`418,033→418,079`, the candidate CPU delta was only `-5.082ms`, and total
`encode_draw_cpu_ms` regressed `+36.930ms` in the no-gputrace run. The
gate/class/bucket follow-up ([index-cache-locality-cpucost.18](index-cache-locality-cpucost.18.md)) then
classifies that remaining CPU tax: the frame60 hot encoder scope has `198`
lookups, `102` applied hits, `96` rejected hits, `0` misses/creates, and
`102/102` measured candidates pass the gain gate with all candidates in the
opaque-depth scope. The LRU32 delta is still useful (`460,019 -> 333,936`,
`-27.41%`), but the hot rows are not wasting CPU on failed candidates. Whole-run
counters still show `169/58` pass/fail and `227` opaque-depth candidates, with
candidate+lookup CPU about `0.331ms/present` and index setup rising to
`0.724ms/present` in the contextual r18 comparison. Future CPU work therefore
needs either a cheaper valid-candidate construction path, cheaper per-draw
lookup/setup amortization, or more semantic-safe GPU payoff, not another
per-candidate measurement pass, not basic rejected-key caching, not a broad
"avoid non-eligible draws" gate, not hot-row failed-gate prefiltering, and not
local LRU miss-path normalization.
(2) The dominant remaining frame owner is row
`50/2` — depth-read, screen-blend/standard-alpha/blend-off, textured, large indexed
geometry — whose `~1.49–1.58 GiB` cost is hidden vertex/tiler/parameter storage, not
texture/fragment (index-cache-locality-triage.01). The screen-blend cache *can*
reduce it (index-cache-locality-screenblend.03, `50/2` GPU `-4.64%`) but is
allowed only as an **explicit exact/`lsb1`** artifact
([index-cache-locality-screenblend.04](index-cache-locality-screenblend.04.md)): the combined opaque+screen-blend run
improves top GPU `-11.89%`. The current post-stream/IB proof attempt has now
closed the old "missing movement" gap. The rank-1 same-input mini-replay lowers
replay LRU32 `52,865 -> 38,272` (`-27.6%`) and the `lsb1` image gate changes
only `33 / 786,432` pixels with max delta `1` and SSIM `1.000000`
(index-cache-locality-screenblend.06). The full `gputrace` proof then shows
target `60/2` movement: GPU `19.184ms -> 18.503ms` (`-3.55%`), VS invocations
`642,001 -> 572,933` (`-10.76%`), and VS write `981.159MiB -> 874.767MiB`
(`-10.84%`). However, non-target hot rows `60/0+60/1` move
`13.800ms -> 14.800ms` (`+7.25%`), so the top-GPU gate fails
`32.984ms -> 33.302ms` (`+0.97%`). Follow-up row telemetry shows the reordered
cache path applied only to `60/2` (`103` lookups, `66` hits, `37` rejected
hits), while `60/0+60/1` had no reordered-cache lookups and unchanged
invocations/write/draw/geometry. That demotes screen-blend from "missing proof
input" to "target mechanism confirmed, aggregate proof failed by non-target
replay variance" (index-cache-locality-screenblend.08,
index-cache-locality-screenblend.07). The follow-up semantic ceiling
projection ([index-cache-locality-screenblend.09](index-cache-locality-screenblend.09.md)) makes the Xcode budget
decision explicit: rank2-4 color-exact owner-masked windows sum to only
`-9,113` LRU32 delta (estimated `-0.071ms`), rank1-4 still include a visible
hazard and only reach `-23,706` LRU32 (estimated `-0.186ms`), while the
sample-visible bucket is large enough in principle (`-180,840` LRU32, estimated
`-1.416ms`) but still lacks final-color/final-writer proof.
The automated gate [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md) now carries this
budget decision into the current full report as
`locality-semantic-ceiling=oracle-required`: color-exact/zero-sample locality is
too small for another Xcode capture, while the only large bucket is
sample-visible and still needs final-color/final-writer proof.
The post-visualfix `60/2` class proxy then found depth-read/no-blend windows with
similar locality ceilings. The first selected
two-draw window ([mini-replay-bisection-semantic.02](../mini-replay-bisection/mini-replay-bisection-semantic.02.md)) cuts replay LRU32 misses
`52,865 -> 38,272` (`-27.6%`) and was exact with clear depth and captured D24X8
depth, but the rank-1 real-texture follow-up
([mini-replay-bisection-texture.02](../mini-replay-bisection/mini-replay-bisection-texture.02.md)) changes `2` pixels with max delta `5`,
and canonical primitive-id replay shows `7` pixels where the final writer
changes. The rank-2 follow-up ([mini-replay-bisection-texture.04](../mini-replay-bisection/mini-replay-bisection-texture.04.md)) is
color-exact with real textures and LRU32 `19,131 -> 13,194` (`-31.0%`), but
still changes canonical primitive ownership at `809` pixels. Rank 3
([mini-replay-bisection-texture.05](../mini-replay-bisection/mini-replay-bisection-texture.05.md)) repeats the color-exact owner-masked
shape with LRU32 `11,398 -> 8,946` (`-21.5%`) and `52` owner pixels changed.
Rank 4 ([mini-replay-bisection-texture.06](../mini-replay-bisection/mini-replay-bisection-texture.06.md)) completes the queued lower-ranked
set with the same color-exact owner-masked shape: LRU32 `4,237 -> 3,513`
(`-17.1%`) and `17` owner pixels changed. That rejects broad exact/`lsb1`
production promotion for this state class. The primitive-conflict selector
scout ([mini-replay-bisection-texture.07](../mini-replay-bisection/mini-replay-bisection-texture.07.md)) also rejects the cheap
non-color-threshold family: owner-count, depth, UV, and projected-texcoord
ranges overlap between rank1 fail and rank2-4 exact passes. The visibility/cache
join ([mini-replay-bisection-texture.10](../mini-replay-bisection/mini-replay-bisection-texture.10.md)) also rejects current no-sample rows
as the hot LRU owner: zero rows are only `-2,016` of `-182,856` LRU32 delta.
The visibility-positive semantic join ([mini-replay-bisection-texture.11](../mini-replay-bisection/mini-replay-bisection-texture.11.md))
then rejects the remaining positive-sample shortcut: rank2 is sample-positive
with no final color, while rank1 and rank3 are both sample-positive but split
visible fail versus visible exact-pass.
The full perf gate now attaches the same real-texture semantic replay summaries
directly through [hidden-backend-storage-shape.20](../hidden-backend-storage/hidden-backend-storage-shape.20.md) and emits
`final-writer-replay-oracle=blocked-final-writer-hazard`: rank1 is a real
final-writer fail (`-14,593` LRU32), rank2-4 are color-exact but owner-masked
(`-9,113` LRU32), and owner-safe LRU32 is `0`.
Until a real final-color/final-writer policy or a new non-reorder backend
mechanism exists, residual `50/2` / `60/2` is the open frontier.

## How to run
Every experiment here is a 3DMark05 GT1 run via the standard wrapper. The
production opt-in is the opaque-depth index-cache path; capture a `.gputrace` with
it enabled, then finalize with the production proof preset:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix idx-cache --frame 60 \
  --optimize-opaque-depth-index-cache --optimize-opaque-depth-index-cache-min-gain-pct 10 \
  --allow-partial-stable-frame-proof --timeout 420
# screen-blend (explicit exact/lsb1 policy only): --optimize-screen-blend-index-cache \
#   --optimize-screen-blend-index-cache-min-gain-pct 10
# no-mutate identity scout: --measure-index-reuse --measure-index-cache-opt-candidate --no-gputrace

# After Xcode exports encoder counters:
bash scripts/tools/finalize_3dmark05_perf_probe.sh --suffix idx-cache --frame 60 \
  --baseline-joined traces/<baseline>/analysis/frame60-xcode-dxmt-joined-summary.csv \
  --require-opaque-depth-index-cache-proof --allow-partial-stable-frame-proof
```

The exact per-experiment flags live in each leaf's `**Method.**` field. See
`agents/rules/environment_variables.rules.md` for env-var meanings and
`agents/rules/metal_debugging.rules.md` for the full workflow.

## Cross-references
- [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) — proves *why* fewer VS invocations reduce GPU time; this domain is the production application of that mechanism.
- [hidden-backend-storage](../hidden-backend-storage/index.md) — supplies the TVB/parameter-storage cost model and the residual `50/2` hidden-write bucket this domain cannot yet reach.
- [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) — the reverse-triangle / min-index / cache-aware reorder scouts that motivated a *cached, gated* reorder instead of naive order changes; `sort-min-index` was rejected here.
- [index-reuse-measurement](../index-reuse-measurement/index.md) — the `DXMT9_MEASURE_INDEX_REUSE` / LRU32 cache-miss telemetry the candidate gate is built on.
- [mini-replay-bisection](../mini-replay-bisection/index.md) — consumes the no-mutate identity rows; the real-input replay path needed to settle the open `50/2` semantic-tolerance question.
- [mini-replay-bisection-semantic.02](../mini-replay-bisection/mini-replay-bisection-semantic.02.md) — selected `60/2` depth-read/no-blend window with exact white-texture/depth-input replay output.
- [mini-replay-bisection-texture.02](../mini-replay-bisection/mini-replay-bisection-texture.02.md) — rank-1 selected window with real texture inputs; exact/`lsb1` promotion rejected.
- [mini-replay-bisection-texture.04](../mini-replay-bisection/mini-replay-bisection-texture.04.md) — rank-2 selected window with real texture inputs; final color exact but owner-masked.
- [mini-replay-bisection-texture.05](../mini-replay-bisection/mini-replay-bisection-texture.05.md) — rank-3 selected window with real texture inputs; final color exact but owner-masked.
- [mini-replay-bisection-texture.06](../mini-replay-bisection/mini-replay-bisection-texture.06.md) — rank-4 selected window with real texture inputs; final color exact but owner-masked.
- [mini-replay-bisection-texture.07](../mini-replay-bisection/mini-replay-bisection-texture.07.md) — primitive-conflict selector scout rejects simple non-color thresholds.
- index-cache-locality-proofinput.01 — explains why the current experiment matters, records the refreshed opaque proof result, and records the screen-blend demotion proof.
- index-cache-locality-screenblend.06 — current screen-blend same-input `lsb1` semantic input prepared.
- index-cache-locality-screenblend.07 — current full screen-blend proof: target `60/2` movement passes, aggregate top-GPU gate fails.
- index-cache-locality-screenblend.08 — row-level follow-up: screen-blend applies only to `60/2`; `60/0+60/1` regression is GPU-time-only replay variance.
- [index-cache-locality-screenblend.09](index-cache-locality-screenblend.09.md) — semantic ceiling projection: no more locality gputrace/Xcode spend without a final-color/final-writer oracle, broader safe selector, or non-reorder denominator mechanism.
- [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md) — automated semantic ceiling gate;
  emits `locality-semantic-ceiling=oracle-required` in the current full gate.
- [hidden-backend-storage-shape.20](../hidden-backend-storage/hidden-backend-storage-shape.20.md) — automated final-writer replay gate;
  attaches the current real-texture semantic summaries and blocks this
  sample-visible locality set before Xcode.
- [index-cache-locality-offload-synergy.19](index-cache-locality-offload-synergy.19.md) — commit-replay offload absorbs the opt-in's CPU tax at FPS parity; runtime promotion blocker removed.
- [index-cache-locality-offload-promotion-proof.20](index-cache-locality-offload-promotion-proof.20.md) — formal offload+opt-in promotion proof passed every gate (target GPU `-7.39%`, VS write `-16.54%`, VS invocations `-14.12%`).
- [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) — root priority DAG and synthesis (this is the accepted win it points to).

## Root 3DMark05 Map Detail Migration - 2026-07-08

Detail migrated from the former long-form root [3DMark05 overview](../overview-3dmark05-gt1.md) so that `index-cache-locality` owns its detailed synthesis while the root overview stays cross-domain only.

### From Central finding (read this first)

**The one accepted production win** is opaque-depth **index-cache
locality** ([index-cache-locality](index.md)): reordering indices for opaque
depth-writing triangles improves the post-transform vertex cache, which
lowers VS invocations, which linearly lowers TVB write. Historical target rows
proved GPU `-18.4%`, VS invocations `-14.1%`, VS write `-16.8%`; the refreshed
frame60 proof reattaches the same mechanism to current rows `60/0+60/1`
(`-10.64%` target GPU, `-14.12%` VS invocations, `-16.77%` VS write).
The current fast-measure implementation passes the strong Xcode proof gates,
but remains an opt-in rather than a shared `perf` default: the non-diagnostic
smoke still adds about `+216ms` of total encode-draw CPU / `+301ms` of
index-setup CPU over a 1440-present run, and the narrow source-resolve counter
shows the owner is cache/candidate/draw-path work rather than base
index-buffer lookup.


### From What is settled vs open

- Opaque-depth index-cache locality is a real, semantic-safe GPU win, but
  stays opt-in until the remaining index-setup CPU side-effect is reduced or
  amortized by a broader runtime gate. [index-cache-locality](index.md)

- Whether enough sample-visible locality can be made final-color/final-writer
  safe. The automated ceiling gate now rejects current color-exact/zero-sample
  buckets as too small for another Xcode capture, while leaving the large
  sample-visible bucket open only behind an oracle.
  [index-cache-locality-screenblend.10](index-cache-locality-screenblend.10.md)
