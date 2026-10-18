---
type: "Spec Gap"
title: "Detailed Performance Tracing Gaps"
description: "Existing observation primitives and missing integrated trace qualification."
tags: [specs, experiments, harness, tracing, gap]
---

# Detailed Performance Tracing Gaps

Owns implementation/evidence status for [requirements](requirements.md) and
[spec](spec.md). This inventory distinguishes existing components from the
integrated contract; source presence is not a fresh runtime qualification.

The [requirement evidence matrix](evidence.md) is the authoritative
code/test mapping. Implementation and platform qualification remain distinct.
Native SDK import, deterministic fixtures, and device smoke do not by themselves
qualify an unavailable external collector or establish negligible overhead.

| Contract | Implementation | Qualification and remaining limits |
|---|---|---|
| R-HARN-TRACE-17.1–17.6 | Implemented: independent CPU/wait consumers, binary rows/endpoints, raw PE samples and population summaries, log-independent reports, direct structural SQL and selection provenance. | SDK-on/off and six deferred/async × dual/trace-only/unwritable-path native/SQL captures pass. PE sampling retains source QPC without invented native alignment; late/final reports outside admission remain legacy evidence. See [independent collector evidence](evidence.md#independent-collectors-and-batch-transport). |
| R-HARN-TRACE-16.1–16.6 | Implemented: bounded producer pairs and whole-report slots, optional async spool worker, typed full-report batches, generation-safe shutdown and durable completion proof. Deferred remains default; native Perfetto export occurs after close in both modes. | Ownership/exhaustion/deadline fixtures, TLA+ and ThreadSanitizer pass; raw/native/SQL parity passes both modes. Matched live Wine/Metal overhead qualification remains open. The evidence evaluator requires supplied measurements; it does not acquire missing metrics. See [evidence](evidence.md#independent-collectors-and-batch-transport). |
| R-HARN-TRACE-15.1–15.6 | Implemented: shared acquired reports, exact process counters/percentile populations, memory samples, qualified CPU/wait endpoints, generated PE report transport, queue/signpost and renderer artifact adapters. | Native/SQLite fixtures cover all families; see [shared diagnostic evidence](evidence.md#shared-diagnostic-consumers). Live Wine overhead, external captures and capture-window coverage remain separately qualified. |
| R-HARN-TRACE-14.1–14.5 | Implemented: bounded atomic query-ledger/frame/append snapshot projections with source, population and aggregation metadata. | SDK-off 25/25 and SDK-on 28/28; pinned native/SQLite round trip covers 204 snapshot fields. See [evidence](evidence.md#shared-query-and-counter-snapshots). Live Wine overhead remains unqualified; process fields outside CounterSnapshot are covered by R-HARN-TRACE-15. |
| R-HARN-TRACE-13.1–13.5 | Shared object-owned GPU endpoint sources, retained legacy stage attachments, common bounded CPU/wait records, native counter projections and SQL source keys. | SDK-off/on and actual Metal fixture evidence is recorded in [shared acquisition evidence](evidence.md#shared-acquisition). Unbracketed historical legacy clocks remain unqualified for direct placement; R-HARN-TRACE-15 qualifies new endpoint brackets. Fresh Wine and observer-overhead qualification remain open. |
| R-HARN-TRACE-12.1–12.6 | Implemented: default overview, native queue/frame counter tracks, slice-to-SQL source links, supplemental CPU/wait/PE adapters and source-line log indexing, immutable SQLite, experiment and conformance provenance. | Native counter/SQLite round trip and deterministic failure fixtures pass. Full replacement of legacy producers and live migration overhead remain unqualified; PE QPC limits above still apply. |
| R-HARN-TRACE-1.1–1.3 | Catalogue profiles, unique attempts, frozen requests, build/source provenance, separate collector outcomes and atomic manifest sealing. | Python positive/negative fixtures pass; final catalogue Wine evidence is recorded in the matrix. |
| R-HARN-TRACE-2.1–2.3 | Process/thread/session incarnations, distinct raw/queue/CB identities and explicit cold-side relation resolution. | Native and Wine x64 GPU captures qualify typed submit-to-CB flows. Broad many-source/shared-CB fan-out remains unqualified; completion membership does not establish exclusive physical GPU contribution. |
| R-HARN-TRACE-3.1–3.4 | Declared clocks, bounded calibration validity, source metrics, unknown populations and legacy adapters. | Native and Wine x64 GPU calibration passes. Wine x86/x64 PE point brackets now pass the unchanged 1000 ns policy on three captures per architecture; unknown populations remain unknown. See [qualification](pe-clock.md#qualification). |
| R-HARN-TRACE-4.1–4.3 | Cached gates, bounded POD buffers, generation-qualified admission and generated bounded PE transport. | Disabled ARM64 assembly, matched native emitter controls, and x86/x64 PE builds pass. ABI v2 independently calibrates both endpoints. Wine x86/x64 each pass three six-endpoint captures with zero loss and exact native consumer checks. |
| R-HARN-TRACE-5.1–5.3 | Immutable raw/derived artifact graph, native protobuf, pinned Trace Processor and fail-closed normalization. | Native and Wine x64 CPU/GPU captures pass pinned TP. New Wine x86/x64 PE point traces pass complete-attribution checks; historical failed traces remain rejected and zero loss alone is insufficient. |
| R-HARN-TRACE-6.1–6.4 | REA/module/address/ABI/installation validation and owned portable probe lifecycle. | No live target patching adapter is admitted. Static validity never claims activation, hits or state preservation. |
| R-HARN-TRACE-7.3 | Separate optional emulation profile with explicit capability rejection. | QEMU is not installed or qualified as a macOS/Wine performance equivalent. |
| R-HARN-TRACE-8.1–8.3 | Deterministic counterexamples, CPU/GPU lifecycle models, matched OFF/ON controls and frozen budgets. | No speedup or negligible-overhead claim. See exact run scope and limitations in the evidence matrix. |
| R-HARN-TRACE-9.1–9.3 | Private pinned opt-in unix SDK, SDK-free observer facade and deferred original-time native serialization. | Native SDK-off/on and x86_64 unix builds exercised; PE builds reject SDK linkage; public SDK symbols audited. |
| R-HARN-TRACE-9.4, 9.7 | Session generation/admission/drain/seal, atomic receipts, frozen limits and bounded failure cleanup. | Production lifecycle fixtures plus safety/progress and deliberate TLA counterexamples. |
| R-HARN-TRACE-9.5–9.6 | Import/replay/publication/encode/wait/query/submit/completion sites, opt-in detail and batched guest observations. | Final Wine x64 GPU captures resolve engine categories and one detail event; query applicability remains unobserved. Wine x86/x64 PE point traces now qualify original endpoint clocks, native/guest identities and native TP serialization; broad guest invocation attribution is not implied. |
| R-HARN-TRACE-9.8–9.9 | Native tracks/relations, loss checks and build/runtime test matrix. | SDK-off tests pass 17/17; SDK-on tests pass 20/20; x86/x64 PE and unix builds pass. Wine x64 CPU, CB+detail and encoder-stage captures pass exact consumer qualification; x86/x64 PE point captures now qualify at 247–810 ns and 476–956 ns respectively; see [repair evidence](pe-clock.md#qualification). |
| R-HARN-TRACE-10.1–10.2 | Built-in Metal CB timestamps and optional render-stage counter samples with explicit capability/level outcomes. | Wine x64 CB+detail and encoder-stage traces qualify with requested levels attached; unsupported levels still fail explicitly. Completion callbacks remain retirement hints, not GPU timestamps. |
| R-HARN-TRACE-10.3–10.4 | Bounded generation-qualified slots, nonblocking admission, asynchronous polling, quarantine and safe retirement. | GPU model/code tests and intended mutant failures pass; native/Wine GPU captures retire with zero loss. Dedicated live timeout injection remains pending; no unbounded wait or production lifetime authority. |
| R-HARN-TRACE-10.5–10.7 | Source-specific API/tick metadata, actual device clock anchors and GPU track/CB bindings. | Apple M1 matched controls and Wine x64 stage/CB traces pass; GPU source metadata and calibration reach pinned TP. Historical PE captures remain failed cases; subsequent x86/x64 point-bracket runs qualify their own QPC mappings. |
| R-HARN-TRACE-11.1–11.3 | Platform-qualified manifests, loaded-site/ABI validation and thread/installation coverage accounting. | Live Linux perf/ptrace and macOS/Wine breakpoint adapters are unavailable and unqualified. |
| R-HARN-TRACE-11.4–11.6 | Portable wrap-safe perf parsing, loss/throttle records, sample/register semantics and attribution exclusions. | Fixtures do not establish live Linux scheduling or macOS target coverage. |
| R-HARN-TRACE-11.7–11.8 | Owned bounded attach/detach state model and fail-closed capability admission. | No live target mutation occurs without an admitted adapter. No perftrace code or mandatory dependency is included. |

## Optional cross-run analytics

Optional [cross-run DuckDB/Parquet analysis](spec.md#optional-cross-run-analytics)
has a documented [directory layout](../../../../experiments/analysis/README.md)
and a developer CLI entry in `Brewfile`. No integrated exporter or scale/parity
benchmark is provided. Adoption is conditional on a measured analytical query need;
this is not a required runtime collector or a gap in per-run SQLite publication.

## Artifact retention

R-HARN-TRACE-18.1–18.5 is specified in [artifact retention](artifact-retention.md).
Status: **partial**. `scripts/tools/retain_trace_artifacts.py` implements
inventory, frozen explicit plans, manual digest-bound lifecycle reviews,
path/digest pins, conservative reverse-reference checks, a mutation lease,
quarantine/unlink journals, compact evidence preservation and availability
receipts. `scripts/tools/retention_archive.py` supplies zstd archive publication,
bounded isolated extraction, exact file/tree verification and restore-to-new-path.
Archive/restore and evidence writes enforce the operation's free-space floor,
including simultaneous staging and allocation overhead. Focused tests are
`dxmt9-retention-archive` and `dxmt9-trace-retention`.

Eviction is limited to individual reviewed bulk files; directory bundles can be
archive-copied. Unknown/opaque dependency metadata blocks eviction. Automatic
producer lifecycle qualification, SQLite/DuckDB/Parquet dependency adapters,
archive-aware source audits/readers, integrated 10 GiB analysis quotas and
unattended interrupted-operation recovery remain unimplemented. The mutation
lease does not lock producers; explicit offline owner closure is required.
Existing consumers require materialized source paths, so cited evidence remains
materialized. No automatic expiry or cleanup timer is enabled. See the
[operational workflow](../../../../experiments/analysis/retention.md).

## External Collection

External collection gaps are owned by the
[address/register probe contract](external-probes.md):

| Contract | Status | Current evidence | Missing implementation or evidence |
|---|---|---|---|
| R-HARN-TRACE-11.1–11.3: platforms and sites | partial | REA provenance rules and the PE sampler provide discovery inputs; the Linux reference project is identified. | Admitted platform adapters, loaded-address/ABI validation, breakpoint capacity and thread-lifecycle coverage. |
| R-HARN-TRACE-11.4–11.6: transport and meaning | not started | Inspected perftrace blobs are reference material, not a qualified collector. | Wrap-safe/loss-aware transport, register/sample normalization, identity/clock joins, invocation checks and observer controls. |
| R-HARN-TRACE-11.7–11.8: lifecycle/admission | not started | [External probe verification](external-probes.md#verification-and-admission) defines acceptance. | Attach/detach model/code evidence, actual Linux/macOS capability fixtures, preserved target state and pinned Perfetto consumer checks. |

## Existing Component Entry Points

R-HARN-TRACE-15 adds the remaining producer consumers. CPU endpoints use native
brackets and qualified wait pairs become slices; failed brackets stay explicit.
PE sampler/recorder reports use generated bounded transport, and process perf
fields/histogram outputs share acquired values. Queue/signpost and renderer
outputs have native/SQL adapters. These implementations do not establish that a
particular live trace window captured a collector's periodic or shutdown report.
External Instruments/Processor Trace/Xcode capture retain separate acquisition
and join only through qualified identity, clock and artifact provenance.

- `src/dxmt9/dxmt9_cpu_supply_trace.{hpp,cpp}` and
  `scripts/tools/analyze_cpu_supply.py` (`load`, `pair`).
- `src/dxmt9/dxmt9_wait_trace.{hpp,cpp}` and
  `scripts/tools/summarize_wait_trace.py` (`load`, `summarize`).
- `scripts/tools/join_replay_wait_trace.py` (`join`).
- `src/d3d9/d3d9_pe_thread_sampler.hpp`,
  `scripts/tools/symbolicate_xctrace_pe.py`, and
  `scripts/tools/summarize_guest_pc_window.py` (`summarize`).
- `src/dxmt9/dxmt9_signposts.{hpp,cpp}` and
  `scripts/tools/summarize_xctrace_metal_intervals.py`.
- `tests/scripts/test_cpu_supply_trace.py`,
  `tests/scripts/test_wait_trace.py`, and
  `tests/scripts/test_summarize_xctrace_metal_intervals.py`.
