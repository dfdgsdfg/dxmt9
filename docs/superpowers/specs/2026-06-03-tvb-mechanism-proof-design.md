# 2026-06-03 — TVB Pressure Mechanism Proof Design

Status: design approved, awaiting user spec review before plan handoff.

## Context

`specs/perfomance.plan.md` tracks a long investigation of 3DMark05 GT1 frame60.
The dominant Xcode counter is `VS Buffer Device Memory Bytes Written` at
about `1.6 GiB` per frame, with named tiled-buffer counters under `30 MiB` and
all dxmt CPU writers under `1 MiB`. Earlier probes rejected dxmt argbuf/cbuf
writers, transient vertex/index upload, and source-visible `VSOut` width as
the first-order owner.

The cache-aware (LRU32) index reorder probe
(`DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE`) showed a coherent positive
chain on hot rows `50/1` and `50/3` in isolated row-local replays:

| Row | LRU32 misses | VS invocations | VS device write proxy | Named tiled total | GPU time |
|---|---:|---:|---:|---:|---:|
| `50/1` | `-16.87%` | `-10.32%` | `-13.36%` | `-16.13%` | `-8.76%` |
| `50/3` | `-16.63%` | `-10.16%` | `-6.84%` | `-10.99%` | `-3.25%` |

Both standalone replays still reported `0 MiB` for Xcode's
`VS Buffer Device Memory Bytes Written`. The original next-step plan tried
to close that gap by raising mini-replay fidelity (storage mode, texture
usage flags, persistent buffers). External research on Apple Silicon GPU
internals (WWDC20 #10632, Alyssa Rosenzweig's Asahi GPU posts, Imagination
PowerVR Parameter Buffer docs, MoltenVK source) shows that approach cannot
close the gap, because the counter measures firmware-owned Tiled Vertex
Buffer (TVB) / Parameter Buffer (PB) writes, not application MTLBuffer
writes. A standalone single-encoder replay below the firmware PB spill
threshold is expected to report `0 MiB`.

This design accepts that finding, promotes the existing row-local replay
signal to a TVB pressure mechanism proof, and keeps full-frame production
proof as a separate, still-open Path 1 problem.

## Goal

1. Reinterpret the existing row-local replay counters
   (`Tiled Vertex Buffer Bytes`, `Tiled Primitive Block`, `VS invocations`,
   GPU time) as direct evidence of TVB pressure reduction.
2. Document the firmware-owned TVB / PB model with public references so
   future contributors do not chase the `0 MiB` caveat again.
3. Split the acceptance gate into mechanism proof (row-local replay) and
   production proof (full-frame stability), so cached IB reorder work can
   close mechanism evidence now and continue production work separately.
4. Avoid any runtime / probe code change. Touched paths are limited to
   `scripts/tools/*` analysis helpers, `specs/perfomance.plan.md`,
   `agents/rules/metal_debugging.rules.md`, and `tests/scripts/*`. No
   `src/` change, no behavior change at GPU/encoder/recorder boundaries.

## Non-goals

- No change to mini-replay binary (`run_3dmark05_mini_replay.py` .mm
  template, env-var toggles, manifest format).
- No change to runtime probe code paths
  (`DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE`,
  `DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE`, `_MIN_GAIN_PCT`).
- No new Xcode captures in this design's scope; only re-finalize existing
  row50-1 and row50-3 full-payload artifacts with the new derived fields.
- Full-frame production proof (Path 1: cached reorder full-frame stability)
  remains open and is tracked separately.
- No attempt to make `VS Buffer Device Memory Bytes Written` nonzero in
  mini-replay (the original A+B+D toggle design is discarded in full).
- No reach for Apple counters that Xcode does not export
  (e.g. `Parameter Buffer used bytes`, partial-render count).

## External model (load-bearing references)

- Apple WWDC20 #10632 "Optimize Metal Performance for Apple silicon Macs"
  (vertex stage outputs land in a screen-aligned tiled vertex buffer in
  device memory).
- Alyssa Rosenzweig, "The Apple GPU and the Impossible Bug" — Parameter
  Buffer is firmware-managed, grows dynamically, partial-render flushes on
  overflow; two firmware counters: partial-render count and PB bytes used.
- Imagination "What is the Parameter Buffer?" — PowerVR/Apple lineage; PB
  lives in device RAM; size scales with transformed vertex count times
  per-vertex varying bytes.
