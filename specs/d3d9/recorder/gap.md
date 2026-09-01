---
type: "Spec Gap"
title: "PE Recorder Gap"
description: "Implementation and evidence gaps against the PE recorder state and transaction contract."
tags: [specs, gap, d3d9, recorder, pe]
---

# PE Recorder Gap

Domain-owned tracker for [requirements.md](requirements.md). The parent rollup
is [d3d9/gap](../gap.md); formal evidence gaps also appear in
[verification/gap](../../verification/gap.md).

## Direct final-wire and capability workstreams

| Area | Current implementation truth | Required closure |
|---|---|---|
| Fixed-role `SegmentedTransportV1` and later `ExactFixed` CPU Tape emission (`R-CORE-REC-7.6`–`7.8`) | mandatory all-family production semantic owner, opt-in PE→Unix fixed-region transport, and bounded direct-Arena `CpuReadySemanticTransfer` owner implemented | The semantic owner is now the sole production final-wire transaction. Contiguous ExactFixed remains the default/capture representation; the negotiated 144-byte segmented descriptor carries the immutable producer interval and remains selected only by `DXMT9_PE_SEGMENTED_TRANSPORT`. CPU-ready pooled storage and broader Wine/GPU/locality evidence remain separate promotion work. |
| PE final wire blob (`R-CORE-REC-7.1`–`7.2.1`) | `PeSemanticBatchOwner` is the sole production owner for all 21 producer rows, exact typed pins, PendingDelta settlement, buffer hazards, Render Tape pending leases, ExactFixed capture, and negotiated segmented emission. Device construction fails closed when its bounded storage cannot be established; append/seal/transport failure has no legacy production fallback. `CommandChunkBuilder`, `PePrewireChunkTransaction`, and `PeOwnedRecordCandidate` remain differential or Bootstrap test oracles only. The owner-qualified ledger now records successful typed admission and contiguous ExactFixed emission, cancels rolled-back admissions, and releases retained extents on settlement/reset. | Focused owner/batch/commit/capture native tests and current-toolchain x64/x86 PE builds pass. The 2026-09-01 GT2 Tape-off ledger-enabled run passed 1,647 Presents with zero rejects/errors, `3.999` CB/P and `15.761` passes/P; builder/seal rows were absent, while admission measured `2.564ms/P` inclusive and ExactFixed `0.908ms/P`. A ledger-disabled direction check passed 1,693 Presents at `26.150 FPS`, `3.9994` CB/P, and `15.7631` passes/P with zero rejects/errors and no ledger rows. Admission includes retain and PendingDelta settlement and is not directly comparable with the retired memcpy-only builder row; the adjacent single-run FPS delta is not a promotion claim. Bounded Wine all-family fault evidence and supervised GT1/GT2/GT3/SFIV correctness/locality runs remain required. Larger debug `DXMT9_PE_CHUNK_MAX_RECORDS` values above the fixed 256-record owner bound fail closed. |
| Typed recorder borrow/lock capability (`R-CORE-REC-7.3`) | `RecorderLockCapability`, `RecorderBorrow<T>`, and non-movable `SparseStatePlan` remain production-shaped differential-oracle types, but the promoted runtime no longer constructs a compact plan. Production prepares one recorder-owned `SparseStateInput` under `PeRecorderGuard` and synchronously transfers it into the semantic owner's typed arenas before the guard can end. Reset/poison/teardown still advance the recorder transaction epoch; native exhaustive capability and stale-epoch negatives continue to protect the oracle used for byte-equivalence checks. | Wine Reset fault evidence remains required. The source audit must keep `buildSparseStatePlanForRecord` and every plan-selection branch absent from `D3D9DeviceImpl`; C++ cannot reject a deliberately retained raw reference inside an arbitrary test callback. |
| Heterogeneous semantic projection (`R-CORE-REC-7.4`–`7.5`) | The existing default-off `DXMT9_PE_SCALAR_SEMANTIC_OBSERVER` gate allocates a separate all-family cold ledger; the mandatory production owner is independent of diagnostics and adds one heap-owner pointer to `D3D9DeviceImpl`. One producer table covers 21 exact record families. The common append envelope issues an observer source ordinal before CapacityPre when observation is enabled and, after semantic-owner admission, records exact category/record-key/payload value and every full wire identity. Effect-unknown bridge failure retains accepted tokens until discard; capture materialized/rejected/skipped settles only after acceptance. The table generates `PeRecorderSemanticProjectionTable.tla`; the owning model plus ten counterexample configs and exhaustive native all-row/field/differential/lifecycle tests execute shared predicates. | Typed chunk reserve/unique-retain/capture-throw injection, bounded Wine all-family faults, and wild observer evidence remain open. An observer-enabled single record above the 16 MiB proof arena or above 64 qualified identities intentionally fails stop; the cold oracle does not prove PE COM behavior, bridge ABI effects, allocator failure, or pixels. |

