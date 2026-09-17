---
domain: present-pacing
workload: STALKER CoP Day / D3D9 query conformance
title: "Deferred Query END groups and an unstable absolute-performance comparison"
type: evidence
status: bounded-candidate
updated: 2026-09-17
source: experiments/output/query-fences-r639-20260917.noindex; build/meson-logs/query-r639-drain-guard.txt; build/meson-logs/query-r639-hardened-tla.log; build/meson-logs/query-r639-final-wine-x64.log; build/meson-logs/query-r639-final-wine-x86.log
related: present-pacing-query-default-promotion.316.md; overview.md
---

# Deferred END groups; no default or FPS promotion

`DXMT9_COALESCE_QUERY_ENDS=1` is a new **default-OFF** candidate. It requires
the default-ON asynchronous query-fence policy. The PE wire ABI is unchanged.
Several ENDs may share one ordinary writer; only that writer's successful
publication binds their receipts to an actual queue sequence. No future
sequence is predicted and no GPU completion is synthesized.

The old/current absolute-performance comparison is inconclusive. Both images
slow dramatically in later repetitions. This does not isolate either a code
regression or a host-specific cause, and the prior 18–19 FPS level does not
reproduce. README remains unchanged.

## Absolute comparison, before the candidate

R639 freezes all five original `09421d37` images and rebuilds `4248c3d0` in an
isolated worktree. Full Meson option tables are compared. The only accepted
metadata differences are removed `b_bitcode=False` and new
`build.cpp_importstd='false'`; both are disabled. A rebuilt historic image is
not asserted to be byte-identical to the original historic build.

| Order | Image | Average FPS | Frame rows |
|---|---|---:|---:|
| old1 | `4248c3d0` | 12.250623 | 2,030 |
| current1 | `09421d37` | 11.052355 | 2,064 |
| current2 | `09421d37` | 3.981589 | 1,618 |
| old2 | `4248c3d0` | 3.319464 | 1,425 |

The first adjacent pair is slower on current; the reversed late pair is
faster on current. Same-image deterioration dominates that comparison. Do not
average the four rows into a causal implementation delta. Host snapshots show
background CPU activity, but do not establish exclusive scheduling, thermal,
memory-pressure or application-phase attribution.

All four runs complete with zero runner failures, verified staged-image hashes,
no capture error and separately inspected actual scene captures. They use
Sikarugir cx24.0.7, Day/High, `renderer_r2.5`, 1280×720 windowed, perf/Warn,
offloaded replay ON and CPU-ready Tape OFF. Both images explicitly enable
asynchronous fences. Segmented transport, inline constant delta, precise
uniform invalidation, lock-scope refinement, opaque-depth index optimization,
observers, perf counters, frame sampling and completion delay are OFF.
No task-owned build, test or profiler overlaps a wild performance run.

## Ownership and progress

| Stage | Authority / behavior |
|---|---|
| END admission | Under the queue mutex, capture queue incarnation, source index and full 64-bit source/storage generations, plus the initializer prefix. No speculative sequence. |
| Shared group | One `EndMarker` allocation per nonempty writer, reference-counted by its Slot and receipts; repeated ENDs share it. Empty/eager receipts allocate no marker. |
| Publication | After successful `sealAndPublish`, bind once to the exact actual sequence, before encode visibility. Existing Present, capacity, readback and ordered-control cuts remain. |
| Non-FLUSH poll | An unbound marker returns Pending even when all existing completion watermarks are zero or complete. |
| FLUSH poll | Publish the exact owning writer, then request its actual sequence. Actual completion remains a later observation; no GPU wait. |
| Capacity | Defer only below the publication cap. Later sources cannot bypass the unpublished writer. Full-cap END uses the existing eager Issue/backpressure path and requests prior submission. |
| Completion/reclaim | Both ordinary and initializer success are required. Receipt ownership survives Slot reuse; reissue/reset cannot revive an older query issue. |
| Failure | Allocation, invalid ownership, publication rejection and terminal queue failure never yield a successful receipt. |

