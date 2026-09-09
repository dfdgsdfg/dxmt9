---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Bounded queue/arena lock-scope candidate"
type: evidence
status: native-verified-candidate
updated: 2026-09-09
source: specs/backend/lock-scope-refinement.md; specs/verification/tla/LockScopeRefinement.tla; tests/native/backend/buffer_use_batch_spec.cpp; tests/native/backend/range_upload_lock_scope_spec.cpp; tests/native/backend/queue_lock_scope_refinement_spec.cpp; build/meson-logs/lock-scope-reference.txt; build/meson-logs/lock-scope-candidate.txt; build/meson-logs/lock-scope-tla.txt
related: present-pacing-lock-contention-audit.307.md; present-pacing-upload-arena-locks.306.md
---

# Bounded queue/arena lock-scope candidate

This is an implementation/native-evidence overlay on `1f3f0358` and H306,
not a new GT2 result. `DXMT9_LOCK_SCOPE_REFINEMENT=1` enables all three bounded
changes below; absence retains the reference path. It remains default-off.

| Change | Removed or relocated work | Preserved boundary |
|---|---|---|
| DEFAULT+DYNAMIC range upload | Outer queue acquisition | Synchronous bytes and generation lookup/mutation under arena-exclusive lock |
| Captured-buffer stamping | Up to 64 exact handle/backing stamps per arena acquisition | Fixed final seq; flush before logical/current/other-resource effects and publication |
| Replay preparation | Immutable entry/payload counts and adjacent state compatibility before queue acquisition | Post-acquire destination check, live snapshot, exact Slot capacity/reserve/append/publish |

The batch is fixed-size and call-local. A/B/A deduplicates only the same
generation-qualified handle and concrete backing; a missing captured backing
still advances the logical record exactly as the scalar reference does.
Current-backing marks, other resource effects and command boundaries are
flush fences. Overflow flushes and retries the new stamp, never drops it.
Publication batching applies to the ChunkSlot resource walk; the separate
Arena-source mark walk is unchanged.

Preparation borrows immutable input for the synchronous call, not queue
storage. Live snapshot synthesis can enlarge payloads: changed spans are
recounted under the queue mutex. Slot reserve/copy/commit is not moved outside
the lock by this candidate. Full uploads and managed rotation remain unchanged.

## Native evidence

The complete native build passes. Thirteen focused tests pass, including
reference/candidate process-isolated cases; six existing resource/compatibility/
lifecycle tests also pass with the candidate enabled. Named logs are under
`build/meson-logs/lock-scope-{reference,candidate}.txt`.

- Actual range-upload entry with a real Shared Metal buffer preserves prefix,
  patch and suffix bytes, bumps revision once, and rejects an invalid extent
  without mutation. ON completes while a separate thread owns the queue mutex.
  Neither range case skipped on this host. This is buffer-byte evidence, not a
  rendered pixel or overlapping in-flight GPU-read oracle.
- Actual Pool pins exact A/B/missing backing, lower-seq max behavior, stale
  generation reuse and 64/65 capacity handling. Three distinct captured keys
  use one arena `UpdateMany` acquisition.
- Actual queue tests compare A/A/B/A final-Slot semantics with a scalar Slot
  reference, preserve owned input bytes, and reject an invalid suffix without
  partial append. A live dynamic binding synthesizes three snapshot payloads
  with correct capacity and active/stale watermarks. H306 counters pin the
  captured stamps to three scalar acquisitions OFF versus one batch ON;
  the separate logical batch remains one acquisition in both modes.
- The shared post-acquire destination predicate has all eight stop/arena/direct
  truth-table rows. This is a manual model/code correspondence, not generated
  whole-program isomorphism.

## Formal boundary and outstanding gates

`LockScopeRefinement` and four negative configurations cover the bounded
backing/flush/preparation obligations. Full `dxmt9-verify-tla` passes in
260.93 seconds: 52 normal, two progress and 113 expected-counterexample
configurations. The new normal model visits 24,297 distinct states; wrong
backing, missing prepublish flush, unretained input and stale validation
controls fail their intended invariants. The log is
`build/meson-logs/lock-scope-tla.txt`. Generation reuse/reclaim remains owned
by the existing lifetime models plus actual Pool tests, not by constant
generation fields in this small model. It does not prove OS mutex fairness,
arbitrary MT caller behavior, allocator latency or GPU pixels.

No runtime products were installed and no wild/performance run was made for
this overlay. Cross-runtime build/install qualification, deterministic draw
parity and supervised 05/06 GT2 correctness are still required, followed by
matched H306 acquisition/hold/count measurements and low-observer CPU/FPS.
H306's old times are not saved milliseconds: queue and arena scopes overlap,
bounded linear dedup adds CPU work, and fewer acquisitions may lengthen an
individual arena hold. Keep exact duplicate rates, hold tails and supply
progress in the next comparison. README values are unchanged.
