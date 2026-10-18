---
domain: present-pacing
workload: STALKER Clear-fusion candidate coordinator qualification
title: "Actual queue counterexample: failed fused Clear becomes successful inline completion"
type: evidence
status: private-candidate-promotion-blocked
updated: 2026-10-15
source: traces/stkcop-clear-queue-failure-h553/{queue_spec.cpp,run.py,verify.py}; traces/stkcop-clear-queue-failure-h553/r6/{build.log,build-receipt.json,results.json,verification.json,serial-1-ordinary.log,serial-1-exact.log,serial-2-ordinary.log,serial-2-exact.log,candidate-1-ordinary.log,candidate-1-exact.log,candidate-2-ordinary.log,candidate-2-exact.log}
related: present-pacing-clear-economy.552.md; present-pacing-clear-backed-null.551.md; ../../../specs/backend/gap.md
---

# Failed-open is not a successful empty source

H553 reproduces H552's static concern through the actual CPU-ready session
coordinator, FrameGraph backend, private H551 encoder and queue completion path.
The candidate's null-encoder failure becomes a successful inline completion
without executing the requested depth Clear. This blocks promoting the private
candidate unchanged; it does not invalidate H552's measured non-failing local
CPU saving or establish a live default-path rendering incident.

## Finite fixture

The native x86_64 fixture links the unchanged, hash-pinned H551 images. It
uses the existing `CommandQueueArenaLeaseTestAccess` seam, actual Metal
attachments and the production queue publication/coordinator. One or two
published sources each contain a depth-only Clear followed by one DrawRun.
The real recorder seam suppresses render encoder creation. Draw execution is
skipped to isolate attachment effects, as in H551. The queue is put into its
existing deterministic shutdown-drain test mode; there are no worker races or
timing claims. Normal and exact attachment-store policies are tested separately.

The same binary runs extension OFF and ON for each source count/store policy:
four controls and four counterexamples. Logs prove the private images loaded.

| Observation | Serial OFF control | Fused ON candidate |
|---|---|---|
| Source count | 1 or 2 | 1 or 2 |
| GPU completion records before watcher | 1 | 0 |
| Inline completion records before finish | 0 | Equal to source count |
| Final completion watermark | Equal to source count, after GPU completion | Equal to source count, without GPU submission |
| Candidate depth Clear | Not the failure-under-test | Seed depth .75 remains instead of requested .25 |
| Candidate resident/inflight sources after finish | Not the failure-under-test | 0 / 0 |
| Candidate query-failed / arena-poison status | Not the failure-under-test | false / false |

Candidate fixed seed color, depth and stencil all remain unchanged by independent
readback. The test's zero exit means the **unsafe counterexample reproduced**,
not that failure handling passed. The controls drain their actual GPU completion
records and finish sequences; they do not establish a new full rendering oracle.

## Cause and required boundary

`dxmt9_draw_encoder_chunk.mm::encodeChunk` and
`dxmt9_command_queue_cpu_ready_session.cpp::runCpuReadySessionEncodeLoop`
collapse exceptions through borrowed callbacks into missing optional submissions.
The coordinator's ordinary null-result branch invokes `completeInlineSnapshot`.
`dxmt9_queue.cpp::completeInlineChunk` validates source identity and ordering,
reclaims the source and queues completion, but has no retained encode-failure
fact to distinguish this result from legitimate inline work.

The directly injected H551 caller retained pending Clear identity before throwing.
That local invariant is insufficient once the enclosing coordinator can discard
the session and turn the failure into completion. Neither `stop_` alone nor
query-only failure flags define the missing contract: normal shutdown also
drains valid work, and this case has no query scope.

The next implementation must preserve an explicit encode-failure disposition
through borrowed callbacks and backend wrappers. Failure must not publish an
inline completion, replay the failed source or submit a partially modified
pending carrier as successful. Existing successful empty/inline operations
must remain distinguishable. Source identity, wake/terminal settlement and a
previously encoded pending carrier require separate verification before live use.

## Evidence limits and development attempts

This test uses the actual queue loop, not a copy of its policy. It covers only
fresh pending sessions with one or two failed sources, no hardware query scope,
and a shutdown drain. It does not yet cover failure after prior successful
encoding, already-submitted effects, live wakeups, reset or normal producer races.
No Wine staging, runtime promotion or canonical encoder edit occurs.

The first build failed on a const device call. Early fixture attempts incorrectly
expected the final watermark before consuming the finish queue; those failed
assertions are retained outside `r6` and are not evidence of successful failure
settlement. Final builds use the runtime's release `NDEBUG` definition. `r4`
reproduced the basic counterexample; `r5` added OFF controls; `r6` additionally
checks actual GPU completion records and drains control completions. Only `r6`
owns the table above. `verify.py` independently validates all eight logs, image
hashes and recorded counters without rerunning GPU work.
