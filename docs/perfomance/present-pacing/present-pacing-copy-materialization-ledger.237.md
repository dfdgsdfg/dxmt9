---
domain: present-pacing
workload: 3DMark05 GT2 and SFIV Benchmark
title: "Matched copy/materialization ledger and CPU evidence"
type: evidence
status: partial
updated: 2026-09-01
source: include/dxmt9/copy_materialization_ledger.hpp; tests/native/bridge/pe_diagnostics_spec.cpp; tests/native/backend/source_payload_spec.cpp; experiments/output/app-d3d9-3dmark05-task4-gt2-ledger-default-20260830; experiments/output/app-d3d9-3dmark05-task4-gt2-ledger-direct-20260830; experiments/output/app-d3d9-sfiv-benchmark-task4-sfiv-ledger-direct-20260830; experiments/output/app-d3d9-3dmark05-head-ledger-gt2-20260831; experiments/output/app-d3d9-3dmark05-semantic-owner-ledger-gt2-r2-20260901; experiments/output/app-d3d9-3dmark05-semantic-owner-ledger-bound-gt2-20260901; traces/app-d3d9-3dmark05-task4-gt2-profile-default-20260830; traces/app-d3d9-3dmark05-task4-gt2-profile-direct-20260830
related: docs/perfomance/overview.md; specs/archicture/gap.md; specs/d3d9/recorder/gap.md; specs/backend/gap.md
---

# Matched copy/materialization ledger and CPU evidence

This is the owner-qualified copy/materialization ledger evidence leaf. It
records counters and CPU time, not an FPS claim. The original GT2 pair used
the same current staged build, `perf` profile, no-gputrace wrapper, frame
sampling, and `DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=1`; only
`DXMT9_CPU_READY_TAPE` changed (`0` Tape-off default versus `1` Tape-on
provider). The Tape-independent direct-to-`ChunkSlot` construction path is
enabled in both runs, so this pair does not isolate that path. No command
buffer, pass, or present cadence knob was changed. Both runs passed with zero
GPU command-buffer errors.

## Native gate and disabled-path proof

The focused native gate was green with the ledger both unset/off and enabled:

```text
meson test -C build dxmt9-source-payload-spec \
  dxmt9-pe-chunk-record-value-spec dxmt9-pe-diagnostics-spec \
  dxmt9-managed-mutation-offload-transaction-spec \
  dxmt9-cpu-ready-production-routing-spec dxmt9-chunk-record-allocation-spec
6/6 passed (ledger unset and DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=1)
```

The disabled path resolves a cached null for each owner; the source contract
keeps registry construction behind the cached gate and every event constructor
behind its ledger pointer. The diagnostics source test now bounds the disabled
branch and asserts it contains no `steady_clock`, `fetch_add`, event
construction, `operator new`, or `malloc`. The allocation-count test also
observes no owner allocation with all diagnostic gates off. This proves the
requested clock/atomic/allocation property at the production lookup and cold
owner boundary; it does not claim that unrelated application or Wine work is
allocation-free.

## Ledger coverage

`calls`, `copy MB`, and `copy ms` below are per successfully published Present;
`sem` and `sem MB` are semantic boundary counts/bytes and are not counted as
physical copies. `peak` is the maximum retained byte count observed in the
run. A missing row means zero activity (the report intentionally filters empty
rows).

