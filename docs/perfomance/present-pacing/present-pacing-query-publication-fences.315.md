---
domain: present-pacing
workload: STALKER CoP Day / D3D9 query conformance
title: "Published query END receipts and nonblocking completion polls"
type: evidence
status: default-off-candidate
updated: 2026-09-17
source: experiments/output/query-fences-r636-20260917.noindex/analysis.json; experiments/output/query-fences-r636-20260917.noindex/{off-final-x64,on-final-x64,on-delay-final-x64,on-delay-final-x86,on-hardened-x64,on-hardened-x86}.json; build/meson-logs/query-fences-r636-verified-native-tests.log; build/meson-logs/query-fences-r636-hardened-native-observer.log; build/meson-logs/query-fences-r636-tla.log
related: present-pacing-stalker-query-waits.314.md; overview.md
---

# Query publication and completion are separate operations

`DXMT9_ASYNC_QUERY_FENCES=1` implements a **default-off** candidate for
R-CORE-11.5(a). Query END captures an actual published queue prefix rather
than a core operation ordinal. Pending GetData(FLUSH) requests submission
progress and returns `S_FALSE`; only actual completion permits `S_OK`.
Readback/reset/teardown retain their blocking flush contracts. This is not a
claim that the complete PE GetData entry is wait-free: PE recorder settlement,
bridge replay drain, queue-lock acquisition and bounded END admission remain.

## Ownership and failure boundaries

END settles the compatibility draw batch, flushes pending initializer uploads
without waiting, publishes the ordinary writer and captures its queue
incarnation, published sequence and initializer event prefix under the queue
mutex. Empty ordinary prefixes remain valid only with a live incarnation.
Active Arena/direct build transactions reject this operation rather than
returning an invented or deferred receipt.

The query retains the receipt, issue ordinal and reset epoch. Reissue replaces
the receipt; reset invalidates previous epochs. No completion callback captures
or writes a public query object. A later unrelated source cannot extend an
already-issued fence. Pending FLUSH creates a persistent progress target that
wakes the encode coordinator and survives return from the API.

Initializer shared-event completion is joined with successful upload-command-
buffer completion. The opt-in tracker holds at most 64 submissions and fails
closed on overflow, allocation/event failure or observed GPU error. Ordinary
queue GPU errors are tracked separately from the reclamation waterline, since
retiring failed work does not authorize successful query results.

Publishing at END is a deliberately bounded first implementation. It can
increase source and command-buffer fragmentation; it is not automatically a
performance win. An END marker that shares a larger published source needs a
separate receipt/ordering design, not a predicted next sequence.

## Deterministic and actual-GPU evidence

| Layer | Result | Scope |
|---|---|---|
| Focused native tests | 10 pass | Core query lifecycle, real queue publication/completion/reclaim, persistent encode-CV wake, completion failure, scheduling/session/early-prefix and audits |
| Observer-enabled native lifecycle | Pass after a discovered shutdown fix | First attempt terminated because the lazy logger's mutex was destroyed before the ledger's final report; initializing the logger inside ledger construction fixes the static lifetime order. Original failure log is preserved. |
| Full TLC inventory | Pass, including four new expected failures | `QueryPublicationFence` ordinary-prefix safety/progress: early completion, dropped async request, stale issue and ignored failure mutations all reject |
| Wine x64 OFF and ON, no delay | Four cases each pass, zero skips | Public support/size checks and project-authored Clear/Draw/Event END/reissue/readback |
| Wine x64 ON, 200ms completion delay | Four cases pass, zero skips | Two issues observe 140 / 171 pending FLUSH polls before completion; exact clear and center-draw pixels match |
| Wine x86 ON, 200ms completion delay | Four cases pass, zero skips | Two issues observe 150 / 222 pending FLUSH polls; exact pixel probes match. Historical auxiliary filename remains `d3d9_queries_x64.exe`; staged receipt identifies the x86 build. |
| Final failure-hardened provider, Wine x64 and x86 ON with 200ms delay | Four cases each pass, zero skips | Repeated delayed polls (145 / 224 on x64; 150 / 136 on x86), reissue and exact pixel probes; native missing-initializer-event injection also stops ordinary submission before END |

The fixture calls no Present or readback to provide progress before the query
completes. It polls without FLUSH first, then with FLUSH, verifies pending output
storage is untouched, observes successful completion and only then reads the
render target. It repeats with the same query and a different draw color.
The oracle checks two meaningful pixels per issue, not full-frame equivalence.

The new model explores 1,742 distinct states (4,721 generated, depth 15) in
its bounded two-sequence/two-epoch domain. It excludes Metal result bytes,
initializer execution, C++ memory ordering and admission-capacity behavior.
Native exhaustive predicate products add the initializer frontier, but do not
prove the Metal event or error path itself.