## Bounded common fault envelope (2026-08-31)

`DXMT9_PE_RECORDER_FAULT` now supplies a separate, typed, one-shot seam for
CapacityPre reserve, typed retain/acquire, bridge pre-effect, bridge entered/effect
unknown, active capture disposition/throw, Reset, and teardown. Pre-effect
capacity and retain faults return before acceptance; bridge-pre keeps the sealed
bytes retryable; entered bridge faults poison because the C ABI effect is
unknown; capture faults are only armed while capture is active; Reset returns
the injected HRESULT and teardown consumes without changing cleanup. The
retain selector is at the actual unique acquisition boundary: decimal
`retain_acquire=0`, `=1`, or `=N` fails after that many successful unique
retains, so the matrix distinguishes pre-first from partial-retain rollback;
duplicate handles do not consume the budget. The native matrix proves this
transaction algebra and the script `scripts/tools/run_d3d9_fault_matrix.py`
drives one selector through the canonical runner per fresh process. This is
deliberately not exact all-family owner promotion or x64/x86 Wine COM evidence:
the current-toolchain cross build and live Wine matrix remain open, and no
runtime evidence is claimed here. The recorder matrix selects the
architecture-matched clean-room auxiliary by default; `--aux-exe` remains a
repeatable override for staged-build layouts.

The current Sikarugir x86 matrix passes all nine recorder points in fresh
processes: CapacityPre, retain before/after one unique acquisition, bridge-pre,
bridge-entered, capture disposition/throw, Reset, and teardown. The capture
lane also pins two boundary defects found by that run: the runner projects its
host output root through Wine's absolute `Z:\\` path, and capture disposition
defers abort until the accepted recorder transaction settles its reserved
token/ordinal, so optional capture rejection cannot become device loss. The
remaining gap is live x64 evidence and broader wild evidence for the
default-off all-family production adapter lane, plus CPU-ready Tape integration.
The clean-room fixture
`tests/conformance/d3d9/d3d9_recorder_fault.cpp` closes the former driver
false-positive: the recorder matrix defaults to an empty main range and runs
one fresh fixture process per selector, with `SELECTED:` and `REACHED:` output
markers plus the exact `DXMT9_PE_RECORDER_FAULT_CONSUMED=<selector>` receipt
from the production consume seam. The runner requires both lines, so a stale
fixture/runtime, wrong selector, or post-check without seam consumption fails.
It deliberately reaches CapacityPre (using a fixture-only 50-byte
chunk cap), zero/partial unique retain rollback with two resource identities,
bridge-pre same-sealed-byte retry, bridge-entered fail-stop/Reset recovery,
teardown-after-drain, and active Render Tape capture disposition/throw when
the runner can configure capture. Missing capture setup is an explicit
`SKIP:recorder_fault_matrix:` result with rc=77, never a pass; unknown and multi-selector
values fail before COM work. An unavailable D3D9 runtime is likewise an
explicit `SKIP:recorder_fault_matrix:` result. Teardown's receipt is emitted
inside the device destructor seam before the post-scope `REACHED:` marker. The
x64 and x86 fixture variants are manifest
scaffolds and still require exact app-local/builtin Wine runs; no runtime
evidence is claimed by this change.

