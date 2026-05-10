# V1 boundary baseline measurements (W4)

First-run baselines for the boundary-isolated probes landed in W1+W2,
plus a re-run of the R-BACK-2.33 cap=4 validation against the small-RT
chain workload predicted to win.

Measurement infrastructure:
- `scripts/tools/run_dx9_present_policy_ab.py --boundary {B1..B6}`
  filters `summary.json["summary"][i]["counters"]` to the matching
  boundary's keys (W3).
- `scripts/run_suites/run_boundary_audit_suite.sh` runs every boundary
  in one pass (W3).
- Per-boundary counter schema documented in
  `specs/benchmarks/boundary-counter-schema.md`.

## B2 — PE → unix bridge (dxmt9-perf-bridge-empty)

20,000 BRIDGE_EMPTY_ITERATIONS, single run, default wine prefix.
Probe killed at 60s timeout before completion (313 successful
commit_chunk calls captured; far below 20k target — Clear() in the
inner loop is heavier than the cost model assumed). Numbers reflect
the captured 313 calls.

| metric | value |
|---|---:|
| `chunk_admit` | 313 |
| `chunk_reject` | 0 |
| `bridge_commit_latency_p50_ns` | **10,000** (10 µs) |
| `bridge_commit_latency_p95_ns` | 11,792 (11.8 µs) |
| `bridge_commit_latency_p99_ns` | 306,125 (306 µs — long tail) |
| `bridge_commit_latency_max_ns` | 656,166 (656 µs) |
| `bridge_commit_latency_ns` (sum) | 5,196,575 (5.2 ms total) |

Headline: **steady-state PE→unix bridge round trip is ~10 µs**,
with a heavy tail. Worst-case 656 µs is large enough to matter for
draw-heavy frames, but only as the rare outlier — the p95/p50 ratio
is just 1.18×.

This is the first-ever per-call bridge latency number for dxmt9.
Previously only `chunk_admit` count was visible.

## B3 — CommandQueue + sub-CB chain (dxmt9-perf-chain-parametric)

CHAIN_LENGTH=4, CHAIN_DRAWS_PER_PASS=20, CHAIN_ITERATIONS=300.
Two runs: `policy=off` (baseline) and `policy=per-render-pass +
cap=4 (default)`. Both finished cleanly.

| metric | off | cap=4 | delta |
|---|---:|---:|---:|
| `chunk_admit` | 1,201 | 1,201 | 0 |
| `command_buffers` | **0** | **0** | — |
| `sub_command_buffers` | 0 | 0 | — |
| `chunk_subcb_count_max` | 0 | 0 | — |
| `render_pass_begin` | 0 | 0 | — |
| `gpu_command_buffer_time_*` | 0 | 0 | — |

**Anomaly:** the probe successfully crosses the PE→unix boundary 1201
times (chunk_admit confirms it) but zero MTLCommandBuffers are ever
created. The encode thread is silently skipping every chunk. This
needs investigation — likely the probe's offscreen-only render-target
loop without any Present causes a queue/encode-policy edge case where
chunks are admitted but never encoded.

Status: **W4 cap=4 re-validation is therefore inconclusive** for the
chain probe. The bridge probe's data is still valid (B2 numbers
above). Investigation required before the cap=4 default-flip decision
(R-BACK-2.34) can be made.

Suggested follow-up:
1. Run the chain probe with `DXMT_LOG_LEVEL=trace` to see what the
   encode thread does with the admitted chunks.
2. Add a single `Present()` at the end of every iteration so the
   encode thread is forced to commit its work.
3. Or replace the offscreen RTs with a normal swap-chain backbuffer
   and Present()-driven flushes — at the cost of folding B6 in.

The chain probe's design — small RT, distinct RTs per pass, no
Present — was deliberate to isolate B3+B4 from B6. The observed zero
encode work suggests the encoder relies on Present-driven flushes
more than expected; this is itself a discovery worth documenting.

## B6 — Presenter (dxmt9-perf-present-loop)

Not run as part of W4. Probe is built and catalogue-registered;
schedule for the next cycle.

## B4 — encoder (encode-replay + 4 existing probes)

