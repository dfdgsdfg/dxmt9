---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 36
title: Current-HEAD Producer Queue-Mutex Proof Closure
date: 2026-08-25
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-qmutex-current-head-gt2-info-off-r1-20260825; experiments/output/app-d3d9-3dmark05-qmutex-current-head-gt2-info-on-r1-20260825
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.30.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md; specs/backend/producer-concurrency/spec.md
---

# Current-HEAD Producer Queue-Mutex Proof Closure

## Question and method

The T2a' implementation had outlived part of its gap text: the deterministic
interleaving harness and restamp counters were present, while the encode
scheduling tracker still described both as open. This run re-binds the proof
stack and queue-mutex mechanism measurement to current master `3eaac5a8`.

The matched runs used the same staged artifacts, Sikarugir-CX 24.0.7, the
`perf` profile, GT2, a 120-second supervised run, frame sampling,
`--keep-frontmost`, no gputrace, and no encoder breakdown. Both used
`DXMT_LOG_LEVEL=info`; the ON arm additionally enabled
`DXMT9_PERF_QUEUE_MUTEX_SPLIT=1` and
`DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT=1`. `Info` is required because the
per-site queue table is emitted at that level. An earlier warn-level ON scout
completed correctly but intentionally supplied no queue-table rows and is not
used for the mechanism result.

```sh
DXMT_3DMARK05_ARGS='-gt2 -nosplash -nosysteminfo -noscreens' \
DXMT_3DMARK05_RESULT_FILE=dxmt9_gt2.3dr \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix qmutex-current-head-gt2-info-off-r1-20260825 \
  --frame 60 --no-gputrace --no-encoder-breakdown --frame-sampling \
  --keep-frontmost --timeout 120 --top 5 --dxmt-log-level info

DXMT_3DMARK05_ARGS='-gt2 -nosplash -nosysteminfo -noscreens' \
DXMT_3DMARK05_RESULT_FILE=dxmt9_gt2.3dr \
DXMT9_PERF_QUEUE_MUTEX_SPLIT=1 \
DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix qmutex-current-head-gt2-info-on-r1-20260825 \
  --frame 60 --no-gputrace --no-encoder-breakdown --frame-sampling \
  --keep-frontmost --timeout 120 --top 5 --dxmt-log-level info
```

The OFF/ON staging hashes are byte-identical. Both runs are `status=pass`,
return code zero, `gpu_command_buffer_errors=0`, and
`command_chunk_rejects=0`.

## Mechanism result

The final complete queue-table emission is at present 1,800; rows are
cumulative, so the table uses each site's last emission rather than summing
periodic snapshots.

| current-HEAD site | acquires/present | acquire wait ms/present | hold ms/present |
|---|---:|---:|---:|
| `find_reordered_index_buffer` | `405.745` | `0.7851` | `0.0823` |
| `mark_chunk_resources_and_capture_buffer_bindings` | `15.802` | **`0.1500`** | `0.0027` |
| `submit_draw_run_batch_impl` | `22.567` | `0.0643` | n/a |
| `note_present_dequeued` | `0.999` | `0.0354` | `0.0002` |
| `submit_draw_run_batch_impl/append` | `562.148` | `0` | **`3.3471`** |
| **all acquire-wait rows** | — | **`1.0536`** | — |

The required T2a' signature is present:

- `submit_draw_run_batch_impl/mark` has zero rows; the worker mark segment did
  not move to another queue-mutex site.
- Producer mark/capture acquire-wait is `0.1500 ms/present`, consistent with
  the historical post-T2b/c `~0.148` and below T2a's `0.194`; the pre-T2a
  value was `0.601`.
- Total acquire-wait is `1.0536 ms/present`, consistent with the historical
  post-T2b/c `~1.00`; the pre-T2a value was `2.14`.
- The commit phase independently reports `mark=0.4761`,
  `mark_lock=0.1524`, `mark_core=0.3347`, and `mark_dedup=0.1105`
  ms/present. The queue row and commit-side lock scope agree within ordinary
  instrumentation boundary differences.
- `mark_ticket_restamp_checks=1,069,104` and
  `mark_ticket_restamp_fires=0` (`~575.1` checks/present). This sizes the
  window for this workload; zero fires is not used to remove or weaken the
  protocol.

The same-binary instrumentation pair produced 1,835/1,829 steady-body frames
(`frame >= 30`, `0 < wall_ms <= 200`). Harmonic scene FPS was
`29.779 -> 29.687` (`-0.31%`); median FPS was `31.233 -> 31.281`
(`+0.15%`). One pair is not an FPS promotion experiment. It establishes only
that the heavy observer did not visibly change this run class.

## Proof stack and exact scope

All four canonical build directories compiled at current HEAD. The following
gates passed:

```sh
meson test -C build --print-errorlogs \
  dxmt9-producer-interleaving-spec dxmt9-producer-mark-reclaim-spec
meson test -C build --print-errorlogs dxmt9-thread-ownership-audit
meson test -C build --print-errorlogs dxmt9-verify-tla

meson setup build-tsan -Db_sanitize=thread -Db_lundef=false
meson compile -C build-tsan \
  dxmt9-producer-interleaving-spec dxmt9-producer-mark-reclaim-spec
TSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
  build-tsan/tests/native/backend/dxmt9-producer-interleaving-spec "$PWD"
TSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
  build-tsan/tests/native/backend/dxmt9-producer-mark-reclaim-spec
```

The TSan interleaving run completed with 384 random seeds, 373 distinct
outcomes, 395,109 release/acquire observations, and zero violations. Its
deliberately relaxed control also produced zero violations on this host, so
that free-running lane demonstrated no detection power and is not cited as a
negative proof. The effective stack is instead:

1. `ProducerMarkReclaim.tla` proves the abstract protocol and runs the pin,
   restamp, and capture counterexample configurations as expected failures.
2. `dxmt9-producer-mark-reclaim-spec` binds the model predicates and traces to
   shared C++ predicates.
3. `dxmt9-producer-interleaving-spec` drives real `CommandQueue`, `Pool`,
   arena locks, atomics, and producer entry points; its source contract pins
   the production release/acquire and restamp ordering.
4. The explicit TSan binary checks the real threaded path without relying on
   Meson's default exit behaviour.
5. The wild counters show the intended queue-mutex shape and preserve the
   real-workload safety/error gates.

The proof remains deliberately bounded. The scripted scheduler supplies its
own happens-before edge; slot publication and the decomposed restamp branch
are mirrored where the native fixture cannot construct the full Metal source
pipeline; and the worker-side `submitDrawRunBatchImpl` call-site surroundings
are covered through the shared helper and wild row, not directly invoked by
the native fixture. This is a refinement stack, not a claim that TLC proves
the C++ memory model.

## Verdict

The stale T2a' evidence debts are closed: current-HEAD profile, deterministic
interleaving binding, TSan lane, restamp observability, and GT2 runtime safety
all exist and agree. The remaining concurrency item is different: the
producer still performs the atomic ticket acquire and frozen-ticket re-read.
Removing that final acquire requires a new reservation/sufficiency proof; it
is not licensed by the zero-fire observation. T2d reserve-copy-commit also
remains deferred. The current queue table identifies the encode-side index
lookup and worker append hold as the large residual rows, but this run alone
does not promote either as the next FPS owner.