- Asahi `drm/asahi` UAPI (Rosenzweig patches v2, v5) — TVB heap is a
  persistent firmware-/kernel-owned heap with 128 KB blocks of 32 KB
  pages; ASC firmware manages overflow.
- Mesa `src/asahi` — vertex outputs flow through the Unified Vertex Store
  staging unit; real backing is the TVB/PB in main memory; storage class is
  chosen by driver/firmware at submission, not by the shader.
- MoltenVK `MVKDevice.mm` `getMTLStorageModeFromVkMemoryPropertyFlags`,
  `MVKDeviceMemory.mm`, `MVKBuffer.mm` — on Apple Silicon only `Private`
  and `Shared` are used; no Apple-specific branch affects TVB.

These sources converge on one rule: `VS Buffer Device Memory Bytes Written`
aggregates firmware TVB/PB traffic, not application MTLBuffer traffic.
A standalone single-encoder replay below the PB spill threshold reports
zero through this counter even when the same draws under full-frame
pressure produce gigabyte-scale TVB writes.

## Architecture

```
[Existing row-local replay artifacts]
  traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/...
  traces/app-d3d9-3dmark05-cache-opt-row50-3-full-payload-r1/...

  Per-row paired Xcode counter CSVs already report:
    Tiled Vertex Buffer Bytes        decreases
    Tiled Primitive Block Bytes      decreases
    VS Invocations                   decreases
    Vertex stage time share          decreases
    GPU time                         decreases
    VS Buffer Device Memory Bytes Written  reports 0 (both variants)

                ▼
[finalize_3dmark05_perf_probe.sh — derived fields added]
  dxmt_tvb_pressure_proxy_mib       = vs_invocations × expected_vsout_bytes / 1048576
  dxmt_tvb_named_total_mib          = tiled_vertex_buffer_bytes + tiled_primitive_block_bytes
  dxmt_tvb_named_to_proxy_ratio     = named / proxy   (sanity vs Imagination model)
  dxmt_vs_buffer_write_to_tvb_proxy_ratio
                                    = vs_buffer_device_memory_bytes_written / proxy
                                      (partial-render multiplier estimate; ~0 means below spill)

                ▼
[compare_xcode_dxmt_bottlenecks.py — new gate option]
  --require-tvb-mechanism-proof
    PASS iff:
      tiled_vertex_buffer_bytes      decreases
      vs_invocations                 decreases
      top_gpu_ms                     decreases
    on the compared row(s).

  Existing --require-stable-frame-proof is unchanged. It still owns
  full-frame production acceptance (row-key match, top GPU / VS write /
  unexplained write decrease, draw / vertex / triangle drift <= 5%).

                ▼
[Spec + rule updates]
  specs/perfomance.plan.md  +  "TVB Pressure Mechanism Proof" section
  agents/rules/metal_debugging.rules.md  +  one paragraph on TVB semantics
```

No mini-replay binary, no probe, no runtime path changes.

## Components

### C1. Spec section in `specs/perfomance.plan.md`

Append a section "TVB Pressure Mechanism Proof" containing:

1. Statement of the firmware-owned TVB / PB model with the public references
   above and short quotes.
2. Reinterpretation of the `0 MiB` standalone-replay caveat as expected
   behavior when the firmware PB does not reach its spill threshold.
3. Tabulated `50/1` and `50/3` row-local replay counter movements from the
   existing artifacts, framed as direct mechanism evidence (named TVB
   counters, VS invocations, GPU time).
4. Explicit statement that mechanism proof is closed and that the remaining
   open work is Path 1 full-frame production proof (cached reorder drift
   gates).
5. Pointers to the design doc at this path and the new derived fields / new
   gate.

### C2. Derived fields in `scripts/tools/summarize_xcode_encoder_counters.py`

`finalize_3dmark05_perf_probe.sh` is a bash wrapper; the joined summary
CSV and bottleneck Markdown are produced by
`scripts/tools/summarize_xcode_encoder_counters.py`. That file already
emits per-row `vs_invocations`, `tiled_vertex_buffer_mib`,
`tiled_primitive_block_mib`, `dxmt_hidden_backend_write_mib`,
`dxmt_hidden_backend_write_ratio`, `expected_vsout_bytes_per_vertex`, and
`vs_buffer_to_expected_vsout`. Add these new per-row columns next to those:

