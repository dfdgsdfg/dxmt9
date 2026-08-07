---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #234 - Multi-Source Replay Recovers Locality but R11 Times Out"
type: leaf
status: current
updated: 2026-08-06
source: experiments/output/app-d3d9-3dmark05-fragment-lookahead-off-gt2-r13-20260806/result.json; experiments/output/app-d3d9-3dmark05-cap-observe-on-gt2-r15-20260806/result.json; experiments/output/app-d3d9-3dmark05-page640-on-gt2-r16-20260806/result.json; experiments/output/app-d3d9-3dmark05-carrier-canonical-on-gt2-r17-20260806/result.json; experiments/output/app-d3d9-3dmark05-natural-attribution-on-gt2-r18-20260806/result.json; experiments/output/app-d3d9-3dmark05-exact-natural-on-gt2-r14-20260806/result.json
related: specs/backend/encode-scheduling/gap.md; specs/backend/encode-scheduling/requirements.md; specs/backend/encode-scheduling/spec.md
---

# Present-Pacing #234 - Multi-Source Replay Recovers Locality but R11 Times Out

## Question

Can one bounded combined FrameGraph over an already-Ready source prefix repair
cross-source same-attachment re-entry without changing command-buffer,
completion, or scheduler-progress shape?

## Implemented experiment

The default-off CPU-ready Tape lane gained a serial planner and executor with
these constraints:

- at most eight already-Ready compatible sources;
- one combined alias-resolved RAW/WAR/WAW graph seeded by the active render
  pass;
- exact `(retainedSourceIndex, commandIndex)` command identity;
- complete DrawRun-only cross-source permutations;
- complete retain, resolve, range, run, completion, admission, and carrier
  preflight before the first Metal effect;
- source-qualified fragment replay with completion registered once in natural
  FIFO order; and
- natural source-order fallback before effects and fail-stop after the first
  fragment replay call.

Native production-path fixtures cover Legacy `A` plus Arena `B,A`, the
eight-source bound with a ninth natural successor, and a natural Present tail
in the same session and command buffer. The focused native suite, related
broader suite, TLA checks, and x86_64 release build passed.

## R11 same-build result: locality recovered, run wedged

R11 compared fragment replay off and on with the same staged unix provider hash,
`780333fb9d959db5`. Both runs used GT2, no GPU trace, and the repository wild-
run wrapper.

| Counter | Fragment off | Fragment on |
|---|---:|---:|
| Wrapper completion | completed | timed out after progress stopped |
| Return code / `timed_out` | `0` / `false` | `143` / `true` |
| Process elapsed | 81.586 s | 281.055 s |
| Presents | 1,582 | 720 |
| Command buffers | 6,327 | 2,926 |
| Command buffers / Present | 3.9994 | 4.0639 |
| Render passes | 24,967 | 11,256 |
| Render passes / Present | 15.7819 | 15.6333 |
| Planned/replayed windows | 0 | 579 / 578 |
| GPU errors / chunk rejects | 0 / 0 | 0 / 0 |
| Post-effect encode/fold fatals | 0 / 0 | 0 / 0 |

The fragment-on partial interval recovered the historical command-buffer and
render-pass locality envelope, but it is not a successful performance result:
the run stopped advancing after 720 Presents and the wrapper terminated it on
timeout. The generated `result.json` also contains `status: "pass"` because its
collected checks found no listed failure; that field must not override
`timed_out: true` and return code 143. No promotion or performance conclusion
may use this incomplete interval.

The leading code-level cause candidate is scheduler lock scope. The composite
FrameGraph planner performs graph construction and resource-alias resolution,
and the transaction observer may update/export diagnostic state. Running either
under the queue scheduling mutex can prevent Ready publication, reclaim, and
other scheduling progress even when Metal and correctness counters remain
clean. The corrected transaction contract is therefore:

1. reserve the exact prefix as `TentativeRepresented` and snapshot its source,
   fence, lease, capture, initializer, and frontier generations;
2. release scheduling before planner/resource work;
3. reacquire and exactly revalidate before commit, or discard the plan and
   restore the exact Ready prefix with no effect and no observer; and
4. after all qualified fragment effects and carrier folds succeed, invoke the
   observer exactly once in natural FIFO order with scheduling released.