This is not a zero-runtime-cost type change: shared groups add one allocation
per writer and refcounts per END. Even OFF retains null marker fields in Query
and Slot storage and a cached candidate check; it is not a zero-size wrapper.
It avoids per-query callbacks into public
objects and avoids a future-sequence reservation. It may conservatively wait
for a suffix inside the same published chunk, without moving the END's query
result bytes or initializer prefix. Real Metal occlusion/timestamp result
providers are still outside this work.

Arena admission publishes an existing ordinary writer before admitting a new
Arena source. Direct continuation either appends to that same writer or
publishes it before rotation. The CPU-ready ordered-control release boundary
is retained; some lanes therefore already publish before END and cannot gain
from deferred grouping.

## Evidence gates

| Layer | Result / scope |
|---|---|
| Builds | Full native, x86_64 Unix and both PE architectures pass; canonical Unix install-name fixup and three install-name/perf-counter audits pass. |
| Native predicates/lifecycle | Thirteen final focused specs/audits pass, plus 20/20 repeated coalescing-queue processes (`query-r639-repeat.txt`). Full-width generation mismatch, exactly-once binding, unbound polling, source reclaim/reuse, terminal failure, real marker allocation failure and public reissue/reset are covered. |
| Actual bounded capacity | Fill 30 actual sources, admit/FLUSH the last available deferred position, then test the 32nd writer's eager END under the real 31-source cap. A producer-pressure wake and actual submit/completion release the wait; all sources reclaim. |
| TLA | 62 normal + 4 progress + 144 expected-counterexample configurations pass. `QueryEndCoalescing` explores 62,150 distinct states and depth 22; unbound success, stale binding and dropped submission pressure are rejected. |
| Metal completion + bytes | Independent GPU SharedEvent blocks execution. Two ordered clears share one marker/source, pending polls do not wait for GPU, and eventual completion requires no Present/readback to rescue progress. Readback afterward checks all 256 final green bytes. Existing initializer-only oracle checks 768 bytes over three gated uploads. API validation enabled. |
| Wine x64/x86 | Four cases pass per architecture, no skip/fail/timeout. With Tape OFF and coalescing ON, 200ms delayed completion produces Pending before success; actual Clear/UP+VB draw pixels and public sizes are checked. Split diagnostics prove deferred admission and FLUSH publication executed. |

`stopAdmission()` intentionally allows an already admitted writer to publish;
it is not a publication-failure injection. That distinction is now covered by
the queue test. Explicit terminal stop remains fail-closed. TLA does not prove
C++ memory ordering, unbounded histories, actual driver faults or game pixels.
The bounded Metal oracle is not a whole-game exact-frame comparison.

## Split diagnostic

`DXMT9_QUERY_SPLIT_OBSERVER=1` counts empty/eager ENDs, new/shared deferred
groups, FLUSH publication, capacity fallback, marker binding, ordinary source
publication reasons and actual completion-bearing tail command buffers. Its first 128 events carry
queue/sequence identity; final totals are a separate full census. No per-draw
clock or payload walk is introduced. This observer's `command_buffer` category
does not count mid-chunk sub-CB commits or initializer buffers. Compare it with
the separate `sub_command_buffers` commit and `command_buffers` creation
counters. None is an exclusive attribution of why each CB split; they are not
interchangeable with publication counts.

Both OFF/ON diagnostics complete and verify the same candidate images.
`P` below is the full-run `submit_present` population, including startup;
benchmark frame rows are a separate scene population.

| Measure | Coalescing OFF | Coalescing ON |
|---|---:|---:|
| Presents (`P`) | 2,154 | 1,581 |
| END calls represented by group counters | 5,725 | 4,979 |
| Deferred new / shared END | 0 / 0 | 1,614 / 3,365 |
| Eager END / FLUSH publications | 5,725 / 0 | 0 / 490 |
| Ordinary source publications | 7,879 | 2,071 |
| Sources / P | 3.658 | 1.310 |
| Completion-bearing tail CB submissions | 7,879 | 2,071 |
| Mid-chunk sub-CB commits | 10,112 | 6,149 |
| Total CB creations / P | 8.353 | 5.200 |
| Submitted draws / P | 2,780.579 | 2,879.208 |
| Queue sequence waits / GPU CB errors | 0 / 0 | 0 / 0 |
| Diagnostic benchmark average FPS | 13.863977 | 9.039410 |