- `dxmt_tvb_pressure_proxy_mib`
    = `vs_invocations × expected_vsout_bytes_per_vertex / 1048576`
    (Imagination / WWDC TVB-bytes model)
- `dxmt_tvb_named_to_proxy_ratio`
    = `(tiled_vertex_buffer_mib + tiled_primitive_block_mib) / dxmt_tvb_pressure_proxy_mib`
    (sanity: named Xcode counters versus theoretical lower bound)
- `dxmt_vs_buffer_write_to_tvb_proxy_ratio`
    = `vs_buffer_device_memory_bytes_written_mib / dxmt_tvb_pressure_proxy_mib`
    (partial-render multiplier estimate; near zero means below PB spill)

`dxmt_hidden_backend_write_mib` is retained as-is. The new
`dxmt_tvb_pressure_proxy_mib` is added beside it as a complementary
theoretical baseline.

Add to the bottleneck Markdown header (one short paragraph): cite WWDC20
session 10632, Rosenzweig's Asahi GPU post, and Imagination PB doc URLs.
State that `VS Buffer Device Memory Bytes Written` measures firmware
TVB / PB traffic, not application MTLBuffer writes, and that the named
tiled-buffer counters plus VS invocations are direct TVB pressure proxies.

Missing inputs (counter not exported by Xcode, dxmt log missing
`expected_vsout_bytes_per_vertex`) produce a blank cell. Never fabricate.
No new Xcode capture is introduced by this work item.

### C3. New optional gate in `scripts/tools/compare_xcode_dxmt_bottlenecks.py`

Add `--require-tvb-mechanism-proof`. Implementation behavior:

- Independent of `--require-stable-frame-proof`. Either, both, or neither
  can be set; FAIL conditions union.
- Operates on the compared row(s) chosen by the existing `--top` selection.
- PASS iff, on **every** compared row, all three are strictly less in
  the after-CSV than the before-CSV:
    - `tiled_vertex_buffer_mib` strictly decreased
    - `vs_invocations` strictly decreased
    - `top_gpu_ms` strictly decreased
- Equality does not pass.
- FAIL message names each individual failing predicate per row (one line
  per row × predicate).
- Treats missing counter inputs as FAIL with explicit "counter not present"
  reason, never silent PASS. The reason line states which counter and which
  CSV (before or after) was missing.
- Also forwarded by `finalize_3dmark05_perf_probe.sh` if its existing
  `--require-stable-frame-proof` plumbing is reused (add a parallel
  `--require-tvb-mechanism-proof` pass-through in the bash wrapper so the
  full workflow can request mechanism + production gates together).

### C4. Rule update in `agents/rules/metal_debugging.rules.md`

Append a short paragraph stating:

- `VS Buffer Device Memory Bytes Written` reports firmware-owned TVB / PB
  writes, not application MTLBuffer writes.
- `0 MiB` in a row-local mini-replay is expected when the PB does not
  cross its spill threshold.
- Bottleneck report now contains TVB pressure proxy fields and a new
  `--require-tvb-mechanism-proof` gate.

### C5. Unit tests

Add to `tests/scripts/test_compare_xcode_dxmt_bottlenecks.py`:

- A fixture-based row matching `50/1` shape that passes
  `--require-tvb-mechanism-proof`.
- A fixture that flips one of the three predicates (e.g. `vs_invocations`
  rises) and confirms FAIL with the right reason.
- A fixture missing `tiled_vertex_buffer_bytes` and confirms FAIL.
- Regression coverage for `--require-stable-frame-proof` PASS / FAIL cases
  is unchanged.

Add to the finalizer's existing python helper tests (or create
`tests/scripts/test_finalize_3dmark05_perf_probe.py` if there is none yet):

- Derived field arithmetic on a small fixture.
- Blank input handling for missing counters.

## Data flow

1. Operator re-summarizes the already-captured row-local artifacts by
   running `summarize_xcode_encoder_counters.py` directly on the existing
   Xcode CSVs (the finalizer bash wrapper assumes a fresh run; the python
   summarizer can be re-invoked stand-alone). Concrete invocation per row,
   to be wrapped in a thin helper if not already exposed:

   ```
   python3 scripts/tools/summarize_xcode_encoder_counters.py \
     --xcode-counters traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/analysis/mini-replay-full-r3-cache-opt-lru32-counters-xcode.csv \
     --joined-output  traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/analysis/mini-replay-full-r3-cache-opt-lru32-joined-summary.csv
   ```

   (Adjust flag names to match the script's actual CLI; the design
   commitment is that derived TVB-proxy columns appear in the joined
   summary CSV without re-running any GPU capture.) Repeat for the
   original-ordering CSV in the same directory, and for both 50/1 and
   50/3 artifacts.

