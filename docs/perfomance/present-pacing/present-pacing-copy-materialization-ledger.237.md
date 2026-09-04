---
domain: present-pacing
workload: 3DMark05 GT2 and SFIV Benchmark
title: "Matched copy/materialization ledger and CPU evidence"
type: evidence
status: partial
updated: 2026-09-01
source: include/dxmt9/copy_materialization_ledger.hpp; tests/native/bridge/pe_diagnostics_spec.cpp; tests/native/backend/source_payload_spec.cpp; experiments/output/app-d3d9-3dmark05-task4-gt2-ledger-default-20260830; experiments/output/app-d3d9-3dmark05-task4-gt2-ledger-direct-20260830; experiments/output/app-d3d9-sfiv-benchmark-task4-sfiv-ledger-direct-20260830; experiments/output/app-d3d9-3dmark05-head-ledger-gt2-20260831; experiments/output/app-d3d9-3dmark05-semantic-owner-ledger-gt2-r2-20260901; experiments/output/app-d3d9-3dmark05-semantic-owner-ledger-bound-gt2-20260901; experiments/output/app-d3d9-3dmark05-semantic-owner-ledger-off-gt2-20260901; traces/app-d3d9-3dmark05-task4-gt2-profile-default-20260830; traces/app-d3d9-3dmark05-task4-gt2-profile-direct-20260830
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
admissions per Present and is default-off.

The same recipe was then repeated with only
`DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=0`. It passed 1,693 Presents at
`26.150 FPS`, `3.9994` command buffers/Present, and `15.7631` render
passes/Present, with zero GPU command-buffer errors, zero chunk rejects, and
zero ledger report rows. The adjacent enabled run was `25.445 FPS`, so the
disabled run is `2.77%` faster while preserving cadence. This is a single-run
observer-cost direction check, not a product-performance or promotion claim;
the intervening source change only moved six continuation counters into the
shutdown report table and did not change the measured hot path. Repeated
matched runs remain necessary before assigning a stable observer cost.

### Calibrated semantic-owner phase split (2026-09-04)

The owner ledger above is intentionally inclusive and updates a high-frequency
clocked ledger on every admission. It therefore establishes ownership and byte
volume, but does not isolate product CPU cost. A matched GT2 direction check
used `DXMT9_PE_STATS_DECIMATION=127` with the new phase observer OFF and ON.
Both runs completed without chunk rejects or GPU errors; the healthy populations
were 1,594 and 1,587 Presents, so the pair is directional evidence rather than
a promotion-quality A/B.

After subtracting the measured empty clock-pair cost, the ON run attributed:

| Semantic-owner phase | calibrated ms / Present |
|---|---:|
| prepare admission | 1.045 |
| fixed values and direct pins | 0.277 |
| sparse and variable copy | 1.255 |
| canonical materialization and metrics | 1.098 |
| PendingDelta settlement callback | 0.415 |
| settle and clear | 0.641 |
| ExactFixed plan | 0.018 |
| ExactFixed role copy | 0.057 |
| ExactFixed inclusive parent | 0.099 |

The child rows are individually calibrated; parent and child rows remain
inclusive and must not be added together. This corrects the interpretation of
the earlier `0.908 ms/Present` ExactFixed ledger row: that diagnostic remains a
valid owner/byte ledger, but its clock and atomic work dominate this short
operation and it is not a product-cost estimate. Removing the contiguous gather
is therefore no longer the first PE optimization. The next bounded candidate is
to eliminate repeated sparse/identity work between admission, typed copy, and
canonical materialization while preserving the prepared witness, qualified
retention, rollback, and byte-equivalence contracts.

The first implementation of that witness was deliberately rejected: it
preserved canonical bytes, but repeated a linear deduplication inside the
admission pass and merely moved work from materialization to preparation. The
corrected implementation records the identity only when the admission set
reports its first wire-visible promotion, uses a bounds-only append under that
proof, and resets only the active witness frontier. A clean follow-up GT2 run
completed with zero rejects/errors and the following calibrated direction:

| Semantic-owner phase | pre-witness ms / Present | corrected witness ms / Present |
|---|---:|---:|
| prepare admission | 1.045 | 1.262 |
| fixed values and direct pins | 0.277 | 0.338 |
| sparse and variable copy | 1.255 | 1.094 |
| canonical materialization and metrics | 1.098 | 0.913 |
| PendingDelta settlement callback | 0.415 | 0.379 |
| five child phases above | 4.090 | 3.987 |
| ExactFixed inclusive parent | 0.099 | 0.091 |

The populations were 1,587 and 1,624 Presents and the result-file values were
25.359 and 25.948 FPS. Their approximately 2.3% difference prevents a matched
promotion claim; the useful evidence is narrower: the corrected witness no
longer shifts the aggregate child cost upward, removes the second handle-order
traversal, and moves the remaining PE target to sparse typed-copy plus canonical
payload construction. One run performed while a native rebuild was active is
explicitly excluded from this comparison.

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

1. **Replay carrier rows.** The current owner-qualified run measures
   `15.852 MB/Present` and `0.643 ms/Present` in GT2. This is the only remaining
   measured, removable CPU materialization class. Expand carrier-free replay
   only through exact source-qualified destination construction; ordered
   control and unsupported producer families remain fail-closed.
2. **PE sparse copy and canonical materialization.** The corrected prepared
   identity/order witness removes the second handle-order traversal. The clean
   follow-up still attributes about `1.094` and `0.913 ms/Present` to sparse
   typed copy and canonical construction. Any further fusion must keep retain,
   rollback, PendingDelta, and exact-wire equivalence mandatory and must not
   recreate a second identity set or per-record full-array clear.
3. **PE final emission and segmented transport.** `ExactFixed` still moves
   `1.252 MB/Present`, but the calibrated phase split measures only about
   `0.099 ms/Present` inclusive (`0.057 ms/Present` in role copies). It remains
   useful as a representation simplification, especially outside capture, but
   is no longer the leading CPU-performance candidate.
4. **Unix backend GPU upload.** This necessary transfer is
   `12.241 MB/Present` and `1.068 ms/Present` in the current GT2 evidence. A
   later GPU-side investigation needs source-qualified reuse or hoisting
   evidence and the GPU correctness oracle before trying to remove it.
5. **PE/Unix raw ownership handoff.** The bridge carries
   `1.252 MB/Present` for `0.043 ms/Present`. Its byte volume is visible, but
   its measured CPU cost is too small to justify weakening the pointer-free
   ownership boundary without a byte-identity proof.
6. **Queue and mutation staging.** Queue finalization is
   `0.008 ms/Present` in the current run; prior queue/mutation evidence is also
   below the T2d promotion threshold. It does not justify reserve-copy-commit
   work without a measured waiting victim.

The former PE builder/seal ranking is historical and closed: the production
all-family semantic owner now emits the final wire transaction, so those
removed temporary rows are no longer a next-owner candidate.

For the planned queue T2d reserve-copy-commit decision, this evidence is a weak
economic signal: queue/mutation/arena materialization is small and the Xcode
producer verdict is inconclusive. T2d must therefore remain deferred unless
the end-to-end lifecycle observer and matched queue-mutex profiles find a real
waiting victim above the specified threshold. The broader ranking is an
owner-qualified next-investigation order, not a default policy recommendation;
repeated matched runs and the existing GPU oracle remain required.