Dependencies are ordered: heterogeneous tokens and fault seams precede the
full legacy/direct oracle; the exact final-layout primitive precedes a
replayable producer transaction; typed capabilities precede any asynchronous
code review claim; formal/native evidence precedes PE/Wine evidence, which
precedes wild counter and performance evidence. Conservative unused handle
capacity cannot shift the payload offset because exact wire bytes are required,
and a post-build compaction copy would recreate the class this workstream
removes. The internal primitive changes neither the production builder route,
wire ABI, recorder cadence, nor D3D9 semantics.

The exact singleton path reuses the persistent builder's final-byte vector and
preserves every legacy staging-vector capacity, including `handleObjects_`,
which carries the local half of warm ownership. Native capacity/ownership
coverage pins this behavior; it does not claim that exact preparation is
allocation-free for the final blob.

All-family acceptance is likewise bound to the latest issuance, not just
structural fields: the producer/record family and source ordinal must equal
the most recent `beginSource` call. Native A/B/A, wrong-producer, and
wrong-source rows plus the companion TLA counterexamples cover this distinction;
the remaining gap is runtime fault and Wine evidence rather than a claim that
unrelated facts are accepted. Successful acceptance consumes and clears the
latest issuance, so a second accept without a new `beginSource` fails closed;
pre-effect failures retain the issuance through `preserveForRetry`.

The semantic row has ten total field/settlement/latest-issuance
counterexamples (the original seven plus A/B/A, wrong producer, and wrong
source); the broader verification inventory count remains owned by its current
rollup.

## Stable PE decomposition audit

`scripts/check/audit_d3d9_pe_abi_codegen.py --source-only` and
`scripts/check/pe_device_abi_manifest.toml` are the source-level acceptance
surface for R-CORE-REC-5.2.2/5.2.3. They pin the QueryInterface key-function
owner, `PeRecorderState` x64/x86 sizes, critical device member order, exact
cold symbol owners, and the export allowlist while deliberately excluding
addresses, RVAs, timestamps, archive/relocation order, whole-object hashes,
and aggregate text size. Native Meson invokes source-only mode, so PE artifacts
are not a native-test prerequisite.

The physical residual is now honestly bounded at 3,713 lines for
`d3d9_pe_device_impl.hpp`; this is compile surface, not a cosmetic line-count
claim. Exact x64/x86 codegen requires the legal, reachable in-class bodies of
`SetStreamSource`, `Present`, and `DrawIndexedPrimitive`, plus the manifest-listed
hot helper/template set (`PeCallScope`, hot-setter/call wrappers, constant and
draw cores, append timers, the common all-family semantic source/accept branch,
recorded-ref helpers, and their small predicates).
All other hot state/draw/scene definitions have physical ownership in
`d3d9_pe_device.cpp`; no state implementation fragment remains. The cold
StateBlock Prepare/Commit pair, implicit-FVF resolver, and constant range/read
helpers remain in `d3d9_pe_device_com_cold.cpp`. `QueryInterface` remains the
deliberate key function in `d3d9_pe_device.cpp`.
A future cross run must use `wine_builtin_dll=true`; similarly named
`wine_builtin_dll=false` output is not evidence.