| Owner / identity | Class | Ownership reason | GT2 Tape-off (calls / copy MB / ms / sem / peak) | GT2 Tape-on (calls / copy MB / ms / sem / peak) | SFIV Tape-on (calls / copy MB / ms / sem / peak) |
|---|---|---|---|---|---|
| PE `materialize.pe.state-shadow` | necessary | producer-visible D3D9 state semantics | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 |
| PE `materialize.pe.wire-final` | necessary | pointer-free PE ownership | 15.579 / 1.244 / 0 / 0 / 229,384 | 15.715 / 1.243 / 0 / 0 / 229,544 | 3.888 / 0.275 / 0 / 0 / 155,788 |
| PE `materialize.pe.builder-temporary` | removable | final-wire layout target owner | 5,150.942 / 0.994 / 0.895 / 0 / 0 | 5,149.223 / 0.993 / 0.967 / 0 / 0 | 1,231.131 / 0.225 / 0.310 / 0 / 0 |
| PE `copy.pe.seal-records` | removable | final offsets known before construction | 14.608 / 0.085 / 0.008 / 0 / 8,192 | 14.746 / 0.085 / 0.009 / 0 / 8,192 | 3.885 / 0.019 / 0.002 / 0 / 8,192 |
| PE `copy.pe.seal-handles` | removable | final handle capacity reservable transactionally | 14.608 / 0.069 / 0.007 / 0 / 9,344 | 14.746 / 0.069 / 0.007 / 0 / 9,360 | 3.881 / 0.012 / 0.002 / 0 / 6,944 |
| PE `copy.pe.seal-payload` | removable | typed producers final payload range | 14.608 / 1.089 / 0.034 / 0 / 0 | 14.746 / 1.088 / 0.035 / 0 / 0 | 3.885 / 0.243 / 0.008 / 0 / 0 |
| Unix `copy.bridge.raw-owned` | necessary | process ABI ownership; no shared ownership ABI | 15.579 / 1.244 / 0.043 / 0 / 568,056 | 15.715 / 1.243 / 0.045 / 0 / 811,492 | 3.888 / 0.275 / 0.010 / 0 / 481,624 |
| Unix `materialize.replay-submission-carrier` | removable | bounded planning final queue storage | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 |
| Unix `copy.replay.submission-carrier` | removable | final queue destination after bounded planning | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 |
| Unix `materialize.queue-final` | necessary | immutable queue ownership | 0.036 / 0.001 / 0 / 0 / 0 | 14.477 / 0.273 / 0.035 / 41.778 / 0 | 0.311 / 0.005 / 0.001 / 0.761 / 0 |
| Unix `copy.gpu.upload` | necessary | CPU allocation is not GPU-readable | 1,087.065 / 12.375 / 1.177 / 0 / 0 | 1,087.523 / 12.774 / 1.260 / 0 / 0 | 378.937 / 1.127 / 0.086 / 0 / 0 |
| Unix `materialize.gpu.shared` | necessary | first GPU-readable owned representation | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 |
| Unix `copy.arena.bytes` | necessary | queue-visible arena representation | 0 / 0 / 0 / 0 / 0 | 41.778 / 0.032 / 0.006 / 0 / 0 | 0.761 / 0 / 0 / 0 / 0 |
| Unix `materialize.mutation.staging` | necessary | asynchronous offload ownership | 0.768 / 0.076 / 0.005 / 0.768 / 0 | 0.797 / 0.079 / 0.006 / 0.797 / 0 | 0.260 / 0.003 / 0.001 / 0.260 / 0 |
| PE `materialize.up.scratch` | necessary | pointer-free wire construction transaction | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 0 / 51.898 / 0 |
| PE `materialize.pe.section-append` | necessary | PE section ownership before sealing | 0 / 0 / 0 / 6,671.424 / 0 | 0 / 0 / 0 / 6,669.701 / 0 | 0 / 0 / 0 / 1,447.390 / 0 |

No `ReplaySubmissionCarrier*` event is emitted: the batch source contract
explicitly checks that the implementation does not claim a false whole-carrier
copy. The Tape-on route's queue-final and arena rows are real final-slot/source
delivery activity, while the Tape-off route has no arena row in this lane.

### Current-worktree ledger correction (2026-08-31)

The expanded production ledger now records the actual per-record replay
carrier field copies instead of treating only a whole-carrier copy as
observable. A fresh GT2 run completed 1,728 Presents with zero chunk rejects
and zero GPU command-buffer errors. Its final cumulative rows normalize to:

| Identity | calls / Present | MB / Present | ms / Present |
|---|---:|---:|---:|
| PE builder temporary | 5,132.172 | 0.991 | 0.854 |
| PE seals, three rows combined | 43.461 | 1.240 | 0.043 |
| bridge raw ownership | 15.451 | 1.240 | 0.042 |
| replay submission-carrier field copy | 1,476.209 | 15.740 | 0.627 |
| GPU upload | 1,083.183 | 11.810 | 1.034 |
| mutation staging | 0.708 | 0.070 | 0.004 |

Thus PE builder plus seals remains about `0.897 ms/Present`, but replay carrier
copy is also a real `0.627 ms/Present` removable class. The older zero row and
the ranking statement that followed it were limits of the former observer,
not proof that field-level carrier materialization was absent. Global byte
sums remain invalid because semantic materialization and physical ownership
rows can describe the same payload at different boundaries.

### Production semantic-owner binding (2026-09-01)

After `PeSemanticBatchOwner` became the sole production final-wire owner, its
two ownership boundaries were connected to the PE-qualified ledger. The same
GT2 Tape-off recipe was repeated with `perf`, frame sampling,
`DXMT_LOG_LEVEL=info`, and `DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=1`:

```text
DXMT_EXPERIMENT_PROFILE=perf DXMT_LOG_LEVEL=info \
DXMT_3DMARK05_LANE=gt2 DXMT9_CPU_READY_TAPE=0 \
DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=1 \
DXMT9_PERF_FRAME_SAMPLING=1 \
scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-3dmark05 \
  --output-suffix semantic-owner-ledger-bound-gt2-20260901
```

The run passed 1,647 Presents with zero chunk rejects and zero GPU
command-buffer errors. Its output preserved the staged artifact hashes. The
preceding owner-promoted r2 run used the same recipe but predates these two PE
instrumentation sites.

| Identity | calls / Present | MB / Present | inclusive ms / Present | retained peak |
|---|---:|---:|---:|---:|
| PE semantic-owner admission | 2,665.815 | 2.693 | 2.564 | 368,064 B |
| PE ExactFixed final wire | 15.645 | 1.252 | 0.908 | 229,384 B |
| Unix bridge raw ownership | 15.645 | 1.252 | 0.043 | 449,696 B |
| Unix replay carrier copy | 1,486.697 | 15.852 | 0.643 | 0 B |
| Unix queue final | 1.780 | 0.037 | 0.008 | 0 B |
| Unix GPU upload | 1,093.570 | 12.241 | 1.068 | 0 B |
| Unix mutation staging | 0.744 | 0.074 | 0.005 | 0 B |

`materialize.pe.builder-temporary` and all three `copy.pe.seal-*` rows are
absent, confirming that production no longer fabricates the retired builder or
seal classes. Admission bytes are the increase in live typed record, pin,
sparse, rectangle, and variable-payload extents. Its timer spans the complete
atomic admission, including retain and PendingDelta settlement, so it is a
transaction cost rather than a memcpy-only row and must not be compared
directly with the old builder-copy number. ExactFixed measures the actual
contiguous zero/header/table/arena emission. Both retained rows returned to
zero at settlement; failed native admissions emit no event, and repeated
ExactFixed emission counts work without double-retaining the fixed buffer.

Cadence remained invariant against the preceding r2 observation: command
buffers were `3.999/P` in both runs and render passes changed from `15.765/P`
to `15.761/P`. Sampled frame rate changed from `26.290` to `25.445 FPS` in
these single runs. That delta is not a product-performance result: the new
diagnostic reads two clocks and updates atomics for approximately 2,666
admissions per Present and is default-off. The evidence closes ownership and
cost visibility while intentionally leaving a matched ledger-off performance
gate separate.

## Matched GT2 CPU and cadence

