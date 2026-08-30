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
| PE final wire blob (`R-CORE-REC-7.1`–`7.2.1`) | The pure, pointer-width-independent `planExactCommandChunkLayout` and exact `CommandChunkBuilder` mode allocate the canonical D9C V2 header/table/table/arena layout once. Exact mode writes final record and handle tables plus arena directly, keeps legacy wire record/handle/payload vectors empty, stores arena-relative record offsets, and uses monotone used-prefix counts with `handleObjects_`/final-table reads for dedup and rollback. The real `D3D9DeviceImpl::Present` boundary now owns a `PePresentBatch` value and feeds that value into the unchanged legacy `appendRecord` path, preserving its ordering, cadence, retain, and error behavior. Separately, `PePresentBatchTransaction` is a production-shaped/native-proven exact two-pass owner: it computes counts without retain/pending/capture effects, then replays the owned value into exact final storage; native coverage includes pre-emit no-retain, exact byte identity, retain multiplicity, repeated seal, Reset/retry, and discard. The exact transaction has no production callsite and remains unselected. | A complete whole-chunk owner is still missing for the remaining producer families: sparse draw/APPLY_STATE, six constant records, Clear, StretchRect, ColorFill, UpdateTexture, UpdateSurface, QueryIssue, Readback, RESZ depth resolve, GenerateMipmaps, and UP draw records. Until those owners can compose with existing CapacityPre/CapacityPost cadence and bridge/capture settlement, the exact Present transaction cannot be promoted as the normal route. All-family differential/fallible rollback, x64/x86 Wine fault evidence, and wild evidence remain open. **Item 6 performance promotion remains open.** |
| Typed recorder borrow/lock capability (`R-CORE-REC-7.3`) | `RecorderLockCapability` and `RecorderBorrow<T>` are epoch-qualified, non-copyable, and non-movable. `SparseStatePlan` privately owns those borrows and is itself non-copyable/non-movable; every production compact-plan build now requires the capability issued by the surrounding `PeRecorderGuard`. Emit, chunk-context, and settlement paths expose only their minimum nothrow visitor (`shadow + bindings`, `bindings`, or `shadow + constants + bindings`) and return no raw source pointer. The one `planRecorderAccess` predicate admits either the no-MT owner lane or an actually-held conditional-lock lane, and Reset/poison/teardown advance the existing transaction epoch without enlarging `PeRecorderState` or `D3D9DeviceImpl`. Native exhaustive owner/lock rows and stale-epoch negatives use the production types; canonical x64/x86 PE and x86_64 provider builds pass. | Wine Reset fault evidence remains required. C++ cannot reject a deliberately retained raw reference inside an arbitrary callback body, so source audit remains the enforcement for malicious callback bodies. |
| Heterogeneous semantic projection (`R-CORE-REC-7.4`–`7.5`) | The production semantic header now has distinct exact matrix-bit, constant byte/range, kind-qualified COM identity, and heterogeneous record category/key/value envelopes. All five families (including scalar) share `planPeSemanticProjection`: pre-effect rejection preserves retry state, while an accepted record with a missing ordinal/range or mismatched exact identity fails stop. The existing scalar ledger consumes through this predicate, and native exhaustive rows prove that same-size matrices, constants, COM records, and heterogeneous records cannot substitute for semantic identity. The StateBlock injector adds a behavior-neutral one-shot `bridge_pre` before the real Capture/Apply/End PE→unix entry; the Capture retry fixture passes with exact injected HRESULT on current canonical x64 and x86 Sikarugir lanes. | Live source ordinals and exact committed byte ranges are still retained only for the scalar observer; wiring the new envelopes into every setter/control producer remains open. Generic chunk reserve/retain/capture-throw injection and a generated formal heterogeneous-token model remain impossible to claim from the StateBlock-only hook; they require explicit runtime injection and matching Wine fixtures. |

Dependencies are ordered: heterogeneous tokens and fault seams precede the
full legacy/direct oracle; the exact final-layout primitive precedes a
replayable producer transaction; typed capabilities precede any asynchronous
code review claim; formal/native evidence precedes PE/Wine evidence, which
precedes wild counter and performance evidence. Conservative unused handle
capacity cannot shift the payload offset because exact wire bytes are required,
and a post-build compaction copy would recreate the class this workstream
removes. The internal primitive changes neither the production builder route,
wire ABI, recorder cadence, nor D3D9 semantics.

