---
domain: present-pacing
workload: STALKER H502/r5 original capture and native foreign-callback fixtures
title: "Request-qualified main dispatch and shared-event observer preflight"
type: evidence
status: correctness-preflight-only
updated: 2026-10-16
source: traces/stkcop-foreign-owner-h579/{contract.md,observer.hpp,generated_sink.hpp,fixture.cpp,analyze.py,build.py,build.json,preflight.py,preflight.json,capture_preflight.py,capture-verification.json,api_fixture.mm,api_preflight.py,api-verification.json,baseline-syntax.log}; traces/stkcop-foreign-owner-h579/{capture-off,capture-on,api-off,api-on}.log
related: present-pacing-default-owner.571.md; present-pacing-clear-default-route.578.md; present-pacing-current-clear-live.574.md
---

# Foreign callback observation is executable, not yet live ownership evidence

H579 adds one private `winemetal_private_api.mm.o` to the frozen H571
default-policy image. Runtime/provider archives, the shader-count repair and
existing ownership observers are unchanged. There is no Clear candidate,
Wine staging, live run or new timing claim.

The observer binds each `execute_on_main` request to its body and each
`MTLSharedEvent_waitUntilSignaledValue` request to its listener callback.
A callback may execute after the waiting call returns; request identity
survives that return. Main-request and main-body CPU can overlap on one TID,
so they are not additive independent owners. Missing event callbacks remain
explicitly pending. Module-load attachment makes a valid zero-event mapping
distinguishable from a disabled or missing observer.

The sink reuses H566's process-birth-qualified mapping and self-thread CPU
sampling implementation with a separate namespace, environment and schema.
The lifecycle parser is new: it accepts nested request/body scopes and rejects
orphan, duplicate, incomplete or malformed scopes. It does not infer that
unobserved closure destruction costs zero.

## Executed checks

| Check | Result |
|---|---|
| arm64 and x86_64 synthetic observer | 32 requests, 128 rows; both pass |
| Empty attached mapping and observer OFF | Both architectures pass |
| Deliberate callback after request return | Identity and distinct callback TID verified |
| Malformed trace mutations | 14 rejected |
| Actual provider main dispatch | Three requests; two same-thread and one cross-thread body |
| Actual Metal shared-event API | One request and one matching callback |
| Actual API observer OFF/ON | Both pass with Metal API validation |
| Original STALKER capture OFF/ON | Exact source/output, query and lifetime checks pass |

Both original captures preserve seven mapped sources, 670 created/released
wrappers, query transcript and BGRX SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
The ON capture has a valid attached mapping with **zero foreign requests**.
This proves absence only in that replay fixture, not in the live scene, which
also owns window/presenter activity. The actual API fixture independently
proves that the instrumented callsites produce request/body records.

The provider compile emits nine warnings, all identical to a same-toolchain
syntax-only compile of the unchanged source: deprecated Metal APIs, the
existing tile-pipeline selector warning, and unused event callback parameters.
The two synthetic builds and actual API fixture build are warning-free.

## Closure audit and remaining scope

`dxmt9_pipeline_cache.cpp::CompileQueue::run` owns each local `packaged_task`
inside its loop. Task and captured job destruction occur before that loop's
existing H566 worker scope ends; this work is not a separate omitted owner.
Compiler, presenter and queue thread-entry wrappers capture `this` or function
objects; their startup/teardown must be distinguished from per-job destruction.

`archive_prewarm::beginAsyncFullLoad` additionally captures the archive path
and `onAttach`; `Archive::triggerSave` captures `reasonTag`. These captures are
destroyed after their lambda bodies return, outside H566 body endpoints.
H579 does not close that tail or the foreign blocks' runtime disposal.

Next live ownership composition must use the actual default ordinary queue,
retain the predeclared scene boundaries, and collect H579 alongside root,
Replay, Encode and worker evidence. A zero count in this standalone replay is
not permission to omit H579 from that composition. Observed CPU must not be
promoted to observer-free performance, and H574's failed population stays
closed. The goal's global maximum and live removable budget remain open.
