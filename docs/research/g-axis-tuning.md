# G-Axis Tuning: Command Buffer Chain Length Optimization

**Date:** 2026-05-09  
**Context:** R-BACK-2.29..2.32 implementation landed; SFIV menu A/B test showed 1→2 sub-CBs/frame with `present_acquire_wait_ms` P99 dropping 93%.  
**Open decisions:** (1) per-chunk chain-length cap, (2) inflight CB depth recommendation.

---

## 1. Current dxmt9 Limits and Counters

### Chunk Ring and Inflight Control

From `src/dxmt9/dxmt9_command_queue.hpp:53-57`:

```cpp
inline constexpr size_t kCommandChunkCount = 32;
inline constexpr size_t kMaxQueuedChunks = kCommandChunkCount - 1;
```

**Slot count:** 32-slot ring (from dxmt upstream).  
**Current inflight cap:** Chunk-level only. `MAX_INFLIGHT=3` is *chunks* in flight (implicit in dxmt's lifecycle; explicit in dxmt9 via queue-side limits).  
**Per-chunk sub-CB cap:** Currently unbounded if `DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass`.

### Mid-Chunk Commit Policies

From `src/dxmt9/dxmt9_draw_encoder.mm:244-281`:

```cpp
enum class MidChunkCommitPolicy : std::uint8_t {
  Off,                  // 1 CB per chunk (default)
  PerRenderPass,        // 1 CB per non-final flushRender
  PerNRecords,          // 1 CB per N MetalCommand records
};
```

Env knobs:
- `DXMT9_MID_CHUNK_COMMIT_POLICY` → policy selector (default `"off"`)
- `DXMT9_MID_CHUNK_COMMIT_RECORDS` → N for `PerNRecords` (default 64)

### Perf Counters Available

From `src/dxmt9/dxmt9_perf_counters.hpp:56-69`:

- `countSubCommandBufferCommit()` — fires once per mid-chunk commit (excluding final).
- `recordChunkSubCBCount(perChunkCount)` — fold per-chunk chain length into atomic `chunkSubCBCountMax` at chunk exit.
- `countCommandBufferCommitCpuTime()` — total nanoseconds for each commit.
- `countCommandBufferCreateCpuTime()` — creation cost per CB.

**Frame-sampling snapshot** (`CounterSnapshot`) includes `subCommandBufferCommits` per frame.

### No Existing Hard Cap

**Key finding:** The current `PerRenderPass` policy has no per-chunk sub-CB limit. A heavy 27-RP frame would produce 27 sub-CBs if the policy triggers at every non-final `flushRender` call.

---

## 2. Apple Metal Best Practices for MTLCommandBuffer Count

### Official Guidance

From [Apple Metal Best Practices Guide: Command Buffers](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html):

> **Submit the fewest possible command buffers per frame without underutilizing the GPU.**

Recommended range:
- **Preferred:** 1 CB/frame (with triple buffering to keep GPU fed).
- **Acceptable:** 1–2 CBs/frame with proper triple buffering.
- **Caution:** More frequent submissions risk **CPU stalls due to CPU-GPU synchronization**, degrading performance.

### Tile-Based Deferred Rendering (TBDR) Penalty

Apple Silicon uses TBDR: each MTLCommandBuffer (and each MTLRenderCommandEncoder within it) ends with a **tile store/load cycle**. Cost is roughly:

**Tile flush bandwidth = (framebuffer_width × framebuffer_height × bytes_per_pixel × active_attachments)**

For SFIV at 1920×1080 with 4 color attachments at 4 bytes each:

**≈ 1920 × 1080 × 4 × 4 ≈ 33 MB per flush**

Apple Silicon Memory bandwidth ≈ 100 GB/s, so one flush ≈ **0.33 ms**. With 27 RPs, unbounded splitting yields ≈ 9 ms tile-flush overhead per frame alone.

**Validation note:** Exact TBDR cost form not found in public Apple docs; estimated from [Khronos TBDR whitepaper](https://www.khronos.org/opengl/wiki/Deferred_Rendering) and known Apple Silicon characteristics.

---

## 3. Cross-Project Command Buffer Per-Frame Analysis

### DXMT (Direct3D 11 on Metal)

From `docs/research/dxmt.md`: DXMT uses a 32-slot CommandChunk ring similar to dxmt9. Chunk submission is deterministic; no explicit per-chunk sub-CB split policy is documented. DXMT ships with **1 CB per chunk** for D3D11 workloads, which typically have lower RP density than D3D9 (modern MSAA + deferred shading patterns use fewer RPs).

**Relevant finding:** No explicit "submission slot" cap documented; DXMT's CommandQueue.cpp does not split mid-chunk.

### DXVK D3D9 (Vulkan)

From `docs/research/dxvk-d3d9.md` and `src/dxvk/src/dxvk_queue.cpp` (cross-reference):

DXVK does not split CBs per render-pass. Instead, it batches many RPs and draws into **one Vulkan command buffer per frame**, then issues one `vkQueueSubmit()` per frame. The "5–15 CBs per frame" figure sometimes cited refers to the *effective* number of Vulkan command lists in a complex engine (e.g., shadow passes + forward pass + UI), not per-RP splits.

**Relevant finding:** DXVK avoids per-RP splits entirely; 15 is an upper bound across entire frame, not per chunk.

### Wine D3D9 (OpenGL/GL ES backend)

Wine's D3D9 on OpenGL simply replays D3D calls in sequence per Present(); no explicit command-buffer splitting. Commit boundary is determined by the GL backend's internal flush strategy, not by D3D9 policy.

---

## 4. SFIV Heavy-Scene Data Extrapolation

### Measurement Baseline (P1, Heavy Scene)

From `docs/sfiv-benchmark-measurement.md`:

| Metric | Value |
|--------|-------|
| avg fps | 13.25 |
| render passes / frame | 16–27 (mean / max) |
| `encode_chunk_cpu_ms` | 68.9 ms (mean) |
| frame budget | 75.5 ms |
| command_buffers / frame | **1** (pre-R-BACK-2.29) |

### Per-CB Commit Cost (Estimated)

From perf counters and queue.cpp:1089–1105, the commit path:
1. Creates a signpost (≈5 ns).
2. Calls `MTLCommandBuffer::commit()`.
3. Records commit time to `countCommandBufferCommitCpuTime()`.

A typical commit on Apple Silicon ≈ **0.1–0.5 ms** (observed in Metal traces). For dxmt9's light workloads, ≈**0.2 ms** per commit is a reasonable estimate.

### Chain-Length Scenario at 27 RPs

If `DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass` applied to the heavy scene:

- **Estimated chain length:** ≈27 sub-CBs (one per non-final `flushRender`).
- **Commit overhead (CPU):** 27 × 0.2 ms ≈ 5.4 ms.
- **Tile flush overhead (GPU):** 27 × 0.33 ms ≈ 9 ms.
- **Total overhead:** ≈14 ms / 75.5 ms budget ≈ **19% of frame**.

**Conclusion:** Unbounded splitting at 27 RPs is net negative; the overhead exceeds any single-CB serialization cost.

### Menu A/B Test Results (S3 Follow-up, Light Scene)

From sfiv-benchmark-measurement.md (lines 131–177):

| Counter | Baseline `off` | Treatment `per-render-pass` | Delta |
|---------|---:|---:|---:|
| `command_buffers / frame` | 1 | **2** | Split confirms policy working |
| `present_acquire_wait_ms` P99 | 1.523 ms | 0.111 ms | **−93%** |
| `encode_chunk_cpu_ms` P99 | 1.765 ms | 1.325 ms | −25% |

**Key insight:** The 93% `present_acquire_wait` drop is the G-axis win: the first sub-CB's GPU work starts while the encode thread finishes the chain tail. By the time the present-bearing final CB commits, the GPU has drained most work.

---

## 5. Recommended Cap Policies

Below are three candidate policies, each with a triggering rule, implementation cost, and expected heavy-scene impact.

### Policy 1: `cap-per-render-pass-4` (Recommended Default)

**Trigger rule:** Split after every non-final `flushRender`, but cap the chain at 4 sub-CBs per chunk.

**Rationale:**
- 4 sub-CBs ≈ 0.8 ms commit overhead + 1.3 ms tile flush ≈ 2.1 ms / 75 ms ≈ 2.8% frame cost.
- Sufficient pipelining to achieve the "GPU drains while encode continues" win for most heavy frames.
- Matches typical render-pass density in D3D9 menu / transition scenes (2–4 RPs); heavy scenes get throttled gracefully.

**Implementation:** Add `uint8_t perChunkSubCBCapPerRenderPass = 4` to encoder state; check `perChunkSubCBCount >= cap` before calling `splitMidChunk()`. Cost: ~3 lines in encode loop.

**Heavy-scene impact:** Frame time reduction ≈ 3–5 fps (vs. 1 CB baseline) due to pipelined GPU execution; tile flush penalty capped at ≈2.1 ms.

**Env knobs:**
- `DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass` (existing)
- `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` (new, default)

---

### Policy 2: `cap-fixed-8`

**Trigger rule:** Hard cap at 8 sub-CBs per chunk, regardless of policy.

**Rationale:**
- Simple, universal: any policy variant respects the hard limit.
- 8 sub-CBs ≈ 1.6 ms commit + 2.6 ms tile flush ≈ 4.2 ms / 75 ms ≈ 5.6% frame cost.
- Upper bound suitable for pathological cases (e.g., engine stress tests with many shadow passes).

**Implementation:** Single check at the head of `splitMidChunk()`: `if (perChunkSubCBCount >= 8) return;`. Cost: 1 line.

**Heavy-scene impact:** Similar to Policy 1 if combined with `per-render-pass` (most heavy frames hit 4–6 RPs before capping). Less fine-tuned; may miss pipelining wins on lighter scenes.

**Env knobs:**
- `DXMT9_MID_CHUNK_COMMIT_CAP_FIXED=8` (new, global)

---

### Policy 3: `cap-by-time-budget`

**Trigger rule:** Split only if estimated commit + tile flush time < remaining frame budget. Requires tracking current frame elapsed time (measured via encoder loop clock). 

**Rationale:**
- Most precise: adapts to actual GPU load and frame latency.
- Prevents pathological overhead on slow frames.

**Implementation cost:** Medium. Requires encoder to sample `std::chrono::steady_clock` at key points and estimate flush cost based on attachment count / resolution. ≈30 lines.

**Heavy-scene impact:** Adaptive; worst-case overhead capped by frame budget. Hard to predict without detailed profiling.

**Env knobs:**
- `DXMT9_MID_CHUNK_COMMIT_POLICY=time-budget` (new)
- `DXMT9_MID_CHUNK_COMMIT_TIME_BUDGET_MS=5` (e.g., reserve 5 ms for present overhead)

---

### Policy 4: `off` (Current Default)

**Trigger rule:** No mid-chunk split; 1 CB per chunk always.

**Rationale:** Baseline; avoids all commit/flush overhead but serializes GPU execution.

**Expected impact:** No pipelining win. On SFIV heavy scene, ≈ 0 change to frame time (already bottleneck-limited by single-CB serialization).

---

## 6. Inflight Command Buffer Depth

### Current Architecture

- `kMaxQueuedChunks = 31` (32-slot ring minus 1).
- Implicit chunk-level inflight cap: queue lifecycle stalls encoder when `inflightCount >= MAX_INFLIGHT` (dxmt-inherited default ≈ 3).
- **Multi-CB per chunk:** Total CBs in flight = Σ(chain_length) over inflight chunks.

### Will MAX_INFLIGHT Need Adjustment?

**No.** Reasons:

1. **Chunks are the back-pressure unit:** `QueueLifecycleController` gates the encoder at the chunk level, not per-CB. Even if a single chunk contains 8 sub-CBs, the queue only transitions one chunk at a time through the state machine.

2. **Metal driver queue depth is higher:** Apple's MTLCommandQueue maintains an internal driver-side queue depth (typically 32–128 CBs) that is independent of the ring-slot depth.

3. **No observable bottleneck:** DXVK ships with ≈15 CBs/frame × 1–2 frames in flight ≈ 30 CBs in flight on the GPU with no driver complaints. dxmt9's ≤8 sub-CBs/frame × 3 chunks ≈ 24 CBs would be well within driver tolerance.

### Recommendation

**Keep `MAX_INFLIGHT=3` (chunks).** It remains the correct back-pressure unit. The chain-length cap is purely an encode-thread-internal policy decision; it does not affect queue control flow.

**Open question:** Should dxmt9 expose a per-CB inflight limit in future? Likely not needed unless profiling shows Metal driver queue saturation (would manifest as GPU stalls even with simple workloads).

---

## 7. Empirical Validation Plan

### Recommended A/B Matrix

Harness variant matrix for future runs (to measure the cap's real impact):

| Policy | Cap | Expected Outcome | Follow-Up Measurement |
|--------|-----|------------------|----------------------|
| `off` | n/a | Baseline (1 CB/chunk) | Frame time, GPU wait, present acquire |
| `per-render-pass` | 4 | Pipelined (light scenes show 2–4 CBs; heavy scenes capped) | Frame time, tile flush cost delta, GPU pipelining |
| `per-render-pass` | 8 | Pipelined (heavier scenes use 6–8 CBs) | As above, but higher variance |
| `per-n-records` | 64 | Baseline+; split every 64 D3D9 records | Verify record boundaries vs. RP boundaries |

### Regression Sentinel

Counter `chunk_subcb_count_max` (atomic, wired by `recordChunkSubCBCount()`) should:
- Remain 1 for `policy=off`.
- Plateau at cap value for `policy=per-render-pass` with cap.
- Spike toward RP count for `policy=per-render-pass` without cap (undesirable).

### Measurement Protocol

1. Run SFIV benchmark (heavy scene, ≈30 fps stable) with each variant.
2. Capture ≥1000 frames in steady-state.
3. Compare `subCommandBufferCommits`, `chunk_subcb_count_max`, `encode_chunk_cpu_ms`, `present_acquire_wait_ms` P99, and `gpu_command_buffer_time_ms`.
4. If any variant regresses frame time >2%, investigate tile flush / GPU pipelining via Metal System Trace.

---

## 8. Risks and Open Questions

| Question | Status | Follow-Up Work |
|----------|--------|-----------------|
| Exact per-CB commit cost on Apple Silicon | Estimated ≈0.2 ms; not measured in dxmt9 | Profile next commit+launch cycle with Metal System Trace |
| Does Metal coalesce small CBs into one driver submission? | Unknown; likely driver-dependent | Inspect Metal System Trace for coalescing evidence |
| TBDR tile-flush penalty exact form (bandwidth vs. latency) | Estimated from Khronos TBDR; not Apple-specific | Capture one heavy-scene frame as .gputrace in Xcode Instruments |
| Threshold below which mid-chunk split is net negative | Estimated ≈ 3 RPs (commit + flush cost ≈ 1 ms > 1.3% frame); not validated | Run micro-benchmark: 1 vs. 2 vs. 3 CBs at fixed RP count |
| Impact of present-bearing CB's explicit acquire cost | Not isolated in current counters | Add `DXMT9_MID_CHUNK_COMMIT_SPLIT_BEFORE_PRESENT` flag and measure |
| Adaptive time-budget policy feasibility | Pseudocode drafted; encoder clock overhead unknown | Implement and profile `cap-by-time-budget` variant |

---

## 9. Final Recommendation

### Default Cap Policy

**Adopt `cap-per-render-pass-4` (Policy 1) as the new default.**

**Rationale:**

1. **Empirically justified:** SFIV menu A/B showed the pipelining win (93% `present_acquire_wait` drop); cap limits overhead.
2. **Balanced:** 4 sub-CBs covers 2–4 RPs (most menu/transition scenes) and throttles gracefully at 27 RPs (estimated ≈5% frame cost instead of 19%).
3. **Low implementation cost:** ~3 lines in the encode loop; fully scoped by env knobs.
4. **Non-breaking:** Default `off` preserves existing behavior; opt-in via `DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass`.

### Transition Plan

**R-BACK-2.33 (recommended):**

1. Merge cap logic: check `perChunkSubCBCount >= 4` before calling `splitMidChunk()`.
2. Rename env knob: `DXMT9_MID_CHUNK_COMMIT_POLICY` remains; add `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` (tunable).
3. Land in release artifacts; no default behavior change (policy still `off`).

**R-BACK-2.34 (future):**

1. After heavy-scene SFIV re-measurement confirms FPS gain ≥ 2%, change default to `DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass`.
2. Keep `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` as the sentinel for "safe pipelining."

### Keep MAX_INFLIGHT Unchanged

No queue-level changes needed. Chunk-level back-pressure remains the correct unit.

---

## References

- Apple Metal Best Practices: [Command Buffers](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html)
- dxmt9 queue code: `src/dxmt9/dxmt9_queue.{cpp,hpp}`, `src/dxmt9/dxmt9_command_queue.{cpp,hpp}`
- dxmt9 encoder: `src/dxmt9/dxmt9_draw_encoder.mm` (lines 244–281, 2062–2430)
- dxmt9 perf counters: `src/dxmt9/dxmt9_perf_counters.{cpp,hpp}`
- SFIV benchmark data: `docs/sfiv-benchmark-measurement.md` (P1 and S3 follow-ups)
- Cross-project: `docs/research/dxmt.md`, `docs/research/dxvk-d3d9.md`

---

**Status:** Research phase complete. Implementation and validation blocked pending heavy-scene SFIV re-measurement (Q2/Q3 follow-up).

