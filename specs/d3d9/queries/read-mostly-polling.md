# Read-mostly backend polling: design review

Status: proposed; no runtime lock bypass or new verification claim.

## Motivation and scope

H326's sampled backend polling cost is dominated by queue mutex acquisition
(87.73%). Pending submission requests already reuse a requested/submitted prefix
99.81% of the time. This motivates bypassing the large submission mutex for
repeated observations, not deleting the persistent submission obligation.
Neither percentage is a frame-time fraction or a measured FPS opportunity.

## Current authority audit

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
