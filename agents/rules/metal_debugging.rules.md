---
description: Xcode Metal Debugger capture, validation, resource and replay-counter evidence constraints
paths:
  - "agents/skills/gputrace/**/*"
  - "src/dxmt9/*capture*"
  - "scripts/tools/*xcode*"
  - "scripts/tools/*metal_capture*"
globs: "agents/skills/gputrace/**/*"
alwaysApply: false
---

# Xcode Metal Debugger evidence

Use [gputrace](../skills/gputrace/SKILL.md) for execution. This rule owns
`.gputrace`, capture-layer correctness, resource inspection and Xcode replay.
[Instruments](instruments.rules.md), [xctrace](xctrace.rules.md), and
[Perfetto](perfetto.rules.md) own their respective measurement/tool contracts.

## Capture and workload identity

- Xcode or an in-process MTLCaptureManager creates `.gputrace`. Enabling a
  capture layer alone does not request a capture. A frame not reached cannot
  prove capture-layer success or failure; retain start/stop and destination logs.
- Bind a capture to its workload, runtime/build, frame/sequence and device.
  Keep manual artifacts in ignored `traces/<app-runid>/`, including exported
  counters, stderr and analysis. Use absolute capture paths for Wine catalogue
  launches, which change working directory. Preserve harness-owned output paths.
- File capture requires an active layer in the real Wine child. An Xcode welcome
  window is not proof of attachment. `developerTools` capture expects exported
  Xcode artifacts rather than a directly written file. Check attach availability
  and Developer Mode before spending a long run.
- Do not enable `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` by default on
  3DMark05: that path has produced black startup with zero draws/presents.
  Use the supervised wrapper and its capture-layer preflight. A deliberate
  layer diagnostic is not an ordinary FPS sample.
- The Wine capture-layer wrapper must patch and restore using same-directory
  temporary files and atomic `mv`; in-place `cp` reproduced code-signature kills.
  Its Wine root must match the child runner's root. See [capture procedure](../skills/gputrace/references/frame-capture.md).
- Use positive workload supervision for 3DMark. Timeout-finalized artifacts may
  be valid when their own checks pass; manual-kill lifetime is not throughput.
  See [probe supervision](../skills/gputrace/references/gt1-supervision.md).

## Resource and correctness interpretation

- Preserve existing resource/encoder labels and sequence identities. Per-draw
  `Draw[...]` groups are gated because they add hot-path overhead. For an
  externally triggered Xcode capture, explicitly enable
  `DXMT9_PER_DRAW_DEBUG_GROUPS=1` when draw narrative is needed; do not make it
  a production default or relabel every object merely to satisfy a parser.
- Raw resource availability varies by capture and Xcode format. The local
  parser uses undocumented-layout/label heuristics: missing `MTLBuffer-*` or
  `MTLTexture-*` files does not prove resources were unused. Metadata/string
  inventories do not establish draw bindings, pixel history or resource bytes.
- Validation, shader instrumentation, debug groups, screenshots/readbacks and
  GPU replay perturb execution. Keep diagnostics separate from clean timing.
  Command-buffer faults remain failures even when the screenshot looks correct.
- GT1's recorded visual alignment anchor is `v0.0.3`; older `v0.0.1` images
  are historical triage. Exact pixel claims require the same frame/input or a
  draw/window proof, not equal capture delay. Follow
  [rendering correctness rules](rendering_correctness.rules.md).

## Exported counters and claim boundaries

- Export the replayed capture with performance data and encoder counters into
  the current run's analysis directory. Wait for actual profiling completion;
  visible encoder rows alone are insufficient. Verify the destination on disk:
  Xcode save panels can retain an older run's `analysis` folder.
- Use exported CSVs and validated same-run encoder joins for numeric claims.
  Preserve source labels/units and missing values. An Xcode counter's name does
  not identify the dxmt CPU writer responsible for the traffic; use the
  [counter semantics](../skills/gputrace/references/gpu-counters.md) and
  [attribution gates](../skills/gputrace/references/xcode-counters.md).
- Small GPU replay deltas require repeated independent replays. Re-exporting
  an open bundle may reuse cached values; close/reopen before collecting the
  next replay. A delta inside measured variance is inconclusive.
- GPU command-buffer timestamps are distinct from CPU completion callbacks,
  stage samples and display presentation. Summed overlapping CB/encoder times
  are not frame wall time. Integrated stage timing is documented by the current
  [tracing evidence matrix](../../specs/experiments/harness/tracing/evidence.md).
- Keep original capture and counter exports after reporting. Do not delete raw
  evidence automatically or interpret capture-mode failure as a rendering
  regression. Durable findings belong in the owning `docs/perfomance/` domain.