The current dirty implementation follows that sequence, but R11 predates its
runtime validation. A fresh same-build tape-off/on GT2 run must complete before
the timeout can be called fixed or the recovered locality shape can count
toward `R-BACK-2.50`.

## R14 rejected result: exact-natural execution lost progress

R14 temporarily sent a validated `NaturalAfterMerge` result through the exact
fragment transaction instead of the source-local natural fallback. The staged
unix provider hash was `2148c3280913301d`; the artifact is
`experiments/output/app-d3d9-3dmark05-exact-natural-on-gt2-r14-20260806/result.json`.

| Counter | Partial result |
|---|---:|
| Wrapper completion | timed out |
| Return code / elapsed | `143` / 281.43 s |
| Presents | 900 |
| Exact-natural windows / sources / runs | 713 / 1,882 / 1,882 |
| Planned windows / carrier fallbacks | 1,414 / 111 |
| GPU errors / chunk rejects / FIFO failures | 0 / 0 / 0 |
| Fragment encode / carrier-fold fatals | 0 / 0 |
| Current lease used | 6 sources / 121 pages |
| Current Tape residency | 8 sources / 123 pages |
| Source-plus-page cap events | 34 |

The error counters remained clean and the current lease was not full, but the
run froze at 900 Presents. The timeout is therefore a failed progress gate,
not a usable locality or performance sample. The exact-natural execution code,
counters, and native activation fixtures were removed; `NaturalAfterMerge`
remains non-executable and falls back to source-local natural replay.

## R15 result: capacity demand is split between pages and sources

R15 added observation only: it did not change admission, release, replay, or
capacity policy. The tape-on GT2 run completed normally with provider hash
`15ae6d5af2055d13`; its artifact is
`experiments/output/app-d3d9-3dmark05-cap-observe-on-gt2-r15-20260806/result.json`.
R13 tape-off remains the locality reference because the R15 code changes only
enabled perf-counter state and admission result diagnostics.

| Counter | R13 tape-off | R15 tape-on |
|---|---:|---:|
| Return code / `timed_out` | `0` / `false` | `0` / `false` |
| Presents | 1,597 | 1,493 |
| Command buffers / Present | 3.9994 | 4.0422 (`+1.07%`) |
| Render passes / Present | 15.7752 | 15.9210 (`+0.92%`) |
| Source-only cap events | - | 14 |
| Page-only cap events | - | 24 |
| Source-and-page cap events | - | 1 |
| Peak predecessor sources / pages | - | 30 / 384 |
| Peak candidate payload / wrap / required pages | - | 62 / 36 / 96 |
| Peak required total sources / pages | - | 31 / 437 |

The current 512-page arena reserves 127 pages for one worst-case successor,
leaving a 385-page session. The observed 437-page maximum therefore predicts
that a 640-page arena, with the same successor reserve, would provide a
513-page session and remove the sampled page-cap releases. It cannot remove
the 15 source-axis releases because the 31-source publication ceiling remains
unchanged. This is a bounded next experiment, not evidence that the locality
gate has passed or that 640 pages is a final production policy.

## R16 result: more pages defer rather than eliminate the cap

R16 changed only the streaming page policy from 512/256 high/low water to
640/320. The source, Ready, and 64-page ordinary-source limits were unchanged.
The run completed normally with provider hash `3f91dc477331ccfb`; its artifact
is
`experiments/output/app-d3d9-3dmark05-page640-on-gt2-r16-20260806/result.json`.

| Counter | R15 512 pages | R16 640 pages |
|---|---:|---:|
| Presents | 1,493 | 1,529 |
| Command buffers / Present | 4.0422 | 4.0275 |
| Render passes / Present | 15.9210 | 15.9477 |
| Source-only / page-only / combined caps | 14 / 24 / 1 | 16 / 11 / 1 |
| Peak predecessor pages | 384 | 509 |
| Peak required total pages | 437 | 546 |
| Carrier fallback | 22 | 27 |