## Stable PE decomposition audit

`scripts/check/audit_d3d9_pe_abi_codegen.py --source-only` and
`scripts/check/pe_device_abi_manifest.toml` are the source-level acceptance
surface for R-CORE-REC-5.2.2/5.2.3. They pin the QueryInterface key-function
owner, `PeRecorderState` x64/x86 sizes, critical device member order, exact
cold symbol owners, and the export allowlist while deliberately excluding
addresses, RVAs, timestamps, archive/relocation order, whole-object hashes,
and aggregate text size. Native Meson invokes source-only mode, so PE artifacts
are not a native-test prerequisite.

The physical residual is now honestly bounded at 3,677 lines for
`d3d9_pe_device_impl.hpp`; this is compile surface, not a cosmetic line-count
claim. Exact x64/x86 codegen requires the legal, reachable in-class bodies of
`SetStreamSource`, `Present`, and `DrawIndexedPrimitive`, plus the manifest-listed
hot helper/template set (`PeCallScope`, hot-setter/call wrappers, constant and
draw cores, append timers, recorded-ref helpers, and their small predicates).
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
| Pending Render Tape identity lifetime (`R-CORE-REC-3.3`, `3.4`, `4.3`) | ✅ typed physical-pin/logical-lease coupling + native qualification / ⚠️ promotion evidence open | `CommittedPendingChunkLease` is non-copyable and callback-scoped. The builder issues it only for a full wire identity plus matching local wrapper pointer in the committed handle prefix; active-only handles and warm pins are rejected, and rollback removes the witness. Typed drain visits committed handles, preserves alias-before-parent ordering in an alias-first pass, and leaves physical retain/release ownership in `CommandChunkBuilder`/`D3D9PePendingCommandRetainer`. Native `dxmt9-pe-chunk-record-value-spec` covers committed, active-only, active+committed, rollback, warm-pin, qualified-mismatch, duplicate-drain, and callback-result cases. | GT2 r12 and cross-build capture evidence remain open. `RenderTapeIdentitySegments.tla` still does not model this builder capability; a bounded refinement/code-binding row remains future work. |
| Kind-qualified local/wire/COM identity (`R-CORE-REC-4.*`) | ✅ TU-local validated capabilities + canonical PE compilation / ⚠️ Wine evidence | Every final-wrapper membership gate returns a typed capability carrying the exact public-interface address, raw provider pointer, wire ref, and concrete kind after owner/address/wire validation. Binding setters cache those refs for internal emission, and the convention-only trusted wire extractors are removed. Typed StateBlock policies retain/release the original interface subobject once per occupied slot while the pointer-free wire ABI remains unchanged. | Wine foreign-wrapper runtime conformance remains open. Cached nonzero generation is not a live-generation proof. |
| Public `noexcept`/C ABI failure containment (`R-CORE-REC-3.6`) | ✅ native adoption/startup + real allocator/replay-exception matrices + canonical cross compilation / ⚠️ Wine injection | Wrapper/device/factory constructors are caught at noexcept factories; StateBlock preserves its backend-handle cleanup, device construction takes the factory reference only after fallible work, and device/factory logging contains diagnostic allocation failures. `dxmt9c_device_commit_chunk` contains every escaped allocation/replay exception behind a retained-wrapper guard and conservatively poisons possibly published ledger state. `ReplayOffloadWorker::run` translates allocation and arbitrary replay throws into the existing fail-stop path, then releases the failed and queued wrapper owners exactly once without publishing replay completion. Presence-table growth failure disables only the accelerator and uses the complete linear truth source. Replay tests use the real queue allocator plus throwing replay callbacks and live wrapper reference counts; the post-adoption effect-unknown control stays explicitly synthetic. Canonical x64/x86 PE and x86_64 provider builds pass. | Real Wine bridge/allocation injection is not available, so Windows-only runtime construction and generated bridge failure behavior remain unverified here. The unchanged bridge HRESULT cannot recover an entered-call disposition; ambiguity intentionally stays fail-stop. |
| Hot/cold and disabled observer boundary (`R-CORE-REC-5.*`) | ✅ bounded compile surface and real source owners / ⚠️ Wine observer evidence | Generic StateBlock writer, live-binding, and child call-scope callbacks remain compile-time nothrow capabilities. Six independent plain contexts (StateBlock, Buffer, SurfaceTexture, Query, Presentation, ShaderDeclaration) each hold one device pointer and expose only family-consumed `noexcept` operations; concrete wrappers hold one nullable context pointer. The broad recorder facade and child-visible object-definition/state-shadow invalidation operations are removed; QueryInterface is the deliberate out-of-line key function and FlushPeRecorderForChild is private nonvirtual. The honest 3,677-line compile-surface residual has zero state implementation fragments. `SetStreamSource`, `Present`, and `DrawIndexedPrimitive` plus the manifest-listed helper/template set remain in-class because either PE architecture otherwise changes audited codegen; `d3d9_pe_device.cpp` owns the remaining hot definitions. QueryInterface fixes vtable and RTTI ownership but does not serve as an inline-emission workaround. | `d3d9_pe_device_child.hpp` is now a thin compatibility umbrella and ownership includes are narrowed. Broader Wine/wild observer evidence remains open. |

