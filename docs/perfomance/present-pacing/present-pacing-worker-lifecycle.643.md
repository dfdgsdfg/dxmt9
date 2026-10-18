---
domain: present-pacing
workload: STALKER original captured input, private owned-worker lifecycle observer
title: "H643 — actual worker launch, callable retirement and join now share identity"
type: evidence
status: private-integration-and-original-capture-passed-live-coverage-open
updated: 2026-10-17
source: traces/stkcop-worker-lifecycle-h643/{contract.md,observer.hpp,parse.py,fixture.cpp,qualify.py,build.py,resume.py,finish.py,build.json,build-initial-failure.json,build-prelink-failure.json,build-correction.md,native-validator-correction.md,verify.py,verification.json}; traces/stkcop-worker-lifecycle-h643/native/results.json; traces/stkcop-worker-lifecycle-h643/native-qualified/results.json; traces/stkcop-worker-lifecycle-h643/oracle/{runs.json,O0.log,I0.log,N0.log,O1.log,I0.workers,N0.workers}
related: present-pacing-thread-envelope.642.md; present-pacing-snapshot-performance.641.md; present-pacing-snapshot-live.640.md; present-pacing-root-union.628.md
---

# H643 — lifecycle accounting reaches the real queue and compiler workers

H642's lifecycle observation is now connected to a coherent H640-derived
private Unix image. Four fixed fresh-process original-capture controls pass.
In each observer-ON process, all **11 exercised owned workers** have matched
launch, self-thread samples, callable retirement and successful join events.
There are no unjoined instances in these two captured-input processes.

This qualifies the integration, not a new live cost, global-largest owner or
removable-time result. H641's fixed live population and its conclusions are
unchanged. No Wine files are staged and no original game binary is modified.

## Instrumentation boundary

Only six private translation units change: Replay offload, command queue,
pipeline cache, shader archive, archive prewarm and Presenter. They compile
against H639's frozen coherent mirror headers. Other archive object members
remain byte-identical; Darwin archive symbol-index changes are recorded
separately. Class layouts, bridge ABI, scheduling policy and renderer commands
do not change. Install-name auditing passes after relinking.

The private `H643_TRACE` gate selects fixed mmap event rows and a bounded cold
thread registry. OFF uses ordinary `std::thread` construction and join directly.
ON assigns a monotonically allocated instance, records before creation, and
binds the actual native TID at owner entry. The H642 sentinel samples after
the original callable's captures are destroyed. The real parent-side join
records its endpoint only after `std::thread::join()` succeeds. No earlier
join, timer, forced wake, suspension or per-draw text output is introduced.

Auxiliary checkpoints retain H566's coarse placement: every 64 Finish/
Completion iterations and each compile/acquire iteration. Replay/Encode keep
H584's sparse endpoints. Thread naming/QoS setup is now inside the observed
callable lifetime. Nontrivial TLS and platform cleanup remain in the interval
between callable retirement and actual join, not silently charged as zero.

Native tests cover concurrent move-only tasks, nested launches, exactly-once
call/capture destruction, TLS destruction before join, and disabled mapping
absence on ARM64 and Rosetta. Twenty-six corrupt-stream controls reject schema,
publication, identity, ordering and self-counter violations. This is observer
lifecycle qualification, not exhaustive proof of every thread-construction
failure or scheduler behavior.

## Fixed original-capture population

| Arm | Snapshot sharing | Lifecycle observer | Result |
|---|---|---|---|
| O0 | Reference | OFF | Original pixels, production-counter query transcript and lifetimes match |
| I0 | Reference | ON | Same, plus 11 complete owned-worker lifecycles |
| N0 | Candidate | ON | Same, plus 11 complete owned-worker lifecycles |
| O1 | Reference | OFF | Original pixels, production-counter query transcript and lifetimes match |

Every arm preserves 670 created/released resources, seven actual/source
mappings, nine query issues, five polls, four results and five readiness
obligations. Source and output readback SHA-256 remain
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
N0 witnesses the expected 84 shared batches, 2,828 draws, 2,343 runs and 726
base owners. These queries use the existing production-counter diagnostic;
they are not a true GPU occlusion-result oracle.

Each ON process exercises one Encode, one Finish, one Completion, four pipeline
compile and four library compile workers. Independent raw-row reconstruction
checks **22 owners / 446 rows**, composes every lifetime with H642's conservative
bound reducer, and verifies that actual successful joins exclude subsequent
activity. Provenance checks cover 3,967 hashes. These whole-lifecycle fixture
bounds are not late-scene CPU milliseconds per frame.

## Preserved setup failures

The initial native validator expected nine events per worker rather than eight.
Both ARM64 processes had exited successfully; their exact original artifacts
are revalidated and reused, not replaced. The remaining x86_64 pair then runs.

Private source generation stops once at Presenter because a comment separates
the function entry from its first loop. Five successful objects are retained;
a function-qualified anchor generates only the remaining source. A subsequent
archive audit stops on `__.SYMDEF`, the expected changed symbol-index metadata.
The corrected audit still requires exactly the six intended object changes.
Both stopped receipts remain. No renderer process ran before these build guards
were resolved; there is no omitted failed performance arm.

## Remaining integration

H628's baseline PE32/PE64 hashes match the PE images used in H641, so its
qualified root-union observer has the correct baseline lineage for a subsequent
same-episode population. That is a binary identity result, not permission to
reuse H629's old timing bounds.

The captured-input harness does not exercise real Replay-offload, archive
load/save or WSI acquire threads. Their sites compile, but live paths still
need coverage. Foreign main/event/parallel callbacks and disposal also remain
outside this owned-thread observer. Long unjoined tails must retain the H642
upper bound or gain qualified retirement evidence; process exit must not be
rewritten into a fabricated successful join.

Next combine the matching PE-root union, sparse Replay/Encode samples, owned
lifecycle and foreign-thread accounting in the same late camera episode.
Default-transfer controls and complete coverage are still required before
asserting a global dxmt9 winner. This stage performs no performance promotion.
