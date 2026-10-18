# GPU counters and replay variance

### Per-command-buffer GPU wall time (M4)

When `DXMT_PERF_COUNTERS=1`, dxmt9 samples
`MTLCommandBuffer.GPUStartTime` / `GPUEndTime` after each command
buffer reaches Completed (`dxmt9_queue.cpp:~1450`). Driver-returned
0 / non-monotonic values are filtered. Surfaced as:

```
gpu_command_buffer_time_ms          # sum
gpu_command_buffer_time_max_ms      # max single CB
gpu_command_buffer_time_samples     # number of CBs sampled
gpu_command_buffer_time_p50_ms      # P50 over a 256-sample ring
gpu_command_buffer_time_p95_ms
gpu_command_buffer_time_p99_ms
```

### GPU fault counter (M5)

```
gpu_command_buffer_errors
```

Single counter; pair with `expected_counters{ max = 0 }` on probes.

### TVB and `VS Buffer Device Memory Bytes Written` semantics

Xcode counter `VS Buffer Device Memory Bytes Written` measures
firmware-owned **Tiled Vertex Buffer / Parameter Buffer (TVB/PB)** traffic
on Apple Silicon, not application `MTLBuffer` writes. The TVB/PB lives in
device RAM, is sized by the firmware at submission, and grows dynamically
during the vertex-binning phase. When it overflows, the firmware emits a
partial render that contributes additional store/reload bytes to the
same counter.

Practical consequences:

- A row-local mini-replay that submits a single encoder with a small
  vertex payload may report `0 MiB` for this counter because the PB
  never reached its spill threshold. This is expected, not a fidelity
  bug. The `summarize_xcode_encoder_counters.py` joined CSV now exposes
  derived `dxmt_tvb_pressure_proxy_mib`, `dxmt_tvb_named_to_proxy_ratio`,
  and `dxmt_vs_buffer_write_to_tvb_proxy_ratio` fields per row so this
  case is distinguishable from a missing capture.
- Mini-replay mechanism proofs that need to certify a TVB pressure
  reduction should use the new
  `compare_xcode_dxmt_bottlenecks.py --require-tvb-mechanism-proof` gate.
  It checks that top-N hidden backend write, `VS Buffer Device Memory Bytes
  Written`, `vs_invocations`, and `gpu_ms` all strictly decrease. The named
  tiled counters remain diagnostic subtype evidence, but they are too narrow
  to be the primary pass/fail gate when Xcode's VS write is dominated by
  hidden-expanded TVB/parameter storage.
- Full-frame production proofs continue to use
  `--require-stable-frame-proof`, which gates on
  `VS Buffer Device Memory Bytes Written` because at full-frame scale the
  PB spills and the counter is nonzero.

References:

- WWDC20 #10632 — `https://developer.apple.com/videos/play/wwdc2020/10632/`
- Imagination Parameter Buffer doc —
  `https://docs.imgtec.com/Profiling_and_Optimisations/PerfRec/topics/c_PerfRec_parameter_buffer.html`
- Alyssa Rosenzweig — Asahi GPU part 5 —
  `https://alyssarosenzweig.ca/blog/asahi-gpu-part-5.html`
- Design doc — `docs/superpowers/specs/2026-06-03-tvb-mechanism-proof-design.md`

### Xcode replay variance — when a delta is below the noise floor

`scripts/tools/analyze_xcode_replay_variance.py` quantifies how much
Xcode's GPU replay perturbs per-encoder counter values between
exports of the same `.gputrace` bundle. Run it before treating a small
or borderline delta as a real signal.

Workflow:

1. Open one `.gputrace` bundle in Xcode.
2. Show Performance > Counters, wait for draw-counter profiling to
   complete, then **Export Encoder Counters** to
   `traces/<run>/analysis/replay-variance/run1-counters-xcode.csv`.
3. **Close the bundle** in Xcode and reopen it; this forces the next
   counter export to re-run the GPU. Without closing the bundle,
   subsequent exports may reuse the cached counter values from the
   first replay.
4. Repeat steps 2-3 until you have `run1..runN` CSVs. Use at least three
   independent replays for an initial variability check and prefer five or
   more for comparisons; repeat count alone does not establish significance.
5. Run:

   ```sh
   scripts/run_python.sh scripts/tools/analyze_xcode_replay_variance.py \
     traces/<run>/analysis/replay-variance/run*-counters-xcode.csv \
     --output traces/<run>/analysis/replay-variance-report.md \
     --summary-output traces/<run>/analysis/replay-variance-summary.csv \
     --max-cv-pct 5
   ```

   The gate exits nonzero when any (encoder, metric) coefficient of
   variation exceeds the limit. Default metrics are `gpu_ms`,
   `vs_buffer_write_mib`, `vs_invocations`, `tiled_vertex_buffer_mib`,
   `tiled_primitive_block_mib`, `buffer_write_mib`. Override with
   repeated `--metric NAME`.

Use this tool whenever an A/B comparison reports a sub-10% delta or
when only some encoders move and others appear noisy. The report's
"Summary (top variance)" table sorts encoders by max CV across the
reported metrics so the noisiest rows are immediate.

Design: `docs/superpowers/specs/2026-06-03-xcode-replay-variance-design.md`.

## Integrated stage timing

The Perfetto GPU collector now provides explicit command-buffer and encoder-stage
timing; see [GPU Perfetto capture](perfetto.md) and the
[qualification matrix](../../../../specs/experiments/harness/tracing/evidence.md).
The legacy `DXMT9_PERF_ENCODER_GPU_TIME` diagnostic is separate from the frozen
Perfetto session configuration. Do not substitute that knob for a `gpu-detail`
request or infer integrated trace qualification from legacy counters.

## Policy comparison and diagnostics

For a GPU-time comparison across present policies, use the existing A/B harness:

```sh
scripts/run_python.sh scripts/tools/run_dx9_present_policy_ab.py \
  --mode default --mode async --runs 5 --cv-tolerance 0.02
```

Compare the generated Backend metrics, P50/P95/P99 and CV gate with the matched
workload/output checks. The startup `dxmt9-device capabilities` log reports
Metal families and `counter_sampling={stage=...,draw=...,blit=...}`; inspect the
actual device's support before selecting a timing level.

Existing audits remain in `scripts/check/audit_perf_counter_table.py` and
`scripts/check/audit_perf_counter_callsites.py`. Catalogue expected-counter
ranges and policy-comparison CV gates complement these structural checks;
they do not replace capture or clock qualification.