## Query observer and attribution limits

`DXMT9_QUERY_OBSERVER=1` records query type, unique query/issue/reset identity,
FLUSH, HRESULT, core ordinal and authoritative queue/initializer receipt.
Five type-specific cumulative counters conserve polls = OK + pending + error.
Event output is bounded to 128 Issue and 384 poll rows, with omitted counts;
periodic summaries are bounded separately and a final summary is emitted.
This is a sampled event population, not a complete game lifecycle trace.

Elapsed time covers core GetData only, excluding PE flush and replay drain.
Issue rows describe replay disposition, not a previously returned PE recorder
HRESULT. Queue waterlines are sampled after the call and may have advanced
after an `S_FALSE` decision. The observer-off branch constructs no report,
reads no clock and acquires no diagnostic mutex. Observer-enabled FPS is not
a clean performance baseline.

## Matched-image STALKER Day diagnostic

One OFF then ON pair uses identical five staged images, source hashes and
driver; Sikarugir cx24.0.7, Day/High, `renderer_r2.5`, 1280x720 windowed,
perf profile, replay offload ON and CPU-ready Tape OFF. Other candidate gates
match H314. Both use Info logging, the query observer, per-frame/periodic
counters and drain-site attribution. No task-owned build/test overlaps either
run. Campaign receipts retain environment, host-start processes, image hashes,
appdata backups and benchmark artifacts. They verify source/images unchanged
through the pair and actual Wine-root staging.

| Observation | OFF | ON |
|---|---:|---:|
| Benchmark average FPS — diagnostic only | 11.976194 | 17.338490 |
| Benchmark frame rows | 1,694 | 2,430 |
| Encoded Presents | 1,783 | 2,519 |
| EVENT polls (all FLUSH) | 1,752 | 2,488 |
| OCCLUSION polls (all FLUSH) | 3,490 | 438,835 |
| OCCLUSION S_FALSE / S_OK | 0 / 3,490 | 435,134 / 3,701 |
| Core GetData elapsed, total | 63,253.546 ms | 1,051.857 ms |
| Bridge GetData elapsed, total | 64,634.786 ms | 3,692.327 ms |
| Queue sequence-completion wait, total | 62,521.186 ms / 2,518 waits | 0 ms / 0 waits |
| Command buffers / encoded Present | 6.552 | 7.897 |
| Legacy published sources / encoded Present | 2.412 | 2.818 |
| Replay elapsed / encoded Present | 19.371 ms | 22.145 ms |
| Encode elapsed / encoded Present | 23.014 ms | 26.236 ms |
| GPU command-buffer time sum / encoded Present | 11.641 ms | 11.576 ms |

All eight covered health counters are zero; no observer query errors occur.
The 73 sampled authoritative ON END rows join actual queue identity; for
example core ordinal 239 corresponds to queue prefix 32, not 239. Initializer
prefix 1 is also present in the game sample. No timestamp-family polls occur.
OFF rows are explicitly non-authoritative.

This supports the mechanism: completion waiting has left the application query
boundary, while the completion worker still waits for real GPU completion.
It is **not** removal of GPU synchronization or a sum-of-thread-times CPU
budget. Polling grows sharply and END publication increases source/CB count;
the replay/encode elapsed scopes per Present also grow. The 44.77% diagnostic
FPS difference is one ordered pair, not a promoted gain or a verified upper
bound. Frame populations differ and the counters overlap.

Both runs still report `unable to find onscreen window matching title:
S.T.A.L.K.E.R.`; their images are fallback captures. Visual/focus qualification
is **not established** by `status=pass`. Neither README FPS nor runtime defaults
are changed.

After this pair, failure-only hardening makes initializer rejection stop the
queue even when an ordinary encode flush detects it before Query END. The
preserved `measured-source.patch` and campaign image receipts identify the
measured revision; do not call the spots a performance measurement of the
later hardening revision. Final native and Wine checks qualify that revision
separately.

## Remaining promotion gates

- Visible STALKER correctness and matched observer-off performance/locality
  repetitions, including reversed order; no README/default promotion from
  diagnostic spots.
- Initializer-only GPU oracle, 64-entry tracker exhaustion and concrete Metal
  upload/GPU-error injection. Native dual-frontier tests do not close these.
- Active Arena/CPU-ready source integration beyond the explicit fail-closed
  candidate contract.
- Real occlusion visibility data and timestamp sampling: existing primitive-
  count/core-ordinal providers and constant frequency/disjoint data are unchanged.
