---
type: "Spec"
title: "Render Provider Policy Spec"
description: "Typed policy registry for renderer, scheduling, encoding, submission, and presentation lanes."
tags: [specs, backend, render-provider, policy, spec]
---

# Render Provider Policy Spec

## 1. Ownership

Provider configuration is a composition of domain-owned typed values, not one
flat bag of booleans:

```text
RenderProviderConfig {
  RendererPolicy renderer
  ProducerReplayPolicy producerReplay
  RenderSchedulingProviderConfig scheduling
  SubmissionPolicy submission
  BindingPolicy binding
  FfpExecutionPolicy ffp
  PresentPolicy present
}
```

Each owner resolves its value once at the narrowest valid lifetime. Backend,
producer replay, and renderer feature selection are process/device scoped;
render scheduling and submission policy are queue scoped; binding and FFP
selection may fall back per pass after queue-cached capability resolution;
Presenter owns acquire and boundary policy. Hot draw code consumes resolved
values and never reparses the process environment.

## 2. Current Provider Registry

`StableProvider` means the mode name and rollback contract are durable. It does
not mean the mode is the current default or that implementation/evidence is
complete.

| Axis | Modes | Policy / implementation | Current activation | Owner |
|---|---|---|---|---|
| Renderer backend | `traditional`, `framegraph` | Stable / implemented | `framegraph` default; `traditional` fallback | `R-BACK-31.1` |
| Compatibility profile | `strict`, `progressive`, `aggressive` | Stable; strict/progressive implemented, aggressive planned | `progressive` default | `R-BACK-40.*` |
| Semantic features | `passcoalesce`, `dce`, `memoryless`, `objectschedule`, `mesh`, `gpudriven`, `bindless` | Stable names; passcoalesce/dce implemented, remaining modes planned | passcoalesce default; dce opt-in; remaining unavailable | `R-BACK-31.3`–`31.4` |
| Renderer fallback | `allow`, `strict` | Stable / planned | allow specified default, selector not implemented | `R-BACK-31.5`, `R-BACK-31.8` |
| Producer replay | `inline`, `offloaded` | Stable / implemented | offloaded default; inline rollback | `R-BACK-2.51` |
| Opaque-depth index locality | original order, conservative LRU32 reorder | Stable / implemented | follows offloaded replay by the explicit `R-BACK-2.51` coupling; opt-out reachable | `R-BACK-2.51` |
| Source delivery | `compatibility`, `streaming` | Stable; compatibility implemented, streaming partial | compatibility default; streaming available through migration alias | `R-BACK-2.66` |
| Partition execution | `identity`, `serial`, `parallel` | Stable; identity implemented, explicit serial partial, parallel planned | identity default | `R-BACK-2.66` |
| CB segmentation | `off`, `metal4` | Stable; off implemented, Metal 4 planned | off default | `R-BACK-2.66` |
| Submission grain | `off`, `per-render-pass` | Stable / implemented | per-render-pass default with cap 4; off rollback | `R-BACK-2.29`–`2.34` |
| Binding representation | Stage 1 direct, Stage 2 constants, Stage 2b direct-cbuf, Stage 2 resource-array | Stable / implemented with capability fallback | Stage 2b automatic on Apple3+/Tier2; Stage 1 fallback; resource-array opt-in | `R-BACK-12.22`–`12.26` |
| FFP execution | `portable`, `tile-auto` | portable stable / implemented; tile-auto candidate / correctness-blocked | tile-auto resolves to portable | `R-BACK-13.*` |
| Present acquire | `sync`, `pre-acquire`, `sync-on-submit`, `async` | Stable / implemented | sync default | `R-BACK-6.11`, Presenter |
| Present boundary | `default`, `after-acquire`, `completion`, `present-completion` | Stable / implemented | present-completion default | `R-BACK-6.12`, Presenter |
| Drawable storage | general drawable, framebuffer-only drawable | Stable / implemented | general default; framebuffer-only opt-in | `R-CORE-WSI-6.1`–`6.2` |

The existing `DXMT9_CPU_READY_TAPE` spelling is only the migration alias for
the stable `streaming` source-delivery mode. The canonical scheduling selectors
and resolver remain implementation gaps under `R-BACK-2.66`.

### 2.1 Experimental candidates

These lanes are implemented enough to run but are not compatibility promises:

