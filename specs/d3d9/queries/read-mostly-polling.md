# Read-mostly backend polling: design review

Status: implemented experimental candidate (`DXMT9_QUERY_COVERED_PENDING=1`),
default OFF pending observer-OFF wild qualification.

The selector is additionally disabled when `DXMT9_ASYNC_QUERY_FENCES=0`, so a
signal-only initializer reference path cannot supply success certificates.

## Implemented boundary

`query::coveredPending` is the shared pure decision. A scalar, nonzero, eager
receipt may return Pending without the queue mutex only when its incarnation
matches, its ordinary prefix is at or below a durably covered prefix, ordinary GPU
completion is still below the target, and its initializer prefix is already
successfully completed. All markers, new/unvalidated prefixes, initializer-only
receipts and possible successes take the original path. Non-FLUSH calls may
reuse the same certificate but cannot create one.

Only a successful locked FLUSH poll publishes coverage, after
`requestQuerySubmission` returns (including notification for a new request).
That owner serializes coverage publication; requested/submitted reuse under
the same queue mutex cannot overtake another request's wake. Successful upload
certificates come from `queryProgressUnlocked`, never the event signal alone.
Both certificates use release/acquire and monotone prefixes. Combining two
stale-low reads cannot invent coverage or successful uploads. No mutable
initializer tracker or `stop_` bool is read by the bypass.

Every production `CommandQueue::stop_ = true` site first publishes a sticky
query terminal latch; controller poison publishes `queryCompletionFailed_`.
The reader also checks atomic arena poison and completion failure. The latch
is not cleared on thread restart, conservatively disabling the candidate for
that incarnation. Terminal overlapping the control observations may linearize
after this Pending result, just as a failure can overlap the old locked return.
Acquire is not a promise of immediate observation across threads. Device reset,
issue epoch, retained backend and output-byte gates remain in the existing
snapshot/bridge wrapper. No successful result is authorized by cached state.

The initializer tracker may discover a new unrelated upload error only when
its authoritative owner polls; this candidate does not accelerate error
discovery. It cannot bypass an unsatisfied initializer dependency. Once ordinary
completion reaches the target, the original path refreshes initializer status.

`QueryCoveredPending` separates Request, Wake, Grant, stale reads and decision;
five mutations reject early coverage/request, early coverage/wake, invented
upload success, ignored terminal and wrong incarnation. Its safety argument
composes with the existing publication progress model; it does not prove C++
cache fairness, driver error discovery, timing or FPS. Native exhaustive truth
tables call the production predicate. The gated GPU fixture tests a real poll
while another thread holds the queue mutex, one-FLUSH progress, invalid identity
and initializer prefix, terminal rejection and exact 256-byte readback.

The backend observer now counts both covered-Pending and slow-path attempts,
with explicit `covered_pending` and `slow` totals. Fast samples cover only the
return observation, not the preceding atomic predicate. Slow phase timings
retain their previous meaning; these are not full entry-to-exit timings.
The receipt census also includes attempts that do not reach backend polling.
Observer-OFF matched performance qualification remains open.

## Verification record (2026-09-19)

Base `c6baedeb` plus this implementation. Full native, x86_64 Unix (including
install-name fixup), PE64 and PE32 builds pass. Native candidate-ON focused
tests pass 7/7, reference/OFF lifecycle tests pass 8/8, and the explicit
async-OFF policy gate passes 1/1. The x86_64 candidate predicate, gated GPU
oracle and install-name audit pass 3/3. Full TLC passes in 295.05 seconds:
65 safety, 5 progress and 157 expected-failure configurations (227 total),
including the new five counterexamples. `git diff --check` passes.

Local logs: `build/meson-logs/query-covered-{final-tests,off-tests,policy-test,
x86-tests,tla}.log`. No wild run or FPS improvement is claimed. The GPU fixture
proves 64 polls complete with the submission mutex held by the other thread;
it does not prove public-wrapper wait freedom or every hardware failure mode.

## Motivation and scope

H326's sampled backend polling cost is dominated by queue mutex acquisition
(87.73%). Pending submission requests already reuse a requested/submitted prefix
99.81% of the time. This motivates bypassing the large submission mutex for
repeated observations, not deleting the persistent submission obligation.
Neither percentage is a frame-time fraction or a measured FPS opportunity.

## Pre-change authority audit

