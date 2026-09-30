---
domain: present-pacing
workload: closed-prefix native transition checker
title: "H421: Receipt consumer transaction and context-side-effect boundary"
type: evidence
status: bounded-consumer-checked-production-consumption-open
updated: 2026-09-30
source: traces/query-ranges-h421/{native.log,x64.log,tla.log}; tests/native/backend/session_release_event_spec.cpp
related: present-pacing-close-coordinator-owner.420.md; ../../../specs/backend/query-scope-ranges.md
---

# H421: A closed pass is necessary, but omitted context creation also matters

Base `d710cd6e` commits H420. The candidate consumer now renews a queue-owned
closed-prefix witness through the actual FIFO acknowledgement. Raw coverage
advances only on success. Rejection leaves the head and raw coverage intact
for the normal Encode path; submission and GPU completion are never discharged.
No production call site invokes the consumer yet.

## Bounded refinement

The native test enumerates 46,656 depth-six histories over reuse, encoder work,
publication, terminal stop, dirty-state mutation and ordinary close. The serial
oracle tracks whether a pass is closed, which publication it certifies and
whether context work remains. It is independent of the candidate's generation
comparison. Accepted reuse must match that oracle and advance the exact head
once; duplicate consumption must fail. Rejected reuse preserves the ordinary
fallback, which then actually acknowledges through the reference transport.
Separate cases reject nonretained writers, disabled ranges and SubmitAndWait.

This is exhaustive only over those bounded, mutex-serialized actions. It is not
a model of arbitrary concurrent dirty-lock acquisition, Metal callbacks, driver
behavior or OS scheduling. The actual queue mutation-edge coverage still needs
to be composed with a producer-side consumer call site.

## Concrete side effect identified

`dxmt9_render_pass_lifecycle.mm::closeEncodeChunkSessionRenderPass` returns
immediately for `NoActivePass`. However,
`dxmt9_command_queue.cpp::makeEncodeContext` first calls `consumePendingDirty`,
which takes `dirtyMutex_`, snapshots all dirty categories/high-water counters
and clears them. It also snapshots completion under the queue mutex.

The production-loop fixture now sets VS float registers 23–24 dirty and calls
the actual context constructor twice, without encoding or closing a pass. The
first context carries high-water 25 and a dirty mask; the second carries zero.
This pins the otherwise easy-to-miss side effect to executable code.

The new consumer requires `contextSideEffectsAbsent`. The native checker
supplies this abstract guard and rejects pending dirty work. A production
integration must synchronize its derivation, preserve lock ordering and decide
whether to require clean state or preserve the reference consumption explicitly.
It must not infer the guard from the closed-pass receipt alone. No assertion is
made that retaining extra dirty state is pixel-equivalent or performance-neutral.

## Scope

This change adds no production scheduling decision or runtime knob. Existing
range policy stays default OFF. Focused native/x86_64 and unchanged TLA results
are retained under `traces/query-ranges-h421/`; the TLA suite is prerequisite
regression evidence, not a new proof of the dirty-state guard. No new GPU exact,
wild FPS or full regression-attribution claim is made.

Final focused native tests pass 3/3, x86_64 plus install-name audit pass 4/4,
and the unchanged TLA suite passes 227 configurations (65 safety, five progress,
157 expected counterexamples). The explicit performance-leaf provenance audit
and `git diff --check` pass.