| Area | Status | Current evidence | Missing work / acceptance |
|---|---|---|---|
| Closed StateBlock category/storage surface (`R-CORE-REC-1.2`, `5.1.1`) | ✅ authoritative physical inventory + private flat tables + canonical PE compilation / ⚠️ Wine evidence | One 26-row Apply physical inventory names four keyed stores, sixteen fixed stores, and all six VS/PS F/I/B constant stores. It generates fixed storage/accessors and typed physical iteration; clear and candidate COM lifetime walk the same visitor, while Prepare and Commit instantiate all 26 rows with dependent compile failures for a newly unbound keyed, fixed, or constant store. The candidate-owned vertex declaration remains distinct from staged/transferred bindings. Fixed tables keep arrays, occupancy, and counters private; bounded category keys reject invalid factories before retain. Writer/snapshot APIs close mutable candidate and wrapper-snapshot access without changing pinned footprints. Native exact-scope/negative-access pins live in `dxmt9-pe-stateblock-category-spec` and `dxmt9-pe-typed-slot-spec`; canonical x64 and x86 PE builds compile the instantiated device and child paths. | The existing StateBlock TLA inventory remains the serial/reference algebra; Wine Apply/COM instrumentation remains open. |
| Command-chunk variable payload surface (`R-CORE-REC-3.1.1`) | ✅ closed production API + atomic negative pins / ⚠️ Wine evidence | Raw builder byte append/overwrite is private. Sparse POD sections use an exact type→kind registry plus schema-rule bounds; constant sections/records, UP byte sections, Clear rectangles, placeholder tables, and final table overwrite use dedicated adapters. Every adapter rejection atomically rolls back the active record. SetConst/Clear tails bind their already-written fixed header, and final overwrite binds the active draw header plus canonical kind order, element/count/byte schema, alignment, sequential payload range, and embedded constant range. `dxmt9-pe-chunk-record-value-spec` proves ignored failures cannot commit and runs the valid non-draw/sparse producer matrix through the same callsites. | C++ has no general recursive “contains no pointer member” reflection; pointer-freedom is a closed audited registry over ABI structs plus schema validation, not a claim about arbitrary unregistered structs. Wine runtime malformed/allocation injection remains open. |
| Three-domain state/category contract (`R-CORE-REC-1.*`, `2.*`) | ✅ single transaction owner + typed candidate/category routing / ✅ staged Apply + conditional interval lock / ⚠️ Wine evidence | `PeRecorderState` now exclusively owns a closed `PeStateBlockTransactionState`, which contains Recording/inside-End/poison lifecycle, a monotonic non-wrapping Recording epoch, the fixed `StateBlockRecorded` candidate/constants, private occupied Apply masks, and category-qualified staged COM values. Its lifecycle API performs candidate release, End publication/fail-stop, reset recovery, staging discard, and commit transfer without independent device booleans or raw `void*` staging. `RecordingCapability` captures the Begin epoch, so End/Reset/Begin cannot revive an old writer. `StateBlockRecorded` covers keyed render/TSS/sampler/transform, texture, independent stream source (offset/stride/ref) and frequency tables, index, VS/PS/FVF/vdecl, RT/depth, viewport/scissor/material, clip planes, lights/enables, and all six constant kinds. Recording-phase setters route only to that candidate; live/pending shadows, getters, backend state, and capture journaling stay unchanged, with `MultiplyTransform` remaining the documented prior-value exception. Wrapper snapshots thread `ALL`/`VERTEXSTATE`/`PIXELSTATE` dispositions and exact key masks; Begin/End uses `Explicit`; Capture refreshes existing keys only. Apply prevalidates before backend mutation and transfers each staged retain independently; a duplicate qualified cell is rejected before retain while the same identity in distinct cells retains independently. Backend failure poisons. End backend/wrapper failure leaves Recording, discards the candidate, and poisons. The shared conditional production guard covers the whole interval and default-pool resource-count mutations observed by Reset; the common append envelope checks poison before capacity/flush work, so `Query::Issue` and every other child writer fail-stop while successful Reset remains reachable. `dxmt9-pe-stateblock-category-spec` covers owner placement, lifecycle, epoch ABA, reset/failure, typed null occupancy, first-value-preserving duplicate rejection, commit transfer, and same-identity retention multiplicity, while `dxmt9-pe-diagnostics-spec` pins the Query/common-append source route. | Stable Sikarugir r2 runs pass the earlier StateBlock fault fixture at 8/8 points on both x64 and x86; the exact per-lane results and canonical staged hashes are archived under `experiments/output/stateblock-fault-runs/summary.json`. The current epoch/callback change has canonical x64/x86 PE compile evidence but no new Wine run. Foreign-wrapper, malformed generic payload, exact internal COM instrumentation, and C++ atomic-order evidence remain open. |
| StateBlock poison/reset lifetime and model/code binding (`R-CORE-REC-5.1.3`–`5.1.4`) | ✅ total poison/ownership/epoch generated binding + bounded Wine fixture / ⚠️ broader proof | The canonical matrix has explicit `PoisonRequested` rows for Idle, Recording, EndPublication, ApplyPrepared, and Poisoned. Every row discards candidate ownership, releases staged refs, preserves capture, and enters/remains Poisoned; Terminal has no write or poison row. Generated TLA checks `PoisonOwnsNoCandidateOrRefs` and `NoStaleCapabilityWrite`; the deliberate `PoisonLeak` and phase-only `StaleCapability` configurations violate them. Native exhaustive rows exercise real cleanup from every owning phase, terminal rejection, and Begin-A/Reset/Begin-B capability ABA. Existing repeated-value and 8/8 x64+x86 fault evidence remains applicable. | This remains bounded evidence, not foreign-wrapper, malformed generic-payload, exact internal COM instrumentation, C++ object-layout, epoch-wrap reachability, or atomic-ordering proof. |
| Implicit FVF declaration transaction (`R-CORE-7.1`) | ✅ native fake backend + PE source binding / ⚠️ Wine failure injection | `createImplicitFvfDeclTransaction` owns backend then wrapper handles through cache publication and converts backend-null, wrapper-null/throw, and cache reject/throw into value failures with exact-once cleanup. `SetFVF` resolves before live shadow/pending mutation and returns the mapped HRESULT; UP overrides and StateBlock Apply preparation use the same noexcept resolver. `dxmt9-pe-fvf-transaction-spec` injects every reachable owner edge, and `dxmt9-pe-diagnostics-spec` pins resolve-before-shadow ordering. | Wine has no allocation/bridge fault injector for these cold failure edges; ordinary FVF management conformance still needs a rerun with the changed PE binaries. |
| Atomic pending-delta consumption (`R-CORE-REC-2.3`, `3.1`) | ✅ explicit consume capability for state/constant append settlement | `settleRecorderAppend` is production-used by normal draw/APPLY_STATE, all four oversized typed batches, tail APPLY_STATE, inline constant fold, and standalone constant records. Pending table mutation is private; production settlement obtains a one-reference `Consumer`, validates the complete bounded key batch, and only then erases exactly represented rows. `dxmt9-pe-shadow-native-spec` and `dxmt9-pe-producer-differential-spec` cover failed retry, canonical order, exact represented consumption, unrepresented tail, exactly-once acceptance, and mixed valid/malformed all-or-nothing rejection. The bounded PE model carries pending snapshots, durable records, and consumption witnesses as `(qualified key, value, ordinal)` tokens. | Bridge/seal/capture-journal failures remain in the distinct commit transaction row; prepare/accept remains non-reentrant under recorder ownership. |
| PE/unix bridge ABI skew fingerprint | ✅ codegen-owned dispatch/schema fingerprint | `gen_wine_bridge.py` hashes declaration-order `BridgeOpcode` ordinals, generated argument-record field order, every source POD record declaration, and layout-affecting preprocessor context (packing directives and ABI macro values) supplied by `device_c.h`/the unix schema. `dxmt9-bridge-codegen-abi-spec` proves identical schemas are deterministic and that prototype reordering, pointed-record layout mutation, packing mutation, and array-extent macro mutation change the fingerprint; generated outputs remain Meson-owned. | The source schema is the pointer-width-independent cross-target layout contract; generated native/WoW64 `Args_*`/`Args32_*` records retain separate target-local standard-layout/trivially-copyable static assertions. Do not use compiler-specific `sizeof`/`offsetof` in the shared hash; runtime mixed-binary Wine handshake evidence remains a separate acceptance layer. |
| Seal/commit/retry/discard transaction (`R-CORE-REC-3.2`–`3.5`) | ✅ composed production predicate/model/native binding / ⚠️ conditional scalar-source and runtime evidence | `d3d9_pe_recorder_settlement_table.inc` and `planRecorderSettlement` distinguish retryable unattempted CapacityPre, emitter rollback, all three accepted CapacityPost outcomes, local bridge pre-effect retry, and effect-unknown entered-bridge poison/fail-stop. `PeRecorderSettlement.tla` composes seal/bridge, capture, ordered drain/reset/warm, emitter acceptance, and CapacityPost settlement. Production scalar settlement always preflights canonical order and exact PendingDelta category/key/index/value. The default-off cold `PeScalarSemanticTokenLedger` additionally binds the exact setter-source ordinal to the builder record ordinal across retry/discard/full snapshots; exhaustive native cases cover repeated overwrite, all 1,088 slots, duplicate/order/value/source/record mismatches, and no-token behavior. | `PeRecorderScalarProjection.tla` refines the enabled observer, not a source ordinal retained by the default path. Its regex freshness audit catches structural drift but is not semantic refinement. Typed matrix/COM/constant/heterogeneous envelopes now share the production settlement predicate, but live non-scalar source ordinals/ranges, generic bridge/capture injection, and Wine observer-on evidence remain open. |

