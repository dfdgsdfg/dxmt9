---
domain: present-pacing
workload: STALKER CoP transaction-local prepare candidate, native and bounded-model evidence
title: "Transaction-local suffix prepare: safety candidate, no CPU saving yet"
type: evidence
status: candidate-not-promoted
updated: 2026-10-14
source: traces/stkcop-prefix-prepare-h536/{check_compile.py,compile-receipt.json,check_native.py,native-receipt.json,testlog.txt,check_model.py,model-receipt.json,model.log,model.foreign.counterexample.log,model.generation.counterexample.log,model.repeat.counterexample.log,model.terminal.counterexample.log,configure.log,tla.log,tla-interrupted.log,tla-rejected-model.log}
related: present-pacing-removal-scope.535.md; present-pacing-validation-authority.508.md; present-pacing-validation-live.509.md
---

# Remove only the transaction-owned prepare prefix

[H535](present-pacing-removal-scope.535.md) identifies repeated prefix visits,
not their removable CPU cost. This candidate leaves admission/reserve full
validation intact and skips the old prefix only at the same assembler's
prepare boundary. It does not reuse H508's rejected admission-to-reserve cache.

The queue checks its active build, owner thread, exact ticket and Writing
destination, derives live identity fields, then immediately mints/consumes a
private move-only `PrefixPrepareCapability` under the commit lock. Build-time
exclusion is owner-thread-pinned; the mutex is not continuously held between
reserve and commit. The suffix shares the original validator switch, checks
all newly added commands and their references, and keeps global SoA and exact
range-conservation checks. Every schema-owned checkpoint extent must survive.

Generic `prepare()` still checks the full Slot. The queue substitution is
compiled only with `DXMT9_EXPERIMENT_PREFIX_PREPARE=1`; this is a private
experiment flag, not a runtime option or a production promotion.

## Deterministic evidence

| Layer | Evidence | Limit |
|---|---|---|
| Native gate and real validator | 16 gate rows; 64 suffix fault combinations; 14 changed binding fields; 31 physical extent removals; move, repeated-consumption, terminal and same-address-generation cases | Finite value fixtures, not arbitrary memory corruption or thread scheduling |
| Actual assembler prefix | 16 append/cut/Clear combinations use real uniform interning and append, compare old validator-dependent fields, prepare the suffix and restore the prefix by rollback | Does not authorize a foreign writer or cross-transaction cache |
| Bounded ownership model | `DirectPrefixPrepare.tla`: two generations, ownership exclusion, suffix faults, extent loss, single consumption and terminal stability; four deliberate broken configurations produce their named invariant failures | Queue exclusion is a reviewed premise; no C++ memory-model or pixel proof |
| Source binding | Private factory placement, existing queue guards, live identity fields, immediate consumption and shared gate spellings; four source mutations fail the audit | Structural drift detector, not a concurrency proof |
| Build | SDK-qualified native linked build and four focused tests pass; default and experimental queue branches pass syntax-only compilation on arm64 and x86_64 | Candidate provider is not yet linked or run under Wine |

The final full `verify_tla.sh` run exits zero: 69 safety configurations,
6 progress configurations and 167 required counterexamples, 242 configurations
in total. The new normal model explores 43 distinct states; this bounded count
is not an exhaustive state count for the concrete queue implementation.

An initial model incorrectly constrained, rather than updated, its `sound` and
`fresh` Boolean witnesses. The required foreign-writer counterexample did not
fail, so that run was rejected. Parenthesized conjunction assignments correct
the model; all four expected counterexamples now execute. The rejected log is
retained. A separate earlier full-suite attempt was interrupted by editing the
runner while it was running and is also not passing evidence.

Xcode's absolute compiler initially failed Meson's standard-library detection
(`libc++` and `stdio.h` discovery). Explicit process-local `SDKROOT` restored
configuration; no build directory was deleted or compiler option changed.
The focused build also reports two pre-existing missing-braces warnings in
`direct_slot_provisioning_spec.cpp`; the new fixture builds without warnings.

The x86_64 staged build outputs remain the pre-experiment images, with SHA-256
`41d46e45beb30a25717b97517464994b7af28fd0e56d365bdd15dd7dd8b917d6`
for `libdxmt9_native.dylib` and
`0bf898b61b9da5240936e0a676b1e5d734337f35429c4e60e53a04bca71904e6`
for `winemetal_dxmt9.so`. No game run or Wine-root staging was performed.

## Remaining acceptance gates

1. Link fresh matched reference/candidate provider images with configuration
   parity and only the candidate selection changed; do not compare a newly
   compiled candidate with historical H508 images.
2. Replay the same H502/r5 input under H506's production-counter query profile,
   requiring exact source/query correspondence and source/output pixels.
3. Disable per-validation probes in **both** timing arms. The candidate bypasses
   the full validator's probe hook; timing it against an instrumented reference
   would wrongly count removed observer work as product savings.
4. Use fixed A/A and alternating A/B–B/A whole-Replay CPU comparisons before
   any live transfer. Removed command visits are not removable milliseconds.
5. Qualify canonical live CPU ownership and actual removal separately. Neither
   the largest default dxmt9 thread nor a default CPU saving is established here.