2. Operator runs the new gate against the original-vs-cache-opt artifacts:

   ```
   python3 scripts/tools/compare_xcode_dxmt_bottlenecks.py \
     traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/analysis/...-original-counters-xcode.csv \
     traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/analysis/...-cache-opt-lru32-counters-xcode.csv \
     --before-label original \
     --after-label cache-opt-lru32 \
     --require-tvb-mechanism-proof \
     --output traces/app-d3d9-3dmark05-cache-opt-row50-1-full-payload-r1/analysis/frame50-tvb-mechanism-proof.md
   ```

   Same for `50/3`.

3. Operator appends the "TVB Pressure Mechanism Proof" section to
   `specs/perfomance.plan.md` and the short paragraph to
   `agents/rules/metal_debugging.rules.md`, citing the generated proof
   Markdown files as evidence.

4. Path 1 (full-frame production proof, cached reorder drift gates) is
   tracked separately in `specs/perfomance.plan.md` and is not part of
   this design.

## Failure modes and what each means

| Observed | Interpretation | Action |
|---|---|---|
| New gate PASS on both `50/1` and `50/3` | Mechanism proof closes. | Append spec section, hand off to Path 1. |
| New gate FAIL on one row only | Single-row anomaly; check whether the existing artifact actually compares matching ordering variants. | Re-finalize, inspect joined summary for missing counters; do not weaken the gate. |
| New gate FAIL on both rows | External model misapplied or finalizer arithmetic bug. | Stop. Walk the derived field calculation and external references; do not change the runtime probe code. |
| Derived field produces NaN or absurd proxy | Likely missing `expected_vsout_bytes` in dxmt log. | Surface as blank cell with explicit "counter not present" note in the report; never substitute a default. |

## Verification gates (design self-acceptance)

1. Existing `--require-stable-frame-proof` callers produce bit-identical
   results before and after the change. Unit tests cover this.
2. New derived fields are computed only from inputs present in the joined
   summary CSV. Missing inputs produce blank cells.
3. New gate fails when any one of its three predicates fails.
4. External citations are verbatim against the listed URLs and are checked
   into the spec section.
5. Re-running the new gate over the already-archived row50-1 and row50-3
   artifacts passes without any new GPU capture.

## Testing

- `pytest tests/scripts/test_compare_xcode_dxmt_bottlenecks.py`
- Finalizer helper test (existing or new file) for derived fields.
- `git diff --check` on the updated spec section and rule paragraph.
- Manual run of the operator command sequence in `Data flow` above,
  producing two `frame50-tvb-mechanism-proof.md` files under
  `traces/.../analysis/`.

## Sources

- WWDC20 #10603 — Optimize Metal apps and games with GPU counters
  — https://developer.apple.com/videos/play/wwdc2020/10603/
- WWDC20 #10632 — Optimize Metal Performance for Apple silicon Macs
  — https://developer.apple.com/videos/play/wwdc2020/10632/
- Apple — Measuring the GPU's use of memory bandwidth
  — https://developer.apple.com/documentation/xcode/measuring-the-gpus-use-of-memory-bandwidth
- Apple — GPU counters and counter sample buffers
  — https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers
- Imagination — What is the Parameter Buffer?
  — https://docs.imgtec.com/Profiling_and_Optimisations/PerfRec/topics/c_PerfRec_parameter_buffer.html
- Alyssa Rosenzweig — Asahi GPU part 5 (The Impossible Bug)
  — https://alyssarosenzweig.ca/blog/asahi-gpu-part-5.html
- Alyssa Rosenzweig — Asahi GPU part 2, 3
  — https://alyssarosenzweig.ca/blog/asahi-gpu-part-2.html
  — https://alyssarosenzweig.ca/blog/asahi-gpu-part-3.html
- Asahi Linux docs — AGX SoC overview
  — https://asahilinux.org/docs/hw/soc/agx/
- Mesa Asahi driver documentation
  — https://docs.mesa3d.org/drivers/asahi.html
- MoltenVK source (Apple Silicon storage-mode policy)
  — https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm
  — https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDeviceMemory.mm
  — https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKBuffer.mm