| Replay carrier materialization ledger (`R-BACK-2.84`) | bounded observation / default-on ordinary Direct lane | An admitted populated-slot Direct continuation is proven by the shared typed predicate to append only into existing final `ChunkSlot` vector/arena capacity and ready uniform lookup chains. The continuation path does not emit `ReplaySubmissionCarrierCopy` or `ReplaySubmissionCarrierMaterialization`; typed queue counters separately report attempted/admitted/rejected/committed/fallback decisions. Native coverage binds terminal append failure, malformed shape and coordinator/resource/readback exclusion, A→B→A state/completion differential, Present-tail exclusion, ordinary/oversized/Present-tail TriangleFan Legacy routing, and zero carrier-ledger activity. `DirectChunkSlotContinuation.tla` covers admission, pre-effect rollback, commit, and post-effect fail-stop/no-retry. Fresh bounded scene evidence is correctness-positive but only GT2 completed its benchmark result: GT2/HDR1/SFIV recorded 2,703/7,118/0 committed continuations respectively and zero continuation failures, chunk rejects, or GPU command-buffer errors; the HDR1 and SFIV samples ended at the supervised timeout, and SFIV's 22 attempts all failed closed on capacity. This is a bounded non-UP Draw + APPLY_STATE/constants mechanism and does not alter the ledger meaning of Legacy carrier rows. | Completed matched production cost/performance and broader ordered-control/resource/capture evidence remain open. |
| Pending Render Tape identity lifetime (`R-CORE-REC-3.3`, `3.4`, `4.3`) | ✅ typed physical-pin/logical-lease coupling + native qualification / ⚠️ promotion evidence open | `CommittedPendingChunkLease` is non-copyable and callback-scoped. The builder issues it only for a full wire identity plus matching local wrapper pointer in the committed handle prefix; active-only handles and warm pins are rejected, and rollback removes the witness. Typed drain visits committed handles, preserves alias-before-parent ordering in an alias-first pass, and leaves physical retain/release ownership in `CommandChunkBuilder`/`D3D9PePendingCommandRetainer`. Native `dxmt9-pe-chunk-record-value-spec` covers committed, active-only, active+committed, rollback, warm-pin, qualified-mismatch, duplicate-drain, and callback-result cases. | GT2 r12 and cross-build capture evidence remain open. `RenderTapeIdentitySegments.tla` still does not model this builder capability; a bounded refinement/code-binding row remains future work. |
| Kind-qualified local/wire/COM identity (`R-CORE-REC-4.*`) | ✅ TU-local validated capabilities + canonical PE compilation / ⚠️ Wine evidence | Every final-wrapper membership gate returns a typed capability carrying the exact public-interface address, raw provider pointer, wire ref, and concrete kind after owner/address/wire validation. Binding setters cache those refs for internal emission, and the convention-only trusted wire extractors are removed. Typed StateBlock policies retain/release the original interface subobject once per occupied slot while the pointer-free wire ABI remains unchanged. | Wine foreign-wrapper runtime conformance remains open. Cached nonzero generation is not a live-generation proof. |
| Public `noexcept`/C ABI failure containment (`R-CORE-REC-3.6`) | ✅ native adoption/startup + real allocator/replay-exception matrices + canonical cross compilation / ⚠️ Wine injection | Wrapper/device/factory constructors are caught at noexcept factories; StateBlock preserves its backend-handle cleanup, device construction takes the factory reference only after fallible work, and device/factory logging contains diagnostic allocation failures. `dxmt9c_device_commit_chunk` contains every escaped allocation/replay exception behind a retained-wrapper guard and conservatively poisons possibly published ledger state. `ReplayOffloadWorker::run` translates allocation and arbitrary replay throws into the existing fail-stop path, then releases the failed and queued wrapper owners exactly once without publishing replay completion. Presence-table growth failure disables only the accelerator and uses the complete linear truth source. Replay tests use the real queue allocator plus throwing replay callbacks and live wrapper reference counts; the post-adoption effect-unknown control stays explicitly synthetic. Canonical x64/x86 PE and x86_64 provider builds pass. | Real Wine bridge/allocation injection is not available, so Windows-only runtime construction and generated bridge failure behavior remain unverified here. The unchanged bridge HRESULT cannot recover an entered-call disposition; ambiguity intentionally stays fail-stop. |
| Hot/cold and disabled observer boundary (`R-CORE-REC-5.*`) | ✅ bounded compile surface and real source owners / ⚠️ Wine observer evidence | Generic StateBlock writer, live-binding, and child call-scope callbacks remain compile-time nothrow capabilities. Six independent plain contexts (StateBlock, Buffer, SurfaceTexture, Query, Presentation, ShaderDeclaration) each hold one device pointer and expose only family-consumed `noexcept` operations; concrete wrappers hold one nullable context pointer. The broad recorder facade and child-visible object-definition/state-shadow invalidation operations are removed; QueryInterface is the deliberate out-of-line key function and FlushPeRecorderForChild is private nonvirtual. The honest 3,713-line compile-surface residual has zero state implementation fragments; its 36-line increase is the common inline all-family source/accept binding, not a renamed implementation fragment. `SetStreamSource`, `Present`, and `DrawIndexedPrimitive` plus the manifest-listed helper/template set remain in-class because PE architecture otherwise changes audited codegen; `d3d9_pe_device.cpp` owns the remaining hot definitions. QueryInterface fixes vtable and RTTI ownership but does not serve as an inline-emission workaround. | `d3d9_pe_device_child.hpp` is now a thin compatibility umbrella and ownership includes are narrowed. Current-toolchain PE codegen and broader Wine/wild observer evidence remain open. |

