---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 147
title: EncodeSession Pass-Streaming Runtime Retest
date: 2026-06-21
type: no-gputrace
status: mechanism-accepted-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-tailbatch-failopen-scout-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-tailbatch-failopen-scout-r1-20260620/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait2ms-scout-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait2ms-scout-r1-20260620/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait8ms-scout-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait8ms-scout-r1-20260620/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait16ms-scout-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait16ms-scout-r1-20260620/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait16ms-invalidcall-capture-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-wait16ms-invalidcall-capture-r1-20260620/3dmark05-direct.log, experiments/output/app-d3d9-3dmark05-encode-session-invalidcall-draw-return-capture-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-invalidcall-draw-return-capture-r1-20260620/3DMark05_dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-invalidcall-after-failopen-fix-r1-20260620/result.json, experiments/output/app-d3d9-3dmark05-encode-session-invalidcall-after-failopen-fix-r1-20260620/3dmark05-direct.log, experiments/output/app-d3d9-3dmark05-encode-session-invalidcall-after-failopen-fix-r1-20260620/actual.png, experiments/output/app-d3d9-3dmark05-encode-session-session-head-prefix-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-session-head-prefix-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-session-head-prefix-r1-20260621/actual.png, experiments/output/app-d3d9-3dmark05-encode-session-semantic-split-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-split-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-split-r1-20260621/actual.png, experiments/output/app-d3d9-3dmark05-encode-session-session-cap-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-session-cap-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-session-cap-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-open-cb-bounded-tail-wait.146.md, docs/perfomance/present-pacing/present-pacing-open-cb-finalizer-limit128.145.md, docs/perfomance/present-pacing/present-pacing-render-pass-carry-contract.128.md
---

# Present-Pacing H147 - EncodeSession Pass-Streaming Runtime Retest

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

After adding session-owner retention through queue completion, ordered
`EncodeSession` source metadata, live-slot source views, and fail-open prefix
submit, does open-render-encoder pass streaming become a visual-safe P4/FPS
candidate?

## Runs

Tail-ready pass-streaming scout:

```text
DXMT9_ENCODE_TAIL_PRESENT_BATCH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-tailbatch-failopen-scout-r1-20260620 \
  --frame 50 --no-gputrace --timeout 45 \
  --keep-frontmost --keep-frontmost-process 3DMark05 \
  --frame-sampling --no-encoder-breakdown \
  --stage-pre-present-command-limit 128
```

Bounded wait open-CB scout:

```text
DXMT9_ENCODE_TAIL_PRESENT_BATCH=1 \
DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1 \
DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1 \
DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US=2000 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-open-cb-wait2ms-scout-r1-20260620 \
  --frame 50 --no-gputrace --timeout 45 \
  --keep-frontmost --keep-frontmost-process 3DMark05 \
  --frame-sampling --no-encoder-breakdown \
  --stage-pre-present-command-limit 128
```

Bounded wait sweep follow-ups used the same command shape with
`DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US=8000` and `16000`. The 16ms setting was
also rerun once with `DXMT9_PE_RECORDER_STATS=1 DXMT_LOG_LEVEL=info` as an
invalid-call capture, not as a performance sample.

Comparison baseline: `app-d3d9-3dmark05-h220-current-visual-p4-baseline-r1`.

## Verdict

Mechanism accepted, runtime promotion rejected.

The corrected tail-ready session path is visual-safe and repairs the old
per-source command-buffer fragmentation:

| Metric | Baseline | Tail-ready session |
|---|---:|---:|
| `status` | `pass` | `pass` |
| `gpu_command_buffer_errors` | `0` | `0` |
| `completion_dequeue_status_error` | `0` | `0` |
| `command_buffers_per_present` | `4.010` | `1.011` |
| `render_passes_per_present` | `11.766` | `11.359` |
| `tile_preservation_mib_per_present` | `120.222` | `121.035` |
| `same_key_reentry_per_present` | `2.270` | `2.130` |
| `completion_wait_with_enqueue_ms_per_present` | `0.128` | `0.064` |
| `completion_wait_without_enqueue_ms_per_present` | `28.504` | `33.833` |
| sampled frame average | `18.265fps` | `17.775fps` |

No `invalidcall`, `invalid call`, or `D3DERR` string appears in the run
artifacts. The output image is not black (`mean_luma=68.283`,
`variance=5131.750`).

The bounded-wait open-CB path now also avoids the H146 black-screen failure and
proves the fail-open machinery runs:

| Metric | Bounded wait 2ms |
|---|---:|
| `status` | `pass` |
| `gpu_command_buffer_errors` | `0` |
| `completion_dequeue_status_error` | `0` |
| `open_cb_tail_present_pending_started` | `3,324` |
| `open_cb_tail_present_pending_tail_wait_timeout` | `2,364` |
| `open_cb_tail_present_pending_timeout_submitted` | `2,364` |
| `open_cb_tail_present_tail_submitted` | `959` |
| `open_cb_tail_present_pending_merge_failed` | `0` |
| `command_buffers_per_present` | `3.692` |
| `render_passes_per_present` | `13.052` |
| `tile_preservation_mib_per_present` | `166.666` |
| `completion_wait_with_enqueue_ms_per_present` | `16.905` |
| `completion_wait_without_enqueue_ms_per_present` | `18.758` |
| sampled frame average | `17.821fps` |