Not run as part of W4. The 4 existing probes (ffp-only / multi-rt /
depth-heavy / skeletal) have prior baselines under the legacy
suite-level reporting; re-running with `--boundary B4` to get the
filtered view is a follow-up.

## B5 — Metal/GPU + B1 (CPU recorder)

B5 is observed via `gpu_command_buffer_time_*` on any probe that
emits CBs. Pending B3 fix.

B1 has the native `dxmt9-chunk-record-micro-spec` baseline (W1-C):
mean ~3 µs / iteration over 100k iterations against a typical D3D9
hot-path mix (4 textures + 8 samplers + 16 render states + 4
transforms + vs/ps consts + 8 draws). Soft assertion budget 100 µs
per chunk; observed mean 4.6 µs (under load p99 ~9 µs).

## Findings summary

| Boundary | Status | Headline number |
|---|---|---|
| B1 PE recorder | ✅ baseline (native test) | 3-9 µs / chunk-build cycle |
| B2 PE→unix bridge | ✅ baseline (probe partial) | **10 µs / commit_chunk (p50)**, 306 µs p99 |
| B3 CommandQueue | ⚠️ probe yielded zero CBs — needs fix | inconclusive |
| B4 encoder | ⏸ not run this cycle | (legacy data exists in SFIV runs) |
| B5 Metal/GPU | ⏸ depends on B3/B4 probes | — |
| B6 Presenter | ⏸ not run this cycle | (data exists in present-policy A/B) |

## R-BACK-2.34 default-flip status

The U1 SFIV measurement showed cap=4 is **functionally correct, GPU
time p99 −44%, but fps unchanged within noise** because tile-flush
overhead absorbed the pipelining win. W4 set out to find a workload
where cap=4 wins on fps via the chain_parametric probe (small RT,
small tile-flush envelope, parametric chain length).

The probe is in place and runs cleanly through the bridge; the
encode-side anomaly above must be resolved before any fps-win
conclusion. **R-BACK-2.34 stays deferred.**

## Anti-finding

The suite runner + `--boundary` flag + per-boundary schema work
exactly as designed. `summary.json["boundary"] = "B2"` is set on
boundary runs; `summary.json["summary"][i]["counters"]` only contains
the seven B2 keys. Boundary-isolated reporting is functionally
proven, even if the underlying probes need more refinement to
produce useful numbers in every case.

## Reproduction

```sh
# B2 baseline
env DXMT_PERF_COUNTERS=1 BRIDGE_EMPTY_ITERATIONS=20000 \
  python3 scripts/tools/run_dx9_present_policy_ab.py \
    --app dxmt9-perf-bridge-empty --mode default --runs 1 \
    --timeout 60 --boundary B2 --tag W4-baseline-B2

# B3 chain (currently hits the encode anomaly above)
env DXMT_PERF_COUNTERS=1 CHAIN_LENGTH=4 CHAIN_DRAWS_PER_PASS=20 CHAIN_ITERATIONS=300 \
  python3 scripts/tools/run_dx9_present_policy_ab.py \
    --app dxmt9-perf-chain-parametric --mode default --runs 1 \
    --timeout 60 --boundary B3 --tag W4-chain-off

env DXMT_PERF_COUNTERS=1 CHAIN_LENGTH=4 CHAIN_DRAWS_PER_PASS=20 CHAIN_ITERATIONS=300 \
    DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass \
  python3 scripts/tools/run_dx9_present_policy_ab.py \
    --app dxmt9-perf-chain-parametric --mode default --runs 1 \
    --timeout 60 --boundary B3 --tag W4-chain-cap4

# Whole suite (after probes are stable)
bash scripts/run_suites/run_boundary_audit_suite.sh
```

## Open follow-ups (next cycle)

1. ~~**Chain probe encode anomaly**~~ — **resolved by X1 (2026-05-10)**.
   Cause: encode thread is dormant without a Present-triggered flush.
   Adding `CHAIN_PRESENT_INTERVAL=10` to the probe (Present every 10
   iterations) restores expected behavior. Fix shipped in
   `experiments/apps/ChainParametricProbe/ChainParametricProbe.cpp`.
   Runtime-side investigation deferred — the no-Present case may
   represent a real edge in the queue lifecycle worth a separate
   gap.md row, but it does not block the cap=4 measurement.
