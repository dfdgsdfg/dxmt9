---
domain: present-pacing
workload: STALKER CoP Normal replay preparation
title: "Cold Normal history restoration primitive; provider qualification remains open"
type: evidence
status: implementation-tested-replay-unqualified
updated: 2026-10-11
source: traces/stkcop-normal-restore-h482/verification/{native-testlog.txt,x86-testlog.txt,source-images-sha256.txt,x86-final-build.log,pe32-build.log,pe64-build.log,tlc-good.log,tlc-stale.log,tlc-partial.log,tla.log,tla-final.log,tla-qualified.log}
related: present-pacing-normal-history-input.481.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Restore original history independently of setup writes

H481 supplies original Normal history but not a qualified Normal replay.
This follow-up implements the missing **cold queue restoration primitive**,
not a newly admitted replay policy or a performance result.

`TapeAttachmentRestorePlan::prepare` joins the required original surface
generations to independently resolved local handles. It preserves original
touch bits and discard, rejects missing/duplicate/stale identities, and checks
the bounded alias relation: equal original storage/subresource and observed
current-backbuffer wrappers must map to the same local handle. Unrelated
original storage cannot collapse to one local handle. Wider physical-view
aliases are unsupported. Authentication of the original artifact and retention
of the resolved owners remain caller obligations.

`CommandQueue::restoreRenderTapeNormalAttachmentHistory` consumes this plan,
including on failure. It checks the **local** queue identity and published
queue/initializer prefix, actual completion, writer quiescence, live pool
generations, current backbuffer and unchanged Normal construction/history.
Installed capture, pending work, terminal state and policy ABA reject. Queue
and policy locks span admission through commit. The replacement touch set is
constructed off-state, then swapped atomically with the discard assignment.
Unmentioned local resources are preserved. There is no flush, wait, policy
toggle, captured-sequence adoption or content-validity assertion.

## Evidence

- Three focused ARM64 native targets pass; x86_64 passes the same three plus
  the Unix install-name audit. Unix and both PE builds complete.
- The actual inert queue and real NULL-marker surface arena exercise touch
  restoration without requiring Metal. These are **state tests, not pixel
  or physical texture tests**.
- Native cases cover required coverage, generation mismatch, aliases,
  subresources, current membership, independent touch/discard truth tables,
  active capture, local prefix mismatch, terminal/build state, policy ABA,
  destroyed handles and consumed-plan reuse.
- Both plan allocations and all four observed queue-staging allocation failure
  sites are injected. Four queue attempts reject without partial state; four
  later fault ordinals complete. Unrelated touched handles remain unchanged.
- `RenderTapeNormalHistoryRestore.tla` checks 48 distinct reachable states.
  Removing the local boundary guard violates `SafeCommit`; mutating live
  touches during staging violates `AtomicRestore`. The model binds preparation,
  local publication/reset/completion, locked staging, rollback and commit.
  It excludes GPU bytes, shader coverage, allocator implementation and caller
  retention. Native tests bind the actual allocation sites and pool behavior.

An initial full TLA invocation was invalidated by editing its running script;
that nonzero result is retained, not counted as verification. A subsequent
inventory check caught the newly added model/config counts and was corrected.
The isolated new model and both negative configurations have verified results.
The final stable-script run exits zero: all 233 configurations (67 safety,
6 progress and 160 expected counterexamples) meet their declared outcomes.
This includes both new counterexamples and remains regression evidence, not
Normal GPU replay qualification.

## Exact remaining integration boundary

The existing provider still forces Exact actions in
`replayGeneralPlan` and derives touched history
through `restoreSeededColorAttachmentTouches`. No production call site invokes
the new Normal restoration primitive yet. Existing stable-Exact launchers
continue rejecting H481 before executing a child.

The next integration must authenticate Normal history, construct and retain all
required initial owners (including aliases otherwise materialized lazily),
finish their actual local initialization, and restore original history before
the local capture baseline. Later seed/alias activation must not overwrite that
history. It must preserve wrapper equivalence and obtain first-read content
authority before an original-versus-replay GPU comparison can qualify the
input. Merely changing the provider's Exact boolean would not meet those gates.

No wild run or CPU timing is added here. Largest default-tail dxmt9 single-thread
CPU owner and correctness-qualified removable amount remain unresolved. This
primitive removes a known prerequisite to that experiment; it is not its result.
