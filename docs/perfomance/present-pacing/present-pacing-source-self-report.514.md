---
domain: present-pacing
workload: STALKER CoP H502/r5 frozen late input
title: "Source self-report preserves the original oracle; live Replay join remains open"
type: evidence
status: offline-observer-oracle-passed-live-attribution-unresolved
updated: 2026-10-14
source: traces/stkcop-source-self-report-h514/{contract.md,observer.hpp,diagnostic.patch,build-receipt.json,buildoptions.json,control.cpp,analyze.py,test_analyze.py,parser-tests.log,run_oracle.py,oracle-receipt.json,oracle-on-analysis.json,coverage.py,coverage.json,verify.py,verification.json,restored-tests.log}
related: present-pacing-self-counter-boundary.513.md; present-pacing-cpu-counter-freshness.512.md; present-pacing-production-query-diagnostic.506.md; present-pacing-validation-live.509.md
---

# Validate the observer before interpreting owner costs

H514 adds a private, frozen diagnostic following H513's current-thread counter
qualification. It does not optimize the renderer or reopen H509's failed live
transfer. The objective remains identifying the largest default-path dxmt9
single-thread CPU owner and its correctness-qualified removable amount.

## Boundary and identity contract

The temporary observer reports self CPU, bracketed by Mach wall timestamps,
at the following boundaries:

| Scope | Begin | End | Excluded |
|---|---|---|---|
| Replay | Successful offload FIFO pop | Replay settlement, retention release and `markReplayDone` | Pop/dequeue waiting; worker epilogue |
| Ordinary Encode | Successful ready-source dequeue | Submission/post-commit callbacks or inline completion bookkeeping | Dequeue waiting; session/DCE/lookahead branches; other worker-loop CPU |

No per-draw clock or broad supply trace is enabled. The recorder has bounded
mmap storage, release-published rows, paired RAII endpoints and explicit loss /
failure counters. Binding rows associate `CommandQueue`, lifecycle controller
and queue domain. They do not infer ownership from equal pointer values.

An external controller arms the mapping only after matching PID and process
birth time against the live child. A deliberately incorrect birth tuple is
rejected before the valid request. Deactivation cannot truncate an admitted
pair. Analysis happens after child exit, rejecting partial rows, losses, failed
scopes, mismatched/duplicate endpoints, ambiguous bindings, thread changes,
stale source generations, and nonmonotone raw/sequence order. It also checks
the unchanged H513 CPU-versus-enclosing-wall condition; this is finite host
evidence, not a universal clock error bound.

The Encode begin query runs while the queue lock is held; the inline-completion
end can do so too. Its scheduling impact is not priced by correctness tests.
The observer is deliberately archived outside the shipping runtime, rather
than treating its disabled or enabled overhead as already acceptable.

## Fixed original-input OFF/ON oracle

Both runs use the unchanged H506 `source_probe.cpp`, one frozen diagnostic
binary set, the original H502/r5 bundle, serial provider policy and product
counters/supply tracing OFF. Loaded-image logs pin the archived native dylib
and Unix provider. The observer is absent in OFF and host-armed in ON.

| Check | OFF | ON |
|---|---:|---:|
| Exact captured/local source ordering | 7/7 | 7/7 |
| Query issues / polls | 9/9; 5/5 | 9/9; 5/5 |
| Query result / readiness checks | 4/4; 5/5 | 4/4; 5/5 |
| Resource objects created / released | 670/670 | 670/670 |
| Original source and output bytes | exact | exact |

Each 3,686,400-byte image retains SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
This is H506's production-query diagnostic (`gpu_oracle=0`) with actual Metal
color readback, not a new GPU occlusion-result oracle or complete content-
authority proof. No performance comparison is made from the two runs.

## Coverage finding: a source is not a raw ordinal

The ON trace has 19 valid rows: one binding and nine ordinary Encode pairs.
All nine have `rawOrdinal=0`; there are no offload Replay pairs. This prepared
provider executes replay through its synchronous driver, bypassing the live
offload worker. Zero observations therefore do **not** mean zero Replay CPU.

The H506 exact publication receipts separately map captured source indices 0--6
to local queue sequences 3--9. `coverage.json` joins those seven Encode pairs
to their captured source; sequences 1 and 2 remain outside that captured source
set. Local captured-source index, ring-slot index, queue sequence and raw
ordinal are distinct identities and are never substituted for each other.

Code agrees with the missing raw metadata: `publishReplaySourceBoundary` in
`src/dxmt9/dxmt9_command_queue.cpp` passes an empty compatibility publication
identity and explicitly checks `metadata.rawOrdinal == 0`.
`QueueLifecycleController::commitCurrentChunk` uses zero when no valid identity
was supplied. Do not fill it using the most recent raw, source index or sequence.
In the general runtime a compatibility source need not have one raw owner.

This closes the **offline Encode boundary/oracle** check only. The observer
still lacks a production Replay-to-Encode coverage proof, unmatched-boundary
accounting, live observer-OFF transfer, and Primary game/Wine/PE/synchronous-
Unix attribution. None of the scope CPU values is a default FPS or removable
CPU budget. A live campaign must retain raw-zero/unjoined sources and reject
unsupported worker paths, not silently treat either as zero-cost coverage.

## Verification and restoration

- ARM64 and Rosetta observer fixtures pass disabled-admission, request-close,
  exactly-once end, failure destructor, concurrent pair and capacity-loss checks.
- Eight parser tests cover valid joins, many sources per raw, unjoined work,
  malformed headers, pairing failures, binding reuse and stale generations.
- Five restored native tests pass: query snapshot, replay offload queue,
  Render Tape execution/provider V4, and Unix install-name audit.
- The three temporary production files are restored to their pre-probe hashes.
  A canonical Unix rebuild restores the native/provider images byte-for-byte
  to `41d46e45…` and `0bf898b6…`; full hashes are in `verification.json`.

No scheduler, lifetime, publication or rendering policy changes survive, so no
new scheduling refinement model is claimed or required for this diagnostic.
Existing dirty H501--H508 work is preserved. No Wine/game episode, shared-prefix
staging, commit or push was performed in H514. The diagnostic source patch and
images remain in the ignored evidence tree for reproducibility.