This is not promotable. It creates enqueue-during-wait work, but mostly by
timeout-submitting encoded prefixes before their Present tail arrives. That
reintroduces pass/tile fragmentation and keeps total wait worse than the
current baseline.

Longer bounded waits reduce timeout fragmentation:

| Metric | 8ms wait | 16ms wait |
|---|---:|---:|
| `status` | `pass` | `pass` |
| `gpu_command_buffer_errors` | `0` | `0` |
| `completion_dequeue_status_error` | `0` | `0` |
| `open_cb_tail_present_pending_started` | `1,873` | `1,677` |
| `open_cb_tail_present_pending_timeout_submitted` | `163` | `3` |
| `open_cb_tail_present_tail_submitted` | `1,709` | `1,673` |
| `open_cb_tail_present_pending_merge_failed` | `0` | `0` |
| `command_buffers_per_present` | `1.145` | `1.012` |
| `render_passes_per_present` | `11.671` | `11.568` |
| `tile_preservation_mib_per_present` | `128.599` | `124.774` |
| `completion_wait_with_enqueue_ms_per_present` | `1.862` | `0.000` |
| `completion_wait_without_enqueue_ms_per_present` | `33.015` | `34.477` |
| sampled frame average | `18.425fps` | `18.315fps` |

The 16ms shape is the best correctness/locality scout so far: it almost
eliminates timeout-submitted prefixes and recovers command-buffer shape. It is
still not a promotion result. The run is a short timeout scout, not the 120s
foreground gate; tile preservation remains above the h220 baseline
(`124.774MiB/present` vs `120.222MiB/present`); and no-enqueue completion wait
is still worse (`34.477ms/present` vs `28.504ms/present`).

The invalid-call capture did not reproduce the reported D3D9 error. With
`DXMT9_PE_RECORDER_STATS=1 DXMT_LOG_LEVEL=info`, the 16ms capture also passes:
`chunk_reject=0`, `gpu_command_buffer_errors=0`,
`completion_dequeue_status_error=0`, and no `0x8876086c`, `invalidcall`,
`invalid call`, or `D3DERR` failure string appears in the artifacts. The
captured `pe_present_timing` / `pe_present_call_return` rows all show
`hr=0x00000000` for the sampled Present and early post-Present D3D9 calls.
After extending the PE stats gate to log draw-call return HRESULT failures
beyond the first eight post-Present calls, the focused
`encode-session-invalidcall-draw-return-capture-r1-20260620` scout also passes.
It records `gpu_command_buffer_errors=0`,
`completion_dequeue_status_error=0`, `open_cb_tail_present_pending_merge_failed=0`,
`open_cb_tail_present_pending_timeout_submitted=81`, and no `0x8876086c`,
`invalid call`, or `D3DERR` string in `3DMark05_dxmt9.log`. This does not prove
the manual report was impossible; it only closes this GT1 reproduction attempt.

After tightening fail-open so encoded pending heads cannot be completed inline,
`encode-session-invalidcall-after-failopen-fix-r1-20260620` also passes with
the same 16ms scout knobs. The capture records `gpu_command_buffer_errors=0`,
`completion_dequeue_status_error=0`, `open_cb_tail_present_pending_merge_failed=0`,
`open_cb_tail_present_pending_timeout_submitted=141`,
`open_cb_tail_present_tail_submitted=1128`, non-black image metrics
(`mean_luma=73.368`, `variance=5434.152`), and no `0x8876086c`,
`invalid call`, `invalidcall`, or `D3DERR` string in the direct log.

The follow-up final-Present-tail + queue-state event-wait run removes the
wallclock release point from the successful carrier shape:
`encode-session-final-present-tail-eventwait-r1-20260621` runs without
`DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US`, passes visual smoke, and records
`open_cb_tail_present_pending_started=1217`,
`open_cb_tail_present_tail_appended=1217`,
`open_cb_tail_present_pending_timeout_submitted=0`,
`open_cb_tail_present_pending_abandoned_nonappendable=0`, and
`open_cb_tail_present_pending_merge_failed=0`. Locality is close to the 16ms
scout and no longer timeout-driven: command buffers `1.009/present`, render
passes `11.531/present`, tile preservation `124.270MiB/present`,
`gpu_command_buffer_errors=0`, `completion_dequeue_status_error=0`, and no
nonzero PE post-Present HRESULT (`pe_present_call_return_nonzero=0` across
`29274` rows). This is a stronger R-BACK-2.39 mechanism result, but still not a
promotion result: sampled average remains `11.133fps`, no-enqueue wait is not
improved (`completion_wait_ms_per_present=34.787`), and tile preservation
remains above the h220 baseline.

