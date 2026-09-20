---
type: "Spec"
title: "Identity-qualified blocking wait trace"
description: "Bounded diagnostic evidence for CPU replay and polling waits."
tags: [backend, queries, diagnostics]
---

# Identity-qualified wait trace (R-BACK-WAIT-1)

The diagnostic must preserve production wait predicates, lock ordering,
submission obligations and failure results. It is not an optimization or a
new synchronization authority. Disabled entry points must perform at most a
cached gate, without clocks, diagnostic payload construction or TLS access.

The trace selects an inclusive process-global encoded-Present window. It must
record every condition-variable wait entered in that window, subject to an
explicit fixed capacity, rather than sample every Nth poll. Mutex acquisition
intervals above a declared threshold may also be recorded; elapsed lock
acquisition does not by itself prove blocking or identify a lock holder.

Each replay row must identify a process-local ledger incarnation, target,
captured raw ordinal, initial/final admitted and completed frontiers, terminal
classification and calling thread. The target address alone is not identity.
Ledger address reuse must receive a new incarnation. Query context adds the
immutable query identity; backend queue acquisition additionally carries its
queue and ordinary END prefix. Observed completion frontiers are evidence of
predicate settlement, not a claim about which notification woke the OS thread.

Mutex acquisition and condition-variable wait/reacquisition intervals must
be separate. A wait count means a pending predicate entered the CV operation;
spurious wakeups and scheduler delay remain included. No reported interval
may be labelled exclusive CPU, GPU time or removable FPS cost.

Storage must be bounded and diagnostic-owned, with no allocation, log output
or sink mutex in the measured locked interval. Export occurs outside the
window. Missing window closure, truncated output, capacity overflow, unknown
identity and in-flight writers must make complete attribution unqualified.
Existing evidence files must not be overwritten. Cross-window waits must
retain their entry/exit frames and be excluded from interior totals.

Native tests must cover ready/no-wait, genuinely pending predicates, terminal
failure, supersession, owner reuse, bounds, output completion and disabled
behavior. This observation-only change adds no rendering/refinement policy;
existing replay receipt predicates and TLA progress checks remain authoritative.
GPU pixel qualification is not newly required by the observer, and enabled
wild-run FPS must not serve as an OFF-path performance baseline.

Initial coverage is receipt and resource-scoped replay waits plus backend
query queue acquisition. Global drains, queue-space holder attribution and
game/Wine wait stacks remain separately qualified surfaces; absent hooks
must not be treated as zero time.

## Companion replay ownership evidence

When both wait and CPU-supply traces are enabled, `ReplayLedger` must bind
each dequeued raw FIFO identity to the receipt ledger incarnation. A join
requires one process, overlapping windows, matching raw/owner identity and
paired replay start/end on the worker TID. Publication records identify the
actual queue sequence; membership in a replay interval is a caller-context
relation, not proof that every draw in the published Slot belongs to that raw.
Writer-slot and inflight-cap waits retain the production predicates and are
recorded before release and after reacquisition. Their elapsed durations are
subsets of replay wall time, never additive costs. Missing/clipped pairs or
sink overflow invalidate complete ownership attribution. The companion uses
calibrated native thread CPU; neither saved guest PCs nor stack occurrences
may be multiplied into exclusive game/Wine/PE CPU shares.

Publication reason and command count must use the actual sealed Slot. Query
Issue records carry object identity and flags before execution; scope-boundary
records carry old/new membership counts after successful publication. A query
attempt is not proof of successful completion. Completion timing must separate
encode, actual commit, completion-worker dequeue, observed completion, and
the completion frontier/inflight-credit update. Observed completion is a CPU
observation after `waitUntilCompleted`, not an exact GPU timestamp. Commit-to-
dequeue includes completion-worker FIFO delay; dequeue-to-observed includes
driver waiting and scheduling. Only same-sequence ordinary 1:1 submissions
may use these differences; shared CBs and missing endpoints stay unqualified.

The ordinary Slot encode callback may add bounded prefetch and backend
begin/end events with the same queue owner, source sequence and native thread
clock as the outer encode interval. Adjacent endpoints partition source/borrow
preparation, prefetch eligibility/work, context preparation, backend processing
and return/relock. Both wall and native CPU differences must be reported;
these are observed scopes including diagnostic overhead, not pure function
costs. Skipped prefetch still brackets the eligibility check. Missing phases
(older traces, Arena, session/lookahead paths or exceptions) must remain
unqualified, never zero-filled. Duplicate, reordered, cross-thread or invalid
clock endpoints must be rejected. No global counter delta may be attributed
to one source, and no per-draw diagnostic state is required.

The ordinary backend-begin event may bind the backend interface identity in
diagnostic storage. Framegraph encodeChunk entry/exit events must join through
that identity, source sequence, thread and enclosing backend interval, never
sequence alone. Dispositions distinguish planning bypass, activated replay plan
and natural-order return. The partition is planning/setup, encodeChunk call
and cleanup/return; planning/setup includes wrapper and observer work and is
not an exclusive optimizer timer. Invalid plans or exceptions without both
endpoints remain unqualified. Multiple calls, mismatched dispositions, threads
or clocks must not yield a successful partition. No runtime identity fields,
query semantics or scheduling policy may be added for this observation.