Strict matched true-builtin x64 and x86 artifact audits report all three ABI
owners (`QueryInterface`, the vtable, and RTTI) from
`d3d9_pe_device.cpp.obj`; they also prove the exact export ordinal/name table
and zero-delta normalized hot metrics against the corrected baseline dirs
`/Users/dididi/workspaces/dxmt9/build-win32-x64-builtin` and
`/Users/dididi/workspaces/dxmt9/build-win32-x86-builtin`. The native lane keeps
PE artifacts optional by running source-only mode.
| Shared predicate/model-code binding (`R-CORE-REC-6.*`, `R-VERIF-6.7`) | ✅ generated local + composed + repeated-value + all-family semantic binding / ⚠️ runtime promotion | Existing transition/commit/lifecycle tables plus the recorder-settlement, StateBlock-value, and all-family semantic producer tables are production-used C++ sources. Generators emit separate atomic TLA artifacts and are freshness-checked before TLC; native tests exhaust all 11 settlement and seven value rows, the production-used `planRecorderStateWrite` and `planCommittedPeSemanticProjection` relations, all 21 producer rows, exact-field counterexamples, full capture dispositions, CapacityPost rows, drain/reset/warm guards, and repeated StateBlock values. Same-identity per-slot fake-COM tests own retain/release/transfer multiplicity only. | Current-toolchain x64/x86 PE compilation, generic Wine bridge/reserve/retain/capture injection, and wild evidence remain open; no Metal, pixels, performance, or default-path source-ordinal claim is made. |

