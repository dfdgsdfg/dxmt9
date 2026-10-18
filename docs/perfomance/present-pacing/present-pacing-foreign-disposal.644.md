---
domain: present-pacing
workload: STALKER original-capture and provider callback ownership controls
title: "H644 — original callback-block disposal is observable across request return"
type: evidence
status: provider-and-capture-qualified-live-global-ranking-open
updated: 2026-10-17
source: traces/stkcop-foreign-disposal-h644/{contract.md,observer.hpp,sink.hpp,parse.py,qualify.py,native/results.json,build.py,resume.py,qualified-build.json,api_fixture.mm,api.py,api/results.json,oracle.py,oracle/runs.json,verify.py,verification.json,corrections.md}
related: present-pacing-worker-lifecycle.643.md; present-pacing-thread-envelope.642.md; present-pacing-snapshot-performance.641.md; present-pacing-foreign-owner-preflight.579.md
---

# H644 — request return does not erase callback disposal

The private provider now observes the actual release of its owned original
callback block, not only callback-body execution. ARM64/Rosetta synthetic
controls, actual provider API controls and four fixed original-capture arms
pass. This closes a prerequisite for same-episode owner comparison; it does
not establish the globally largest default-path CPU owner or an additional
removable amount.

## Instrumentation and ownership

Only one private `winemetal_private_api.mm` TU changes. H643 archives and H639
mirror headers remain authenticated and unchanged; no product class layout,
Wine installation or runtime default is changed. Independent source inversion
recovers the original file exactly after removing the observation branch/helper.
The ON shared-event helper retains the original semaphore/listener/check/wait
logic. Nine existing compiler warnings match the baseline.

`H644_TRACE` adds request enter/exit, owned payload creation, body enter/exit,
and disposal enter/exit. A shared payload holds `Block_copy(original)`; its
destructor brackets `Block_release(original)`. Request identity survives timeout
and return. Final release can occur on a different thread, with or without a
callback invocation. Missing disposal remains outstanding rather than zero.

This includes captured-object destruction caused by that release. It does not
include every libdispatch/Metal instruction after release, nor objects still
retained independently by the original caller. Added wrapper allocation and
reference counting are diagnostic overhead. The stream is not evidence of
platform-thread retirement.

## Qualification

| Scope | Evidence |
|---|---|
| Native observer | Six fixed processes across ARM64/x86_64: OFF, ON, empty attachment |
| Ownership edges | Nested/synchronous calls, callback after return, cancellation without invocation |
| Destructor positioning | Actual captured-value destruction falls inside disposal endpoints |
| Negative controls | 20 malformed streams rejected |
| Actual provider API | OFF/ON with Metal API validation, three main dispatch requests and two real shared events |
| Controlled API edges | Two Objective-C event fixtures exercise timeout/late delivery and cancellation through the actual provider API |
| ON API stream | Seven requests, 47 rows, all disposed; one cross-thread main dispatch |
| Original capture | Reference OFF / reference ON / candidate ON / reference OFF, fresh processes |
| Capture ownership | 22 Encode/Finish/Completion/compile workers, 446 lifecycle rows, all joined |

The controlled events are deliberately not called real Metal GPU-completion
oracles. The original capture produces a valid empty foreign stream: it does
not exercise either modified callback branch. The separate API controls are
therefore necessary and are not replaced by the pixel check.

All four captured-input arms preserve original source/output pixels, 670
created/released resources, seven source mappings and the recorded query
transcript. Source/output SHA-256 remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Query checks remain production-counter diagnostics, not real occlusion-query
GPU oracle evidence. No CPU timing claim comes from these controls.

The initial compile's extra unused-argument warnings and the JSON event-key
comparison correction are preserved in `corrections.md`; no unfavorable runtime
arm was replaced. Xcode 27.0 / Metal 32023.921 on Apple M1 passed tool preflight.

## Remaining objective

H641 still supplies the repeated audit-off Replay removal evidence, not a
complete global ranking. The next population must combine matching PE-root
coverage, sparse Replay/Encode endpoints, H643 worker lifecycle and H644
foreign scopes in the same late-scene episode. Authenticate effective policies
for conditional workers, retain unresolved retirement/foreign work, and compare
against observer-OFF controls before transferring ownership to the default path.