The additional 128 pages reduced page-axis releases and removed about 22
normalized command-buffer excesses, but did not eliminate them. Once the
session could grow past the old cap, its predecessor grew to the new 513-page
limit and exposed a later 546-page demand. Render-pass locality did not improve,
and strict locality still failed against R13 tape-off by `+0.70%` command
buffers and `+1.09%` render passes. A finite arena increase is therefore a
bounded command-buffer mitigation, not the structural solution; source credit
and pass continuity remain independent limits.

## R17 result: carrier canonicalization is correct but not the blocker

R17 accepts a carried record's non-empty completion list only when it is
exactly equal to both queue-owned and session-owned FIFO lists across source
and storage generations, slot, sequence, Present, and command range. It then
clears that redundant copy before the first reordered fragment effect and lets
the finalizer republish the complete session list. Native positive and negative
fixtures plus TLA passed, and the GT2 run completed normally with provider hash
`7c0b5a1aa784a17f`. The artifact is
`experiments/output/app-d3d9-3dmark05-carrier-canonical-on-gt2-r17-20260806/result.json`.

| Counter | R16 | R17 |
|---|---:|---:|
| Presents | 1,529 | 1,542 |
| Carrier fallback | 27 | 0 |
| Command buffers / Present | 4.0275 | 4.0305 |
| Render passes / Present | 15.9477 | 15.9754 |
| Source/page cap releases | 17 / 11 | 18 / 11 |
| Distance 1-4 same-key re-entry | 235 | 286 |

Eliminating every sampled carrier fallback did not improve either locality
axis. Against R13 tape-off, R17 remains `+0.78%` in command buffers and
`+1.27%` in render passes. The remaining shapes are now more specific: 29 cap
releases track roughly 33 normalized primary-command-buffer excesses, while
286 short same-key re-entries track roughly 309 render-pass excesses. Source
credit lifetime and progress-safe activation of natural combined plans are the
next independent structural questions.

## R18 result: exact-window attribution is blind to the carried active seed

R18 added observation-only replay-window provenance without changing replay,
pass, capacity, or release policy. The GT2 run completed normally with staged
unix provider hash `66f082a6fde6fd54`; its artifact is
`experiments/output/app-d3d9-3dmark05-natural-attribution-on-gt2-r18-20260806/result.json`.

| Counter | R18 |
|---|---:|
| Return code / `timed_out` | `0` / `false` |
| Presents | 1,559 |
| Command buffers / Present | 4.0282 |
| Render passes / Present | 15.9211 |
| Natural fallback windows started / completed / sources | 1,464 / 1,464 / 3,734 |
| Permutation fallback windows started / completed / sources | 77 / 77 / 348 |
| Natural-attributed render-pass starts | 7,253 |
| Short same-key re-entry d1 / d2 / d3-4 | 199 / 15 / 4 |
| Natural same-window re-entry d1 / d2 / d3-4 | 0 / 0 / 0 |
| Natural cross-window re-entry d1 / d2 / d3-4 | 155 / 14 / 4 |
| GPU errors / chunk rejects / FIFO failures | 0 / 0 / 0 |

Planner outcomes conserve all 2,770 attempted windows as 1,228 planned,
1,464 `NaturalAfterMerge`, 77 rejected permutations, and one unproved moved
head. Both source-local fallback populations also conserve started and
completed windows exactly.

The 173 cross-window observations cover 79.36% of the 218 short re-entries,
but zero same-window observations do not prove that natural fallback is
unrelated to the pass excess. The queue assigns a new window identity from the
first selected source ordinal only after planning, while the carried active
render snapshot contains attachment and hazard proof but no physical pass
lineage. The tracker requires the prior `A`, every intervening pass, and the
current `A` to share the exact new Natural identity. A physical
`seed A | selected B,A` interval therefore classifies as cross-window even when
the planner merged the selected passes with that active seed. This is material
because 1,277 of 1,464 `NaturalAfterMerge` windows were active-seed
distance-one cases.

R18 consequently localizes the next observability gap but does not decompose
the locality blocker: the 173 observations combine true window crossings with
carried-seed intervals whose seed cannot be joined to the current planner
window. An exact active-pass token and source-qualified merge-target witness
are required before choosing an execution policy. The R13 locality gate still
fails by `+0.72%` command buffers and `+0.92%` render passes, and
`NaturalAfterMerge` remains non-executable.