| Route | Presents | CB / Present | Pass / Present | PE builder + seals ms / Present | Bridge raw ms / Present | GPU upload ms / Present | Producer profile weight | Encode profile weight |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `DXMT9_CPU_READY_TAPE=0` | 1,593 | 3.999 | 15.788 | 0.944 | 0.043 | 1.177 | 19,417 ms | 4,989 ms |
| `DXMT9_CPU_READY_TAPE=1` | 1,533 | 4.015 | 15.957 | 1.018 | 0.045 | 1.260 | 18,166 ms | 4,753 ms |

The wrapper's fixed cadence remains approximately four command buffers and
sixteen passes per Present in both runs. The Tape-on route therefore supplies a
matched source-delivery observation, not a performance promotion: it changes
the observed total and has a small cadence difference within the noisy run
shape, so no FPS conclusion is drawn.

The 20-second Xcode system-trace sidecars completed and exported CPU profiles.
Both producer selectors were overwhelmingly `Running` (`19,423/19,424` and
`18,172/18,173` samples) with zero producer wait-keyword hits; both verdicts
are `producer-state-inconclusive`, not proof of a producer bottleneck. The
default trace has `main-thread-holder-positive` with 28 main-thread holder
hits; direct has 21. The full summaries are retained at
`traces/app-d3d9-3dmark05-task4-gt2-profile-{default,direct}-20260830/analysis/`.

## Broader CPU-heavy workload: SFIV

The current direct-route SFIV run passed with 2,761 Presents, 3.938 command
buffers/Present, 22.910 passes/Present, zero GPU errors, `3,852.521 ms`
submit-draw CPU, `7,593.5 ms` encode-draw CPU, and `6,819.085 ms` completion
wait over the complete result. The direct CPU-ready planner completed 125 of
127 attempted windows and recorded 250 completion sources; these counters
show the route was exercised but do not isolate a copy owner or establish an
FPS gain.

## Next-owner ranking and queue T2d gate

1. **PE recorder: builder temporary plus seal tables/payload.** This is the
   strongest removable CPU class: about `0.944-1.018 ms/Present` in GT2 and
   `0.322 ms/Present` in SFIV (builder plus three seal rows). A separate
   all-family exact-final-layout follow-up should preserve exact wire bytes and
   rollback, then repeat the ledger and native fault matrix.
2. **Unix backend: GPU upload bytes.** This is the largest physical transfer
   observed at `12.375-12.774 MB/Present` and `1.177-1.260 ms/Present` in GT2;
   SFIV is `1.127 MB` and `0.086 ms`. It is necessary today, so a later GPU
   upload investigation needs
   source-qualified reuse/hoisting evidence and GPU correctness before trying
   to remove it.
3. **PE/Unix boundary: raw ownership handoff.** Final-wire and bridge rows
   each carry about `1.24 MB/Present` in GT2 for only `0.043-0.045 ms` measured
   bridge copy time, and `0.275 MB/Present` / `0.010 ms` in SFIV. Investigate
   whether the pointer-free ABI can transfer ownership without another byte
   copy, but retain the current necessary owner boundary until a byte-identity
   proof exists.
4. **Unix queue/mutation staging and direct arena bytes.** These are small in
   this evidence (`<=0.041 ms/Present` in GT2, `<=0.001 ms/Present` in SFIV)
   and do not justify queue T2d by themselves without a measured waiting
   victim and a workload showing a different shape.
5. **Replay carrier rows.** The former observer saw no whole-carrier event, but
   the current field-level ledger measures `15.740 MB/Present` and
   `0.627 ms/Present` in GT2. This is now the second removable CPU class after
   PE builder/seal work. Expand carrier-free replay only through exact
   source-qualified destination construction; ordered-control and unsupported
   producer families remain fail-closed.

For the planned queue T2d reserve-copy-commit decision, this evidence is a weak
economic signal: queue/mutation/arena materialization is small and the Xcode
producer verdict is inconclusive. T2d must therefore remain deferred unless
the end-to-end lifecycle observer and matched queue-mutex profiles find a real
waiting victim above the specified threshold. The broader ranking is an
owner-qualified next-investigation order, not a default policy recommendation;
repeated matched runs and the existing GPU oracle remain required.