An optional framegraph pool binding may connect that call to encodeChunk body
endpoints through pool identity, sequence, TID and the enclosing encodeChunk
interval. Setup/partition, command body and finalize/return must partition the
call without per-command clocks. The command body includes dispatch, draws,
pass transitions and in-loop split work; it is not exclusive draw cost.
Finalize/return includes tail encoder closure, submission construction, local
destruction and autorelease cleanup. Missing/duplicate bindings or body
endpoints must not be presented as zero work. These are source-call scopes,
not instrumentation of child parallel workers or a GPU timing authority.

Optional command-body splitting is restricted to selected serial large sources.
Selection must be explicit in the body event. Paired DrawRun/non-draw command
scopes and split-policy scopes must be disjoint and nested in that source body,
using the same pool, sequence and native thread. Counts must cover one split
per executed command. The remaining wall/CPU interval is dispatch residual,
including tracing and timer overhead outside the scopes. DrawRun includes its
internal pass transitions; non-draw includes its helper/pass work. This split
does not independently attribute all pass work. Disabled, excluded, missing,
overlapping, unpaired or invalid-clock evidence must not yield zero-filled
costs. A same-image control is required before interpreting observer cost;
diagnostic timing alone cannot authorize a runtime policy change.

Optional encodeDraw splitting must be separately gated and marked by body
selection mask 0x2 (mask 0x1 retains command-split selection). Each pair must be
nested inside a classified DrawRun command on the same pool/source/thread.
The call scope includes encodeDraw argument evaluation and internal binding,
uniform and draw issue work; the parent remainder includes pass/preparation,
post-draw bookkeeping and observer overhead. It is not pure pass time.
Unpaired, overlapping, cross-parent, mismatched-command or invalid-clock
draw endpoints must be rejected. Missing enablement stays unqualified.
Parallel children remain excluded; two per-call events require a shorter
bounded window and an observer control before product-cost conclusions.

Source-local analysis without a blocking receipt must require explicit
`--source-only` selection. It selects the largest encode CPU among complete
ordinary sources, emits no query target, and must not claim blocking-wait
ownership or scene equivalence. It is mutually exclusive with `--query`.
Normal receipt analysis must reject missing matched receipts rather than
silently falling back to a source-local selection.

Optional serial parallel-candidate observation must remain restricted to the
bounded trace window and IdentitySerial execution. It must use the existing
pure sealed-pass producer with truthful query/capture/initializer facts and
diagnostic-owned scratch that never reaches parallel execution. Production
partition selection and counters must remain unchanged. Pool/source/thread
qualified begin/end and count/rejection endpoints join through the ordinary
framegraph pool binding. Missing/duplicate/cross-thread/nonmonotonic endpoints
must not yield a qualified result. Structural eligibility precedes executor
economics and binding checks. Whole-source CPU may bound candidate work but
must not be attributed to a subset of draws or divided by child count to claim
a speedup. Observer timing is intrusive, not a calibrated subtraction.

Resource rejection details must distinguish incomplete draw-entry reads,
incomplete attachment writes, and pass-wide read-union overflow. These detail
reasons overlap the retained `ResourceSetIncomplete` aggregate; each is counted
at most once per rejected pass, not once per draw or failed append. Existing
reason IDs and coarse production counters must remain stable. Detail attribution
must not widen capacities, bypass canonical identity proofs, or admit a pass.
Native counterexamples cover individually incomplete reads, complete individual
reads whose union overflows, and repeated identical reads whose union fits.
This is observation-only classification, not a stateful rendering refinement;
it introduces no new execution, ownership, ordering or GPU-pixel claim.

Optional resource-cardinality events report the maximum and sum of canonical
read identities over structurally accepted passes in a source. They exclude
rejected/overflowed passes and are not a complete workload capacity histogram.
Older transcripts without the event remain qualified for prior fields, but
resource cardinality is unknown, not zero. Duplicate or inconsistent size
events must fail analysis. This observation does not qualify execution.

The serial observer may additionally install a scoped, thread-local access
observer around the existing sealed-pass producer. Its storage and events must
not enter producer signatures, snapshots, Slots or certificates. The producer
must return the same verdict and snapshots with or without this observer.
Pass boundaries come from the producer, not a second replay grammar.

For every candidate interval it records canonical distinct read identities,
canonical read touches per DrawRun command, maximum commands using one identity,
adjacent-command repeated reads, and sealed/rejected/production-overflow flags.
These count commands, not DrawParams, bytes or GPU reads. The separate 4096-entry
diagnostic dictionary compares full identities on hash collision. Its own
exhaustion or failed canonicalization must mark results inexact/lower-bound;
neither may enlarge production admission or grant execution authority. Bounds,
event order, source/thread clocks and coarse overflow/eligibility counts must
agree before analysis qualifies the per-pass transcript. Missing historical
events are unknown, not zero resource cardinality. This diagnostic measures
structure with intrusive CPU/cache work, not observer-OFF performance.