Strict matched true-builtin x64 and x86 artifact audits report all three ABI
owners (`QueryInterface`, the vtable, and RTTI) from
`d3d9_pe_device.cpp.obj`; they also prove the exact export ordinal/name table
and zero-delta normalized hot metrics against the corrected baseline dirs
`/Users/dididi/workspaces/dxmt9/build-win32-x64-builtin` and
`/Users/dididi/workspaces/dxmt9/build-win32-x86-builtin`. The native lane keeps
PE artifacts optional by running source-only mode.
| Shared predicate/model-code binding (`R-CORE-REC-6.*`, `R-VERIF-6.7`) | ✅ generated local + composed + repeated-value binding / ⚠️ semantic cross-projection + runtime promotion | Existing transition/commit/lifecycle tables plus the recorder-settlement and StateBlock-value tables are production-used C++ sources. Generators emit separate atomic TLA artifacts and are freshness-checked before TLC; native tests exhaust all 11 settlement and seven value rows, the production-used `planRecorderStateWrite` semantic projection, full capture dispositions, CapacityPost rows, drain/reset/warm guards, and the same production `planPeStateBlockValue` Capture/Apply transitions against live mutation and failure preservation. Same-identity per-slot fake-COM tests own retain/release/transfer multiplicity only. Canonical x64/x86 PE and x86_64 provider builds pass without a bridge ABI change. | Exact semantic-token binding across the heterogeneous generic append envelope remains structurally unavailable and open; no record-type/byte-size surrogate is used. Wine bridge/capture injection and wild evidence remain open; no Metal or performance-policy change is claimed. |

## Current bounded implementation note

Ordinary APPLY_STATE and all non-override draw forms now use a real compact
two-pass `SparseStatePlan`. Pass 1 stores only category counts, binding/scalar
masks, constant ranges, UP spans, draw semantics, and borrowed source witnesses
under the recorder lock. It does not contain `PeSparseScratch`,
`SparseStateInput`, or any section-sized wire array; it neither retains handles
nor consumes `PendingDelta`. After CapacityPre, draw emitters finalize the
destination chunk's stream/index selection. `appendVisitedSectionPayload`
then writes every scalar and binding row directly into the final builder
payload, with handle retains and rollback owned by the existing active-record
checkpoint. `acceptSparseStatePlan` revalidates the source bindings, selected
counts/masks, scalar values, optional semantic tokens, and constant ranges
before exact consumption.

`dxmt9-pe-producer-differential-spec` runs the compatibility producer and plan
producer over the fixed and randomized all-category corpus and requires exact
sealed bytes, section order, handles, payload size, and retention counts. It
also pins pre-finalization rejection, builder failure/retry, source-witness
rejection, and exact accepted consumption. `dxmt9-chunk-record-allocation-spec`
pins zero system allocations across repeated warm plan prepare/materialize
cycles. Destination-chunk stream/index cases and full snapshots remain in the
same differential corpus.

The residual is explicit: `PeSparseScratch`/`SparseStateInput` remain an
always-owned compatibility projection for four oversized scalar batches,
Render Tape full-snapshot checkpoint construction, and SWVP override packets
whose temporary FVF/shader bindings are restored before append. Ordinary UP
draws without those overrides use the plan. Removing the residual storage
requires a separate lifetime change for Render Tape/SWVP; it is not claimed by
this increment. Wine runtime fault injection and promotion evidence remain
open.

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