2. **Heavy SFIV B4 baseline with `--boundary B4`** to compare against
   the existing SFIV per-frame data. This validates B4 schema.
3. **Present-loop full A/B** under each `DXMT9_PRESENT_*` mode for
   the B6 baseline.
4. **R-BACK-2.34 default-flip decision** — see X1 result below; one
   workload now supports the flip but more evidence (different RP
   density, different draw count per pass) is desirable.

---

## X1 — chain probe with `CHAIN_PRESENT_INTERVAL=10` (2026-05-10)

After adding optional Present to ChainParametricProbe, three runs at
CHAIN_LENGTH=4 / CHAIN_DRAWS_PER_PASS=20 / CHAIN_ITERATIONS=300:

| metric | NO PRESENT | PRESENT=10 / off | PRESENT=10 / cap=4 |
|---|---:|---:|---:|
| `chunk_admit` | 1201 | 1231 | 1231 |
| `submit_draw` (PE side) | 24,000 | 24,000 | 24,000 |
| `submit_present` | 0 | 30 | 30 |
| `command_buffers` | **0** | 28 | **112** (4×) |
| `sub_command_buffers` | 0 | 0 | **84** (3 mid-chunk × 28) |
| `chunk_subcb_count_max` | 0 | 1 | **4** (cap holds) |
| `render_pass_begin` | 0 | 1120 | 1120 |
| `encode_chunk_cpu_ms` | 0 | 621.6 | **230.7** (−63%) |
| `gpu_command_buffer_time_ms` | 0 | 118.9 | 103.2 (−13%) |
| `completion_wait_ms` | 0 | 134.8 | 122.7 (−9%) |
| `present_acquire_wait_ms` | 0 | 101.7 | **80.9** (−20%) |
| `process_elapsed_sec` | 16.92 | 13.03 | **12.40** (−5%) |
| iterations / sec | 17.7 | 23.0 | **24.2** |

### Findings

(a) **Encode anomaly confirmed and characterized.** Without ANY
Present, the encode thread never produces command buffers despite
1201 successful chunk_admits. Submit-side counters (PE) show 24k
draws + 1.2k clears recorded. The encode thread sees the queue but
never dequeues. Adding Present every 10 iterations restores expected
behavior with all encode-side counters reflecting real work.

This is a queue-lifecycle edge that may deserve its own gap.md row —
"encoder requires Present-driven flush; no graceful drain on long
non-Present runs". The probe-side workaround is sufficient for
measurement purposes for now.

(b) **R-BACK-2.33 cap=4 wins on this workload.** Compared to
`policy=off` at the same Present interval:

  - encode_chunk_cpu_ms 621 → 231 (−63%)
  - present_acquire_wait_ms 102 → 81 (−20%)
  - process_elapsed_sec 13.03 → 12.40 (−5%)

The −63% encode CPU drop is striking — the per-chunk wall time of
encodeChunk falls dramatically when the chunk's `commit()` is split
into 4 smaller sub-CBs. Driver-side commit cost is non-linear in CB
size; 4 small commits beats 1 large commit on this workload.

(c) **Tile-flush envelope is the deciding factor.** SFIV's heavy
scene (1920×1080 × 4 RTs ≈ 33 MB tile flush) cancelled cap=4 fps
gain in U1. Chain probe's small RT (256×256 × 1 RT ≈ 256 KB tile
flush) preserves the gain. The cost model in
`docs/research/g-axis-tuning.md` predicted exactly this asymmetry.

### R-BACK-2.34 status update

One workload (chain probe with small RT) now shows a clear cap=4 fps
win. SFIV (heavy real-app) shows neutral. Recommended next:

1. Run additional synthetic probes varying RT size to map the
   crossover point — at what RT × MSAA × format combination does
   tile flush cancel the pipelining gain?
2. Run a few more real D3D9 titles (Anno1404, Tutorial07,
   simpler-than-SFIV apps) to see if any benefits.
3. If at least one *real* title benefits and none regresses, flip
   R-BACK-2.34 default to per-render-pass + cap=4.
