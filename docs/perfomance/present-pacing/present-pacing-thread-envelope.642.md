---
domain: present-pacing
workload: Native ARM64 and Rosetta worker-lifecycle fixtures; STALKER ownership coverage audit
title: "H642 — worker-body return is not callable or TLS retirement"
type: evidence
status: lifecycle-envelope-preflight-passed-live-integration-open
updated: 2026-10-17
source: traces/stkcop-thread-envelope-h642/{contract.md,envelope.py,observed_callable.hpp,native.cpp,test_envelope.py,model-tests.json,audit_sources.py,source-audit.json,verify.py,verification.json,initial-failure.md,native-verification.json,arm64-build.log}; traces/stkcop-thread-envelope-h642/qualified/{native-verification.json,arm64.jsonl,x86_64.jsonl}; traces/stkcop-snapshot-performance-h641/verification.json
related: present-pacing-snapshot-performance.641.md; present-pacing-owned-worker-preflight.566.md; present-pacing-foreign-owner-preflight.579.md; present-pacing-owner-union-live.629.md
---

# H642 — retain teardown uncertainty instead of treating it as zero

A private lifecycle reducer and native callable observer pass qualification.
They close a measurement-design gap, **not the global STALKER owner result**.
There is no new game run, product optimization, live CPU saving, default
promotion or modification to the frozen H641 population.

H566 measures worker bodies. H579 explicitly leaves closure disposal outside
foreign callback bodies. Neither a body-return marker nor a later callable-
destruction marker proves that the thread has finished: nontrivial thread-local
destructors and platform cleanup can run afterward. A concrete native fixture
demonstrates both gaps instead of assuming their cost negligible.

## Source audit and coverage consequences

The source inventory pins 18 reviewed markers in both the current tree and
the corresponding H639/H640 private sources. Broad thread/callback and TLS
search output is retained, but marker presence is not dynamic reachability or
proof that no additional entry surface exists.

| Surface | Coverage requirement |
|---|---|
| `dxmt9_command_queue.cpp` / `makeQueueWorkerLoop` | Include naming/QoS setup before the loop and captured `std::function` destruction afterward. |
| `CompileQueue::run` | Task destruction is inside the body; thread-entry closure and retirement are separate. |
| `Archive::triggerSave`, `beginAsyncFullLoad` | Captured strings/functions outlive the observed lambda body. |
| Replay/Presenter worker entries | Bind launch, native TID, checkpoints and actual retirement to one instance. |
| `execute_on_main`, shared-event listener | Account foreign-thread execution, nesting and callbacks that may outlive the initiating request. Block disposal is not body CPU. |
| `ParallelPassMetalBackend::emitChildren` | `dispatch_apply_f` is an additional conditional foreign-worker surface. Exclude it only with authenticated effective-policy evidence. |
| Watchdog, PE sampler, vertex probe | Establish that diagnostic entry surfaces are disabled; source defaults alone are not a live configuration receipt. |
| `DrawSubmitScratch`, `ReplayScratchArena`, shader-source TLS | Vector/string destruction prevents treating all TLS as scalar or already covered by body exit. |

Apple/Wine internal asynchronous work is not enumerated by a project-source
search. Any global claim still needs an explicit attribution/coverage argument
for that boundary. This inventory is not such a certificate.

## Conservative lifecycle upper bound

The reducer accepts process/birth/instance identity, a timestamp before thread
creation, owner-thread CPU samples, and optionally a timestamp **after a
successful join**. It splits the possible lifetime into startup, sample-pair
intervals and teardown. Each sample pair contributes at most the smaller of
its CPU delta plus endpoint allowances and its wall overlap with the selected
outer camera interval. Unmeasured startup/teardown contributes clipped wall
capacity, not zero. Overlapping endpoint apertures may overcount conservatively.

Missing join evidence extends the possible tail through the target interval.
No self samples leaves the whole possible lifetime charged. Successive worker
instances on one TID are summed, not reduced to their maximum; the result is
capped by single-thread wall capacity. An unidentified instance remains visible
as unresolved ownership. A retired body alone cannot exclude later activity.

This avoids remote-counter freshness assumptions rejected by H512/H565. It can
also be deliberately loose: a long interval between body exit and eventual join
may prevent ranking. A future integration must then obtain qualified actual
retirement evidence, not invent a fixed teardown allowance.

## Qualification

- Five model tests pass, including **675,840** finite CPU schedule/target/
  sample-layout comparisons with sample aperture and rounding. Negative cases
  reject foreign identity, TID drift, bad lifecycle ordering, impossible self
  deltas and body-return masquerading as successful join.
- ARM64 and x86_64/Rosetta each execute two original and two observed callable
  cases. All eight run the original callable, payload destructor and TLS
  destructor exactly once; payload/TLS destruction stays on the worker.
- The private observer samples entry, body return and after callable member
  destruction. Deliberate roughly 12ms payload destruction appears between the
  last two samples. Deliberate roughly 12ms TLS destruction occurs **after** the
  final self sample and remains charged by the teardown wall envelope. These
  synthetic durations are not real renderer costs or observer-overhead prices.
- Successful join excludes later activity; body exit and an absent join do not.
  Independent reconstruction checks raw native rows and bounds, 56 hashes and
  six unchanged H641 runtime images.

The initial ARM64 compile failed because the new private header omitted a
namespace-closing brace. Its log/receipt remain unchanged. Corrected sources
and the fixed successful population use `qualified/`; no failed timing run was
replaced. No renderer ABI/class layout changed and no Wine files were staged.

## Next decision

Keep H641's established Replay reduction and worker ordering separate from
global attribution. Integrate same-episode PE-root union, sparse Replay/Encode
endpoints, auxiliary lifecycle bounds and foreign callback accounting on one
coherent private image, with detailed per-draw observers still off. Persistent
workers can be bounded by enclosing self samples; cold workers need genuine
retirement or an explicitly retained tail bound. Repeating game-DLL logging
alone would not discharge these native ownership gaps.