| Candidate | Stable fallback | Promotion requirement |
|---|---|---|
| `per-n-records` mid-chunk commit | `off` or `per-render-pass` | production shape and locality evidence |
| deferred present-completion boundary | present-completion | pass-streaming, ordered completion, visual, latency, and locality gates |
| PE inline-constant delta | standalone constant records | renewed workload evidence above the noise gate |
| unpublished-slot PSO prefetch | encode-slot prefetch | no-gputrace producer/encode overlap and no new queue stall |
| native Metal base-vertex submission | index/offset adjustment | draw/readback equivalence plus target-workload CPU/GPU benefit |
| opaque-depth extended scope | conservative opaque-depth predicate | image and Xcode invocation proof across target workloads |
| compatible indexed-draw merge | unmerged draws | visual, query-attribution, and cache-benefit evidence |
| screen-blend index cache | original primitive order | explicit tolerance policy and semantic image proof |
| large indexed-draw split | one draw | workload benefit with command-count and locality conservation |
| StretchRect chunk split | unsplit ordered chunk | surface-op ordering, submission-locality, and workload evidence |
| aggressive color/depth DontCare | conservative Store proof | cross-frame resource-liveness and device-backed equality |
| CAMetalLayer display-sync opt-in | minimum-duration scheduling | interval correctness, compositor behavior, and latency evidence |
| unused-varying trim | full VSOut | new evidence that justifies reopening the rejected GT1 result |
| tile-auto FFP | portable FFP | partial/overlap/multi-draw equality, coverage/prior-colour implementation, then workload benefit |

`DXMT9_ARGBUF_RESOURCE_ARRAY` is a stable opt-in provider mode. The
`DXMT9_TILE_FFP=auto` spelling is retained but currently resolves to portable;
it is a correctness-blocked candidate, not a usable opt-in provider.
`DXMT9_TILE_FFP=force` is diagnostic only.

### 2.2 Diagnostic and retired surfaces

Correctness-invalid state overrides, draw filters, geometry mutation probes,
attachment-view suppression, capture/dump controls, measurement-only counters,
and `DXMT9_*PROBE*` selectors are `DiagnosticProbe`. They are deliberately
excluded from the provider matrix.

Selectors listed in a rules file's retired section are `Retired`; their source
resolver and runtime lane are absent. Historical experiment documents may keep
their names, but launchers and new specs must not treat them as available.

Operational cache controls (`DXMT9_PREWARM`, archive size, cache directory, and
PSO compile-thread count) are supported runtime policies but not render-provider
axes because they must not alter the Metal command stream. Automatic resource
allocation choices such as `MTLHeap` pooling are capability implementation
details unless a future requirement exposes a selectable provider mode.

## 3. Composition Rules

- Semantic features operate above source delivery and partition execution.
  `dce` or `passcoalesce` must not be implied by streaming or parallelism.
- Producer offload is independent of source delivery. An unsupported
  combination must resolve explicitly rather than silently changing both axes.
- The existing offloaded-replay to conservative opaque-depth index-locality
  default is an explicit `R-BACK-2.51` coupling, not a general permission for
  provider selectors to enable unrelated optimizers.
- Submission grain and Metal 4 segmentation are distinct: sub-CB chaining ends
  a physical command buffer, while Metal 4 preserves a validated logical pass
  across jointly committed physical segments.
- Binding and tile-FFP selection may fall back per pass without changing source,
  partition, completion, or Present policy.
- Present policy controls drawable acquisition and frame-latency backpressure;
  it must not change render-command meaning or source completion ownership.

## 4. Resolution and Observability

Every stable axis records the requested value, resolved value, resolution scope,
capability/default/alias fallback reason, and mode-specific work counts. Startup
logging is bounded to one record per resolved owner. Perf snapshots use typed
mode identifiers or bounded counters rather than copying environment strings
through the hot path.

An invalid stable selector resolves to its documented fallback and warns once.
An unavailable planned mode must not half-enable; backend construction or the
owning pass falls back atomically according to its domain contract.

## 5. Verification Mapping

| Contract | Current evidence / gap |
|---|---|
| Renderer backend/profile/features | backend factory and FrameGraph native specs; aggressive and planned features remain unavailable |
| Producer replay | offload and byte-identity native variants; provider-style canonical resolver naming remains optional cleanup |
| Scheduling provider | `R-BACK-2.66` contract and serial/session specs; canonical resolver and full mode matrix missing |
| Submission grain | mid-chunk policy and TLA evidence; `per-n-records` production evidence missing |
| Binding representation | argbuf selector/MSL/populator and shader-runner readback evidence; resource-array performance gate missing |
| FFP execution | tile selector/MSL/readback equality; workload promotion evidence missing |
| Present policy | acquire and boundary pure resolver matrix specs; canonical single-selector migration and deferred promotion evidence missing |
| Classification audit | missing automated audit that every mutating runtime selector is registered or explicitly diagnostic/retired |