The complete source-reason histograms are exactly Present + END for OFF and
Present + QueryPoll for ON. All 1,614 deferred markers bind. There is no
candidate full-cap eager fallback in this ON run (the OFF reference records
14 capacity-pressure encounters). The creation counter also includes buffers
outside the ordinary completion tracker; do not silently assign its residual
to END.

Grouping and lower source/CB density are demonstrated, but diagnostic FPS
falls. Different scene sampling and severe earlier temporal drift prohibit an
exclusive causal speed claim in either direction. Fewer cuts can also delay
upstream publication and the completion of earlier queries by adding suffix
work; that is a plausible batching/latency tradeoff, not measured attribution.
The captured OFF image shows loading, so it does not establish scene
correctness. ON has a separately inspected actual terrain/building scene;
neither is a whole-frame exact comparison.

The candidate's five frozen images, tracked patch and untracked source files
are archived under `images/candidate/`. Same-candidate OFF/ON diagnostics are
kept separate from observer-OFF FPS evidence. Wild/locality qualification and
stable repeated non-regression remain required before enabling the candidate
by default.

## Observer-OFF reverse-order pair

The same five candidate images then run with all observers/counters OFF in the
reverse order, ON then OFF. Both runs complete, verify their images and retain
inspected actual scenes. Results are 9.590875 FPS / 1,522 frame rows for ON and
10.733294 FPS / 2,128 frame rows for OFF: ON is about 10.6% lower in this pair.
This is not a stable universal regression percentage or a calibrated A/A
noise floor. It is sufficient to reject promotion on the claim that fewer
source/CB cuts alone imply an FPS improvement.

The next useful discriminator is a qualified END-to-publication timeline,
pending GetData/poll cost and replay/encode supply gaps on that same timeline.
If delayed publication is the owner, a bounded early-publication policy should
be tested against eager END, retaining actual binding and completion semantics.
Do not weaken completion or reuse a predicted sequence to recover locality.

## Final failure-path refinement

After the measured candidate, an additional post-seal stale-owner injection
exposes a native invariant violation: Tape is Ready but its control is still
Writing with a nonzero sequence. The hardened branch preserves the actual
published Pending ownership/frontier, then fail-stops without binding the
failed marker or exposing work to encode. It neither rolls back published
storage nor advances successful completion. `FailAfterSeal` models this case.
Normal queue shutdown can drain Ready work, so the final dequeue guard also
checks an attached marker's exact identity/sequence before admitting that
source. The extended native test proves the failed source stays retained and
cannot dequeue or advance completion; valid prior work keeps its normal drain
contract. This adds a nullable marker check to normal dequeue, not just code
on an exceptional branch.

The measured frozen candidate predates this failure/ownership hardening.
Final hardening is archived under `images/final/` and qualified by separate
build/native/model/GPU/Wine evidence;
the FPS rows are not relabelled as measurements of the final image. The
candidate remains OFF and no README or absolute-performance recovery claim
is made.

## Preserved evidence

The R639 directory retains per-run receipts, appdata backups, actual benchmark
results, launch logs, staged-image hashes and scene captures. The initial
failing focused-test log remains under `build/meson-logs/query-r639-focused.log`;
its corrected reruns have separate final/details logs. The later failing
post-seal injection is retained in `query-r639-final.txt`; the drain-counterexample
is in `query-r639-retention.txt`. The corrected final run is in
`query-r639-drain-guard.txt`. The old comparison worktree and all
five frozen image populations (including intermediate hardening) are retained. No failed run is silently
reclassified as passing, and no FPS table is updated from diagnostic numbers.