## R19 result: active-seed merges continue the existing physical pass

R19 adds the missing causal join without changing scheduling or Metal policy.
The active pass now carries an exact physical `(seqId, encoderIndex)` token,
exposed separately from the semantic planning snapshot so token availability
or mismatch cannot change completeness, equality, plan acceptance, or prefix
restore. Perf-on token mismatch drops only attribution and is counted as stale
or unavailable. Passcoalesce emits a bounded witness at the successful
active-seed merge mutation. The planner maps that witness to an exact retained
source/local-command target, sorts the complete set for source-local handoff, and exposes no
partial tickets after overflow or inconsistency.

Only a revalidated `NaturalAfterMerge + SeedMerged` fallback receives a ticket.
The render-pass tracker matches it at the actual target pass start against the
prior same-key token and requires every one-through-four intervening pass to
carry the current Natural window. The new counters separate issued, matched,
continued-on-the-active-seed, consumed mismatch, unconsumed/no-begin, witness
overflow, witness mismatch, and matched d1/d2/d3-4 seed bridges. Native
production coverage pins exact active `A | B,A` as one d1 match, immediate
`A | A,B` as one continuation, and a wrong target as one consumed mismatch;
pure tests pin token, target, window, bounded-distance rejection, and
non-monotonic command lookup. Issuance moves into the post-admission encode
guard, making every issued call conserve while earlier failures stay unissued;
perf-off/empty calls skip lookup and classification. Focused native tests pass,
and the wild artifact is
`experiments/output/app-d3d9-3dmark05-active-seed-bridge-on-gt2-r19-20260806/result.json`.

| Counter | R19 |
|---|---:|
| Presents | 1,565 |
| Command buffers | 6,306 |
| Command buffers / Present | 4.02939 |
| Render passes | 24,990 |
| Render passes / Present | 15.96805 |
| Active-seed tickets issued | 2,121 |
| Continued on active seed | 2,121 |
| Reopened matched / mismatch / unconsumed | 0 / 0 / 0 |
| Seed token stale / unavailable | 0 / 0 |
| Witness overflow / mismatch | 0 / 0 |
| Seed bridge d1 / d2 / d3-4 | 0 / 0 / 0 |
| Short cross-window re-entry | 199 |
| Admission waits / total | 22 / 198.472 ms |
| GPU errors / completion-FIFO failures | 0 / 0 |
| Sampled average FPS | 23.540 |

The ticket population conserves exactly: all 2,121 issued witnesses were
consumed as `continued`, with no reopen, mismatch, unconsumed target, stale or
missing token, or witness failure. The 199 short cross-window re-entries are
therefore not carried active-seed merges hidden by R18's window identity rule.
R19 resolves that ambiguity and shows that the active-seed merge is not the
physical pass-close cause: its target was already admitted into the open pass.

Do not run an R20 checkpointed-Natural experiment. Checkpointing a combined
Natural result cannot remove a pass boundary that the exact witness says was
never created. `NaturalAfterMerge` remains non-executable, and the next leaf
must attribute actual physical pass-end causes before choosing another
scheduling or execution mechanism.

## R20 result: deterministic capacity finalization reopens the same key

R20 is observation-only. Queue submission paths now carry a typed session
finalize cause into the existing finalizer, and `endRender` records the exact
physical pass token before clearing it. The bounded encode-thread ledger also
retains the attachment key, `EncoderSplitReason`, and finalize cause. No replay order, pass close, command buffer,
release fence, capacity policy, or completion behavior changes. Perf-off skips
recording and lookup entirely.

At the next physical pass start, immediate adjacency is accepted only through
the exact prior `(seqId, encoderIndex)`. A Natural short-cross re-entry looks up
the exact prior same-key token and reports its close split reason; failed joins
have a separate missing counter. Present is the deterministic frame terminal,
so the completed-frame conservation equation is:

```text
close_ledger_recorded
  = terminal_adjacent
  + terminal_nonadjacent
  + terminal_not_reopened_before_present
```

The same equation is exposed for the Final-close-only subset.