The ordinary session-head prefix retest closes a selector mismatch in the
carry-session path: complete-prefix selection now accepts ordinary non-present
heads, not only `PresentSplitBefore` heads, before a final-Present tail. The
`encode-session-session-head-prefix-r1-20260621` run passes without a D3D9
invalid-call reproduction or GPU/queue error: `pending_started=1215`,
`head_appended=1233`, `tail_appended=1210`, `tail_submitted=1210`,
`timeout_submitted=0`, `nonappendable=0`, `merge_failed=0`, and
`chunk_publish_reason_draw_limit=2448`. Locality shape remains close to the
event-wait scout: command buffers `1.016/present`, render passes
`11.514/present`, tile preservation `124.058MiB/present`, and a normal
non-black effects-heavy output image. This improves R-BACK-2.43 conformance but
does not change the promotion verdict: `completion_wait_with_enqueue=0` and
no-enqueue wait is still `35.016ms/present`.

The semantic-split follow-up then separates source-boundary carry from
semantic pass/barrier splits. Allowing injected command buffers to split at
normal pass boundaries proves the chain machinery works, but the first attempt
resets the sub-CB cap per source and over-splits: command buffers rise to
`5.706/present` (`sub_command_buffers=4.701/present`) even though the image is
normal and `completion_wait_ms_per_present=17.050`. The session-wide cap fix
then applies the R-BACK-2.33 cap across the logical coalesced session instead
of per source. `encode-session-session-cap-r1-20260621` returns to the
baseline command-buffer chain shape: command buffers `4.013/present`, sub-CBs
`3.004/present`, render passes `11.614/present`, `chunk_subcb_count_max=4`,
`subcb_split_suppressed_by_cap=5733`, no timeout-submitted prefixes, no
nonappendable/merge failures, no GPU/queue errors, no invalid-call/D3DERR
strings, and a non-black output image (`mean_luma=67.381`,
`variance=5226.328`). This is the strongest R-BACK-2.41/R-BACK-2.43 alignment
so far. It is still not a production promotion result: the run is
timeout-finalized (`returncode=143` with complete artifacts), tile preservation
is still above baseline at `125.638MiB/present`, and
`completion_wait_with_enqueue=0`.

## Implementation Evidence

The current implementation primitives are useful and should be retained as
default-off infrastructure:

- `EncodeSession` carries active render encoder state across compatible
  source boundaries.
- Session source metadata is compact and ordered by source `seqId`.
- Queue submission records retain the session owner until Metal completion.
- `ReadySlotSnapshot` references live queue slot storage instead of deep-copying
  `ChunkSlot` payload arenas.
- Tail-ready multi-source encode consumes source slots by view, not by merged
  payload copy.
- Fail-open direct submit finalizes an encoded prefix instead of completing it
  inline.
- Encoded pending heads no longer have an inline-completion fallback; if the
  encoded prefix cannot be finalized and submitted, the runtime treats that as
  a fatal queue invariant violation instead of falsely advancing completion.
- Pending-tail release can now use queue-local state instead of wallclock time:
  a pending visible prefix waits for the next ready source while the writer is
  active, and fail-opens through normal submit when the writer becomes inactive
  or the queue stops.
- Carry-session prefix selection accepts ordinary non-present CPU-ready
  sources before the final Present tail, rather than requiring every head to be
  tagged `PresentSplitBefore`.
- Injected open-CB sessions now keep semantic pass/barrier mid-chunk commits
  enabled while deferring source-boundary finalization, so coalesced sources
  can match the baseline command-buffer chain shape.
- The per-chunk sub-CB cap is enforced across a carried `EncodeSession`, not
  reset per source slot.
- Unit coverage now pins both session-owner transfer into
  `QueueSubmissionRecord::retainedPayloads` and retained-payload preservation
  when an encoded head is merged into its Present tail.

Focused verification passed:

```text
meson test -C build-arm64-nowine \
  dxmt9-render-backend-batch-contract-spec \
  dxmt9-queue-completion-sources-spec \
  --print-errorlogs --timeout-multiplier 4

meson test -C build-arm64-nowine dxmt9-verify-tla \
  --print-errorlogs --timeout-multiplier 4
```

## Implication

The remaining wall is not "keep one command buffer open." The tail-ready path
already proves that a session can carry active render-pass state and recover
command-buffer shape, but it does not create useful overlap. The bounded-wait
path creates overlap, but the timeout prefix submit is too common and closes
logical passes before the tail.

The next candidate must satisfy both conditions at once:

- move CPU-ready/encode work into the present wait window;
- avoid timeout-submitting draw-run-tailed prefixes that force final same-key
  reopen, extra render passes, and tile-preservation traffic.

Positive `DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US` values remain diagnostic-only:
the release policy is wallclock-based and therefore does not satisfy the
deterministic production coalescing/publish gate required by R-BACK-2.39.
Accepting ordinary session heads and enforcing a session-wide sub-CB cap fixes
the carrier's selection and command-buffer-chain shape, but it does not by
itself create useful P4 overlap.

Do not spend `.gputrace` on this candidate. The no-gputrace locality/P4 gates
reject promotion.
