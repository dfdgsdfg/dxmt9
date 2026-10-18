---
domain: present-pacing
workload: H646 same-episode owner-stream composition
title: "H647 — sparse self samples refine worker lifecycles without a new run"
type: evidence
status: offline-composition-verified-retirement-open
updated: 2026-10-17
source: traces/stkcop-owner-composition-h647/{compose.py,verify.py,test_compose.py,tests.json,verification.json}; traces/stkcop-owner-lifecycle-live-h646/batch.json
related: present-pacing-owner-lifecycle-live.646.md; present-pacing-thread-envelope.642.md; present-pacing-thread-retirement.648.md
---

# H647 — refine existing bounds instead of collecting the same endpoints again

H646's initial owner composition omitted the separately recorded H584 samples
when bounding H643 Replay/Encode lifetimes. Their unjoined lifecycles therefore
received vacuous full-wall uppers, despite valid same-episode CPU endpoints.
This was a postprocessing omission, not missing runtime data.

The new private analyzer reads the frozen raw streams and writes a separate
report. No H646 script, raw file or prior analysis is changed; no game is run.

## Identity and time contract

1. Both complete stream parsers validate the same native PID and process birth.
2. H584 validates one queue generation, stable owner/controller identities,
   separate Replay/Encode TIDs and both request epochs.
3. Each role must match exactly one H643 owner by native TID and role. Samples
   must follow its entry and precede body return when one exists. Ambiguous
   instances, cancellation and cross-role matches fail closed.
4. H643 has no queue-domain field. This is a thread-execution identity match,
   not an invented equality between H584 queue IDs and H643 owner indices.
5. The actual H640 sparse header equals the H602 header byte-for-byte; both it
   and H643 use native self `PROC_PIDTHREADID64INFO` user+system CPU and scaled
   `mach_absolute_time`. Frozen build/image inputs are rechecked.
6. Merge endpoints in wall order; reject overlapping apertures, backwards CPU,
   impossible deltas and invalid join authority. Keep the qualified 2µs
   self-endpoint allowance. Never query remote CPU or infer a zero exit tail.

Intersect the valid original and refined owner uppers; sum only for the same
TID and cap by its wall interval. Cross-thread sums are not latency. Body-return
and payload-retirement events still do not authorize a terminal boundary.

## Verification and results

Three test groups pass: 17 rejection mutations, preservation of unknown
unjoined suffixes, and 9,216 finite CPU-schedule/window comparisons against
direct integration. The population verifier rechecks all eight runs, image and
script hashes, raw samples, camera/request endpoints, catalogue/staging evidence,
14-file restorations and temporary-service recovery. It recomputes the original
owner reports before applying the refinement.

The refined lifecycle uppers equal the independently computed sparse uppers
for all eight observed Replay/Encode instances:

| Arm | Replay upper ms/frame | Encode upper ms/frame |
|---|---:|---:|
| R0 | 33.665790 | 32.379784 |
| C0 | 35.129685 | 34.281330 |
| C1 | 33.733152 | 34.914501 |
| R1 | 40.364050 | 38.553443 |

ArchiveLoad/ArchiveSave full-wall uncertainty remains unchanged. This result
does not certify exhaustive thread coverage, conditional-worker exclusion or
observer-OFF transfer, and does not establish a new saving. It isolates the
next concrete gap: prove actual retirement of the two unjoined archive owners.