## Current bounded implementation note

All production APPLY_STATE and draw forms now use the same value-owned path.
`buildSparseState` prepares recorder-owned `PeSparseScratch` /
`SparseStateInput`; `appendOwnedRecord` synchronously validates it, copies its
typed values into bounded semantic arenas, acquires qualified pins, and settles
the exact represented `PendingDelta`. No runtime branch can construct
`SparseStatePlan`, select `CommandChunkBuilder`, or restore a compatibility
layout. The semantic owner emits either contiguous ExactFixed or negotiated
segmented roles and is also the authority for buffer hazards and Render Tape
pending leases.

`dxmt9-pe-producer-differential-spec` runs the compatibility producer and plan
producer over the fixed and randomized all-category corpus and requires exact
sealed bytes, section order, handles, payload size, and retention counts. It
also pins pre-finalization rejection, builder failure/retry, source-witness
rejection, and exact accepted consumption. `dxmt9-chunk-record-allocation-spec`
pins zero system allocations across repeated warm plan prepare/materialize
cycles. Destination-chunk stream/index cases and full snapshots remain in the
same differential corpus.

The residual is evidence, not a second implementation: the plan/builder corpus
remains for differential and Bootstrap verification, while production has one
owner graph. Wine all-family fault injection, supervised wild correctness and
locality runs, and a fresh copy/materialization ledger remain open.