Natural short-cross attribution separately conserves as the sum of all matched
close-reason buckets plus `natural_short_cross_close_missing`. Final close and
same-key adjacent reopen counters are partitioned by SessionCap, independent
submission, initializer wait, producer wait, drain, and fail/other. Existing
SessionCap source/page/both demand counters supply the axis breakdown; R20 does
not widen the ordered release-event ABI.

Native evidence covers exact-token hit/miss, short-cross lookup, bounded frame
reset, and a production 30+1 SessionCap sequence in which active A finalizes
and the Ready suffix reopens A on a new carrier. Both perf-on and perf-off runs
retain 31 replay calls, two pass begin/end pairs, and FIFO completion; only the
perf-on run records `SessionCap final close +1` followed by `SessionCap
same-key adjacent +1`.

The wild artifact is
`experiments/output/app-d3d9-3dmark05-pass-close-ledger-on-gt2-r20-20260806/result.json`;
the provider hash begins `9df3b12`.

| Counter | R20 |
|---|---:|
| Presents | 1,560 |
| Command buffers / Present | 6,285 / 4.028846 |
| Primary command buffers / Present | 1,595 / 1.022436 |
| Render passes / Present | 24,906 / 15.965385 |
| Sampled average FPS | 23.296 |
| Close ledger recorded / missing | 24,906 / 0 |
| Terminal adjacent / nonadjacent / not before Present | 30 / 23,317 / 1,559 |
| Final recorded / missing | 31 / 0 |
| Final adjacent / nonadjacent / not before Present | 30 / 1 / 0 |
| Final cause SessionCap / initializer | 29 / 2 |
| Adjacent cause SessionCap / initializer | 28 / 2 |
| Permanent same-key adjacent | 30 |
| Natural short-cross matched / missing | 198 / 0 |
| Cross close ClearBarrier / render-target change | 195 / 3 |
| Cross close Final / all other reasons | 0 / 0 |
| Active-seed tickets issued / continued | 2,032 / 2,032 |
| Other active-seed ticket outcomes | 0 |
| SessionCap sources / pages | 18 / 11 |
| Admission waits / total / maximum | 20 / 320.393 ms / 128.752 ms |
| GPU errors / completion-FIFO failures | 0 / 0 |

Both ledger equations close exactly. More importantly, 28 of the 29
SessionCap-finalized physical passes reopen the same attachment key immediately;
the other two adjacent reopens follow initializer closes. The
Natural short-cross population is disjoint: all 198 cases are explained by 195
ClearBarrier and three render-target-change closes, with zero Final close. R19's
active-seed result also repeats—every one of 2,032 issued targets continued the
open physical pass.

This makes deterministic capacity finalization the measured session-boundary
locality blocker. Enlarging the cap merely moves a finite boundary, and
checkpointed Natural still has no demonstrated boundary to remove. The next
structural target is to decouple payload/source residency lifetime from the
live Metal session and completion identity, allowing storage reclaim and
back-pressure progress without closing the active pass. Do not proceed with cap
enlargement or Natural execution.

## Post-encode retirement P0/P1 boundary

The first retirement slice is intentionally observation and value-contract
only. The code audit found three distinct lifetimes that the R20 conclusion must
not collapse:

| Lifetime | Current carrier | Earliest proven release point |
|---|---|---|
| Borrowed replay bytes | `SourcePayloadView`, resolved source, partition/store-proof spans | End of the synchronous represented encode call |
| Payload owner/destructor plane | Tape-owned `ChunkSlot` or `ArenaSourcePayloadBlock` chain, including `DrawShaderLayoutContext` resource owners | Only through detach-under-lock, destroy-outside-lock, generation-advance-under-lock; destructor re-entry forbids in-lock release |
| Completion/reclaim identity | `QueueCompletionSource::source`, session fixed list, `PendingCompletion` | Generation-checked Tape reclaim after tail completion |
| Metal and CPU sidecars | default-retaining Metal command buffer, callback captures, samples, `retainedPayloads` | Per owner: Metal tail completion or explicit callback/record destruction |

`EncodedCommandId` now describes the locator-free attribution target, while
`UnverifiedEncodedCompletionSpan` and `SessionCompletionAccumulator` pin the
current dense source-sequence arithmetic. They are not installed in production
fields and are not queue seal authority. StateOnly and Legacy raw interposition
does not create a published-source seqId gap in the current allocator; gap,
duplicate, Present-tail append, invalid identity, endpoint/count mismatch, and
bounded overflow are therefore rejection cases.

