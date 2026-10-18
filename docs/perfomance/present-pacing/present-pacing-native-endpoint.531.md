---
domain: present-pacing
workload: owned native and Wine console fixtures; no STALKER episode
title: "Native endpoint attachment reaches the actual Wine process without renderer relinking"
type: evidence
status: native-attach-fixture-qualified-live-transfer-open
updated: 2026-10-14
source: traces/stkcop-native-endpoint-h530/{contract.md,layout.h,observer.c,control.c,target.c,run_native.py,native-receipt.json,analyze_native.py,native-analysis.json,wine-contract.md,launch.c,run_wine.py,wine-receipt.json,wine-loader-contract.md,run_wine_loader.py,wine-loader-receipt.json}; traces/stkcop-native-attach-h531/{contract.md,native.c,attach.c,run.py,receipt.json,analyze.py,analysis.json}; src/d3d9/device_c_replay_offload.cpp; src/dxmt9/dxmt9_command_queue.cpp
related: present-pacing-quiescent-counter.529.md; present-pacing-root-wall-live.525.md; present-pacing-current-thread-owner.510.md
---

# Native control is needed for the leading dedicated workers

H529's guest-thread controller is insufficient coverage for the intended
comparison: `ReplayOffloadWorker::start` in
`src/d3d9/device_c_replay_offload.cpp` and the worker startup in
`src/dxmt9/dxmt9_command_queue.cpp` create Unix `std::thread`s. H510's guest
census does not supply guest control handles for Replay/Encode. Do not infer
that successful guest `SuspendThread` tests cover these native workers.

H530 implements a separate diagnostic library with one native observer thread,
a PID/birth-qualified mailbox, and `task_threads` enumeration in its own task.
`THREAD_IDENTIFIER_INFO` supplies both the numeric TID and the opaque libproc
handle directly, avoiding an inferred pthread layout. The host selects exact
TIDs; absent, duplicate and observer-self requests reject before suspension.
The helper retains the enumerated Mach rights through sampling and resume.

Successful suspension and synchronous control-state retrieval precede two CPU
reads. Resume occurs autonomously before publishing the result: there is no
host handshake while held. Product call sites, canonical binaries and Wine
security configuration are unchanged.

## Native preflight

ARM64 and Rosetta owned native workers each complete 128 rounds. Self-CPU reads
bracket each externally requested endpoint. All held counters are equal and
all 130,816 chronological sample pairs per architecture satisfy monotonicity
and the predeclared 2 us CPU/wall allowance. Each architecture rejects missing,
duplicate and observer-self targets. Single-worker holds span 0.230–0.422 ms
in these fixtures; that is measured fixture disturbance, not a live bound.

## Two failed loader attempts, then actual-process attachment

H530's first `DYLD_INSERT_LIBRARIES` launcher creates a mailbox only for the
initial Wine loader, not the PE target. A follow-up setting `WINELOADER` also
fails to propagate the observer. Both attempts reject the missing target file
before any endpoint request. The second attempt does not prove the selector
was honored. Original receipts and mailbox files are preserved; three service
files per attempt are recovered after target termination. No signing,
entitlement, permissions or loader-binary change is attempted.

H531 instead explicitly loads a diagnostic PE DLL/unixlib into an **owned
console fixture** using a same-architecture helper. This is target-process
mutation, not a read-only profiler: it allocates a path string and starts a
remote loader thread. The helper limits the target to `target529.exe`, resolves
`LoadLibraryW` through its containing module/RVA, compares entry bytes, waits
for the loader thread and verifies the module exists. Its path allocation is
freed only after completion; the module remains loaded until process exit.
The unchanged H530 observer runs from that unixlib. No product instructions,
resource state or canonical image are patched or relinked.

| H531 target | Rounds | Held CPU change | Hold duration | Maximum CPU minus enclosing wall |
|---|---:|---:|---:|---:|
| PE32 | 16 | 0 ns | 0.338–0.405 ms | 42 ns |
| PE64 | 16 | 0 ns | 0.331–0.355 ms | 83 ns |

Both target-process mailboxes match H518's actual native PID/birth/TID, each
with a five-thread census. Native handle identity remains stable; all 2,016
chronological sample pairs per architecture satisfy the frozen gates. These
Wine rounds control a guest-created worker via its native Mach port; native
pthread-only control is supplied by H530's separate native fixtures, not by
claiming the Wine fixture contains production Replay/Encode.

Both console fixtures exit naturally. Six temporary service files are recovered
by hash; no owned target/helper/Wine process remains. Canonical Replay/queue
sources and provider binaries match H529's recorded hashes. Documentation
source audit and whitespace validation are separate from these CPU checks.

## Remaining gates and decision

This closes an actual-process attachment obstacle, not the largest-owner or
removable-work objective. Before live use:

- Qualify multiple selected native workers and partial-failure cleanup; the
  current successful endpoint tests select only one worker.
- Audit first-use/lazy binding and target-lock independence of every operation
  executed while suspended. Owned CPU-loop tests do not prove arbitrary game
  thread suspension cannot deadlock.
- Establish live same-run Primary/Replay/Encode identities and scene endpoint
  coherence. The fixture-only attach target restriction is not yet widened.
- Measure observer-thread wake/attachment and endpoint perturbation with a
  predeclared A/A/control comparison. Byte-identical renderer images alone do
  not imply an unperturbed process.

Counter equality and chronological consistency are not an absolute freshness
theorem. H525 remains diagnostic-only; its root envelope cannot yet exclude
Primary on the default path. No STALKER run, FPS update, runtime optimization,
removal estimate, commit or push occurs in H530/H531.