The transition pass closes mutable `LiveShadow`/`PendingDelta` scalar members
behind allocation-free `PeHotStateShadow::Transition` operations, gives ordinary reads
const snapshot access, and issues a private `RecordingCapability` only while
the transaction is in Recording. The capability carries a monotonic Begin
epoch, and every generic writer/binding/call-scope body is constrained as
nothrow-invocable. Candidate/staged mutation and Apply take operations reject
invalid phases; duplicate qualified staging cells reject before retain and
preserve their first value. The native category spec pins those negative paths,
the Begin-A/Reset/Begin-B ABA trace, and the A-B-C retry/multiplicity seams. StateBlock COM identity tokens
are constructible in production only through validated kind-specific factories;
the explicit test-only fake factory is enabled only for the category spec.

The former public raw/wire extractor surface is closed: source search has no
`D3D9PeRaw*`, `D3D9PeWire*`, or trusted handle-reference declarations.
Adjacent wrapper translation units issue validated kind-qualified capabilities,
while Process Vertices retains the typed outputs from its initial validation pass
and passes validated declaration handles into layout parsing, avoiding repeated
validation in per-draw packet construction. Native focused specs plus x64/x86
PE compilation provide the current source/compile evidence; the bounded
StateBlock fault seam has stable 8/8 x64 and 8/8 x86 Wine results archived in
`experiments/output/stateblock-fault-runs/summary.json`. Foreign-wrapper,
malformed generic-payload, exact internal COM-instrumentation, and C++
layout/atomic evidence remain open as tracked above.

The bounded StateBlock fault fixture is env-dispatched from the conformance
auxiliary executable: pre-effect points must return the configured HRESULT and
retry, while entered points are injected only after a successful backend call,
release staged/returned references, and require DEVICELOST until a successful
Reset. Entered-call disposition is effect-unknown across the unchanged C ABI;
the fixture therefore records only the injected HRESULT, poison, and explicit
reset/discard recovery, and never claims entered-call recoverability. The runner
stages detected x86 PE DLLs under `i386-windows` and supports `--aux-exe`/`--start
0 --end 0` for short point-specific runs.

All four public SWVP draw entries now share one candidate-preparation phase:
bound and UP transforms, instance expansion, and clip/filter work run before
append, with `bad_alloc`/`length_error` translated to `E_OUTOFMEMORY`. The phase
restores temporary UP stream bindings and instancing offsets on every exit, and
local noexcept guards release every temporary vertex/index buffer and unlock
window on exceptional exits. Failed preparation therefore leaves live/pending
state and caller-owned UP bytes untouched; `dxmt9-pe-public-allocation-spec`
covers the indexed/non-indexed, bound/UP transform, expansion, and filter
failure matrix and source-binds all four public routes to the shared phase.
Native and canonical PE compilation are evidence; Wine allocator injection and
runtime retry evidence remain open.

StateBlock recorded-domain mutation now uses a phase-checked callback capability
(`withRecordingWriter`); no production writer arrow
access remains, and constant writes select the six recorded members only inside
the callback after Recording validation. The native category spec exercises all
six constant kinds across Idle, Recording, stale-after-transition, poisoned,
and successful-reset states; broader Wine state-block runtime evidence remains
the existing open item.

## Implementation order

Follow [spec.md §8](spec.md#8-sequential-implementation-dag): establish failure
injection first; clean observers/cache; repair append/commit settlement; then
decompose state/ownership, qualify identity, bind shared predicates, add formal
evidence, and finish with PE/Wine acceptance. These tracks are sequential
because they overlap the recorder hot files and because later proofs depend on
the earlier failure semantics.
