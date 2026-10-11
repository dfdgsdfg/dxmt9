---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, x86 release finite A/A and A/B
title: "Pending-draw lookup removes 8.29 ms of capture-inclusive replay CPU"
type: evidence
status: accepted-capture-only-no-default-transfer
updated: 2026-10-11
source: traces/stkcop-v4-pending-lookup-h493/{contract.md,candidate.patch,build_runner.py,candidate-build.json,run.py,matrix-images.json,rows.jsonl,summary.json,build-refinement.log,tests-refinement.log,build-final.log,tests-final.log}; traces/stkcop-v4-pending-lookup-h493/{image-A,image-B}.stderr
related: present-pacing-provider-stack.490.md; present-pacing-slot-validation.491.md; present-pacing-validation-reuse.492.md; present-pacing-root-game.477.md
---

# A measurable capture cost, not a product-path saving

V4 DrawRun identity binding previously searched the complete record table
twice for every locator: once for admission and once to bind it. It already
required the result to equal the next pending-draw index. Reusing that index,
under an explicit uniqueness witness, saves a paired median **8.290 ms** of
planned-replay CPU over this complete 92-raw input. All eight pairs favor the
candidate and the saving exceeds the maximum **2.200 ms** A/A difference.
All 30 executions preserve the original GPU/source oracles.

This is qualification work inside observed replay. It is not an 8.29 ms game
frame saving, a default FPS improvement, or a pure production Replay price.
H492's rejected uniform-validation runtime change remains reverted.

## Reference semantics and bounded refinement

`device_c_render_tape_runtime_identity_v4.cpp`, `State::bindEmission`, now
uses `pendingDraws_[consumedDraws_ + offset]` only for DrawRun and only while
appended `(eventOrdinal, firstRecord)` keys remain strictly ordered. Bounds
and the supplied full locator are checked before existing kind, pending-order,
unbound writer/physical-command and attempt-prefix predicates. Every locator
validates before any binding changes; both passes execute under the same mutex.
Pending consumption still occurs after the complete binding pass.

`setCut` can supply a duplicate or nonmonotone event cursor. Blindly using the
pending index could then accept a later duplicate that the original first-match
scan rejects. The private ordering witness only transitions true to false;
unordered histories and non-draw commands retain the reference scan. Record
keys are append-only, and rollback truncates. A false witness stays false after
rollback, conservatively retaining the scan. No new allocation, public ABI,
rendering policy, lifecycle transition, or persistent pointer is introduced.

The native test checks 49 locator pairs, including state locators, missing
keys, duplicates, wrong order and a rejected second locator with no partial
first binding. Public altered-cut cases cover duplicate/nonmonotone keys.
A bounded exhaustive checker covers depth-four append/truncate histories over
three keys, calling the actual production `onRecordBegin` and `restoreRecords`
transitions, then comparing pending consumption with the original linear
predicate. Existing real transaction rollback/retry, coordinator and retirement
tests remain. This is a lookup refinement under the existing mutex; no new
interleaving/liveness protocol requires a separate TLA model. GPU/source
verification remains an independent integration layer, not a replacement for
the bounded check.

## Frozen comparison

The baseline is the archived H491 runner and two images retained by H492;
their hashes match H491. The candidate uses the same H486 source and compiler/
link command with the x86 release build. Both arms' OFF dyld logs verify loaded
image paths and every run rechecks image hashes. Base source is `9fa9dfe6`
plus `candidate.patch`. No build overlaps measurement and no Wine staging or
live-game configuration is changed.

The predeclared matrix has two OFF image/oracle runs, two trace warmups per arm,
four A/A pairs and eight alternating AB/BA pairs. Each execution creates a
fresh device; warmups do not establish warm per-device PSO caches. The primary
gate is a paired median saving greater than maximum absolute A/A difference
and at least seven of eight positive pairs. No failures, outliers or unfavorable
orderings are removed, and no additional favorable runs are added.

| CPU scope, complete 92-raw interval | Baseline median | Candidate median | Paired median A−B | Max A/A difference | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Planned replay, including V4 and trace | 51.513 ms | 42.943 ms | 8.290 ms | 2.200 ms | 8 / 8 |
| Unchanged full-Slot validator control | 3.215 ms | 3.357 ms | −0.119 ms | 0.446 ms | 0 / 8 |

The primary gate passes. The validator control is slightly slower in every
pair, below its A/A noise envelope; it is not a claimed saving or a proof of
timing equivalence. Independent arm medians and the median of paired savings
are different statistics. The roughly 16% saving is relative to the observed
planned-replay envelope only; it cannot be transferred to default game Replay.

## Verification and remaining boundary

All 30 runs pass exact original source/output SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`,
unchanged Normal policy, 16 restored surfaces, 12 aliases, balanced 779 wrappers
and three original source cuts. Each of the 28 trace runs retains 92 raw calls,
208 accepted validators and 84,587 successful command-header visits. The full
x86 build and focused V3/V4 identity, provider, observation and install-name
tests pass; the ARM build is not refreshed here. The Metal skill's doctor and
original readback workflow verifies Xcode 27, Apple M1 / Metal 4. No gputrace
or new game capture is required. Artifacts are retained.

The optimization is retained in the capture observer, with its contract in
`specs/experiments/harness/replay/spec.md`. Physical-append verification,
publication coverage and published-source origin construction still scan the
record table; they are not removed or charged to ordinary product savings.
Their remaining cost should be separated before calling the new 42.943 ms
envelope pure replay. Original all-resource content authority remains unknown
and timing qualification false. H477's mixed Primary attribution and the
largest default-tail dxmt9 CPU owner/removal budget remain unresolved.