| State | Current owner/publication | Consequence |
|---|---|---|
| Ordinary completion | `drainCompletedSequence`, release-store to `completedSeqId_` | Acquire reads are conservative progress, not an all-fields success certificate. Inflight/present bookkeeping and possible settlement failure follow the store. |
| Queue identity | Non-recycled `queryQueueIdentity_`, retained backend | Compare the receipt incarnation; a sequence alone is insufficient. Exhaustion fails closed. |
| Terminal | `stop_` under queue mutex; atomic poison and completion-failure flags | Existing completion acquire cannot replace the complete terminal predicate. |
| Initializer completion | `queryProgressUnlocked` polls event AND command-buffer status, mutates upload tracker and may stop scheduling | This is not a const read and cannot simply be called without the queue lock. Event signal alone is insufficient. |
| Submission obligation | Requested/submitted prefixes under `pendingCompletionMutex_`; publication checked under queue mutex | A cache hit must prove persistent request coverage or actual submission. Publication is not coverage. |
| Device reset/reissue | Retained snapshot, issue epoch, replay ledger and final device control recheck | Backend optimization must preserve these caller-side gates and pending/error output preservation. |

## Recommended first implementation boundary

Keep successful completion and new submission requests on the existing locked
path initially. Optimize only repeated Pending observations of a bound,
identity-qualified receipt whose FLUSH obligation is already covered.

1. Require a retained backend lifetime and a bound receipt validated by the
   authoritative owner. An unbound marker, unknown incarnation or unvalidated
   prefix takes the locked path; never synthesize publication from a sequence.
2. Read a monotone ordinary completion prefix. A stale low value may conservatively
   return Pending, but it does not permit a false Complete or false Invalid.
3. For FLUSH, require an acquire-visible, same-incarnation coverage certificate
   published only after a sticky request is installed with a guaranteed wake,
   or after actual submission. Without coverage, use the current request path.
   Do not publish coverage before the wake obligation becomes durable.
4. Check an authoritative terminal publication and retain the device's existing
   reset/identity protocol. Cover every stop/poison/failure writer before adding
   the bypass. Stale completion is safe; stale terminal acceptance is not an
   excuse to weaken the existing failure contract. Define the poll linearization
   point and overlapping failure explicitly.
5. If ordinary completion reaches the receipt, take the locked path to refresh
   initializer status and classify success. Do not wait for an independently
   cached initializer prefix with no actor guaranteed to update it.
6. Leave zero-prefix/initializer-only receipts on the locked path initially.
   No busy seqlock retry, new per-poll allocation, or raw mutable result access.

This deliberately does not promise lock-free S_OK. A later successful-read path
needs owner-published successful ordinary AND initializer completion, terminal
ordering and retained result visibility, with an independent refresh/progress
actor. Several independent atomics do not automatically constitute one coherent
`query::Progress` snapshot.

## Model connection and acceptance gate

| Contract | Existing model anchor | Required extension/test before bypass |
|---|---|---|
| No invented completion | `QueryPublicationFence.NoInventedCompletion`, `CompletionSafety` | Split read/return actions; stale low reads; completion followed by terminal before return. |
| FLUSH progresses | `StickyRequest`, `SubmissionPressureSafety`, `Progress` | Covered-hit versus slow request; reject `CoverageBeforeRequest`, `CoverageBeforeWake`, and coverage derived only from publication. |
| Correct incarnation | Publication model epoch; `QueryReplayReceipt` | Reset/reissue between reads; retired backend identity and stale receipt; unchanged output on error/Pending. |
| Upload dependency | `QueryInitializerFence.CompletionSafety`, `Progress` | Reject signal-only success and a cached Pending loop without initializer refresh. |
| No lost progress | Publication model fair Submit/GPUComplete | Reader eventually observes publication or reaches authoritative refresh; hardware cache fairness must be stated, not inferred from acquire. |

Bind the branch decision to a small production-used pure reducer with exhaustive
native truth tables. Then test the real queue path with delayed submission,
terminal/reset injection, initializer failure and mixed observations. Existing
TLA models explicitly exclude C++ memory ordering: they are prerequisites, not
a proof of this new design. Add a release/acquire publication argument separately.
Run native/TLA and GPU result-byte oracles before observer-OFF wild qualification.

## Measurement limits

Queue split rows describe instrumented owners, not a waiter-to-holder join.
Nested intervals must not be summed. The existing diagnostic table takes its own
mutex while some callers still own the runtime lock; it perturbs contention and
does not include its own aggregation time in those hold samples. Device resource
creation/destruction, initializer entries and newer direct/query paths need a
coverage audit before attributing the entire queue wait population.
