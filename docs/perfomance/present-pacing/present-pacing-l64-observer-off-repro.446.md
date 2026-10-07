---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "L64 corruption reproduced without observers and with early foreground"
type: experiment
status: correctness-blocker-reproduced-root-cause-open
updated: 2026-10-06
source: traces/stkcop-l64-h446/run.py; traces/stkcop-l64-h446/{a,b}/{invocation.json,completion.json,loading-ui.json,loading.png,scene-ui.json,scene.png}; experiments/output/app-d3d9-stkcop-bench-l64-h446-{a,b}/{result.json,daybenchmark.result}
related: docs/perfomance/present-pacing/present-pacing-l64-wild-diagnostic.445.md; docs/perfomance/present-pacing/present-pacing-v4-final-grouping.443.md
---

# Observer-OFF L64 reproduction

Run order B/A (64/0), same staged images and requested/final/control config
bytes, Day/high 1280x720, two engine threads, 16 sound targets. Perf counters,
frame sampling, query observers and supply trace are absent/disabled. Each
game window was restored during a visually verified loading screen before
the captured scene, and later observed without another restore. UI receipts
record PID, window, focus and screenshots. Continuous foreground throughout
the entire run is not independently monitored.

L64 again loses extensive buildings/ground and exposes the background. The
baseline scene shows populated buildings and ground. Both runs complete
successfully with no timeout. The captured cameras differ, so this is not
pixel-exact comparison, but the gross corruption recurs independently of
H445's intrusive diagnostics and mid-scene restore. Normal completion does
not qualify rendering correctness. Reported engine averages (32.745453 L64,
30.972383 baseline) are rejected as equivalent-rendering performance evidence.

## Why the fixed-input GPU gate did not close this domain

Code inspection identifies an important qualification difference, not yet
the demonstrated cause of corruption:

- `src/d3d9/device_c_render_tape_provider.cpp` enables
  `setRenderTapeExactAttachmentPreservation(true)` during provider replay.
- `src/dxmt9/dxmt9_command_queue.hpp` defines the profile as preserving
  first-use contents and live-out stores across serialization boundaries.
- `src/dxmt9/dxmt9_draw_encoder.hpp` / `resolveColorAttachmentLoadAction`
  changes first-use load from DontCare to Load in exact mode (explicit Clear
  and post-Present discard still take precedence).
- `src/dxmt9/dxmt9_render_pass_encoder.mm` / `beginRenderPass` combines the
  exact profile with query preservation to suppress immediate DontCare-store
  proofs. Deferred/late store decisions need separate inspection; the exact
  flag is not a claim that every store path has already been audited.

H443 qualifies its authenticated exact action profile. It must not be read as
qualification of ordinary action selection under arbitrary production splits.
The Metal skill preflight passed (Xcode/xctrace 27, compiler available, M1
Metal support). No expensive GPU trace was needed for this reproduction;
screenshots and code establish the next narrow question, not a GPU root cause.

## Next distinguishing tests

1. Hold input, source grouping and resource identity fixed; compare normal
   versus exact attachment actions without enabling capture/readback side
   effects. Do not infer a fix merely by enabling the whole capture mode.
2. Reduce a split between a writer and its later attachment consumer into a
   native/refinement and GPU-readback case, including unknown future sources,
   aliases, color/depth/stencil, and explicit Clear/Present precedence.
3. Independently inspect compatibility-segment lifetime stamping: the
   zero-progress split branch in `submitDrawRunBatch` raises
   `forceDrawResourceMarkingAfterSplit_` only for payload pressure, whereas
   `maybeCommitDrawChunkUnlocked` raises it after successful DrawLimit commit.
   Reachability and actual insufficient marks remain unproved; this is a
   separate code-level suspect, not an explanation of the screenshots.

No runtime change, default promotion, commit or cleanup. All evidence remains
available. Full-sequence baseline qualification and first divergent draw
localization remain open.