The Metal evidence in this slice is a source-contract audit of the exact
production/WMT/Objective-C ordinary `commandBuffer` call chain, backed by
Apple's `retainedReferences=true` default contract. It is not a GPU lifetime
experiment and does not retain Tape pages or arbitrary C++ owners. The native
empty-session probe separately shows that the generic submission retention
helper destroys its session owner when `retainedPayloads` releases it; the
production coordinator still drops a completely empty session before invoking
that helper.

The next executable slice requires an opaque queue-owned completion seal plus a
static owner audit that forbids borrowed views in asynchronous storage. Only
then can one payload owner be retired after encode while retaining exact
completion locators independently. No early reclaim, cap, session-finalization,
or default-gate policy changed in P0/P1.

## Earlier R4 result

The final run used `DXMT9_CPU_READY_TAPE=1`, GT2, no GPU trace, frame sampling,
and the repository wild-run wrapper. Its staged unix provider hash was
`580caf70fa917e36`.

| Counter | Result |
|---|---:|
| Presents | 1,580 |
| Command buffers | 6,380 |
| Command buffers / Present | 4.038 |
| Render passes | 27,802 |
| Render passes / Present | 17.596 |
| Distance-one same-key re-entry | 2,823 |
| Multi-source windows attempted | 1,527 |
| Eligibility fallback | 531 |
| Non-consecutive-identity fallback | 307 |
| Present-boundary fallback | 224 |
| Eligible active-seed merges | 996 |
| Natural-after-merge | 996 |
| Planned/replayed windows | 0 |
| Second-non-draw / blocked-cycle | 0 / 0 |
| GPU command-buffer errors / chunk rejects | 0 / 0 |

The earlier tape-off envelope was about 4.000 command buffers and 15.77 render
passes per Present. It used provider hash `7be7db3bce184e7f`, so it is a
historical locality envelope rather than a same-build A/B baseline. The current
run retains the command-buffer envelope but leaves roughly 11.6% more render
passes than that earlier baseline. This fails the locality promotion gate.

## R16 failed progress evidence: ordered-tail Writing double count

The next tape-on GT2 frame-12 run again failed the progress gate. The terminal
report captured the app thread in `drainDeferredReplayForBufferLock`, replay in
`beginCpuReadyArenaSource` through a `commitCurrentChunk` capacity wait, and the
encode thread in the first-lease denial capacity-generation wait; finish and
Metal completion were idle. The last capacity report contained exactly 31
Ready sources plus one Writing source, 511 resident pages, and post-encode
retirement conservation of 497 / 497.

That state exposed an accounting cycle: the fixed lease already reserves one
complete successor in `successorHeadroom`, while the old state-blind Tape
snapshot also reported the same ordered Writing publication as unavailable.
The resulting double count denied first acquisition, so no Ready head could
encode and release the compatibility writer. The report's `lease_current = 1`
was from an earlier periodic snapshot; it was not simultaneous with the later
denial branch and is not evidence that two leases coexisted.

The correction credits only one generation-valid ordered-tail Writing claim
that fits the full successor vector, without shrinking the acquired lease.
Native and TLA+ evidence cover the correction, but no replacement GT2 wild run
was performed in this worktree. R16 therefore remains failed runtime evidence,
and the Tape gate remains default off with no promotion, locality, or
performance claim.

## Historical R4 verdict and current evidence limit

R4 showed that the first planner reached production GT2 windows, preserved
correctness counters, and did not activate a replay permutation. R11 later
activated qualified replay and recovered locality in its partial interval, but
its timeout is a stronger failed progress gate. The Tape lane must remain
default off.

The run does not yet prove why all 996 merges remained natural. The permanent
counters do not correlate `NaturalAfterMerge + SeedMerged` with the already-
computed first matching pass distance. Therefore an already-adjacent natural
target and an intervening target kept after its producer remain aggregated.
That R4 correlation remains historical diagnostic context rather than the
current blocker. The immediate evidence requirement is a completed post-lock-
scope same-build run; dependency rules must not be weakened to force
activation.
