---
type: spec
title: Source-qualified optimizer input fixture
description: Device-free diagnostic equivalence and CPU phase attribution
tags: [encode, framegraph, diagnostics]
---

# Optimizer input fixture

This is a diagnostic, not a renderer provider, Render Tape, GPU oracle or
resource-lifetime capability. It must not change optimizer selection, emitted
commands, attachment actions, resource ownership or completion order.

## Input and scope

Observe the actual graph immediately before the production `runOptimizer`
call, after alias resolution and any active-render seed. Own all values:
passes (all attachments, sample count, ranges, flags, state profile and actions),
resources (identity, residency, classifier flags and chronological accesses),
edges, DrawRefs, CommandRefs, frame ID and flush boundary. Do not use the
existing display-oriented DAG JSON: it omits optimizer-relevant fields.

Version `DXMT9FG1` admits the default pass-coalescing-only configuration.
Memoryless, DCE/lookahead and reorder inputs are not replayable by this schema.
Return-diagnostic selection is recorded separately and may be toggled only in
the offline comparison. Numeric identities are qualified by process, backend
and source sequence; they are not live object pointers in the offline consumer.
Alias resolution is already embodied in graph identities/accesses/edges. No
Pool or source payload is accessed by the standalone optimizer experiment.

All fields are explicitly encoded as unsigned decimal scalars; no padding,
native struct bytes or vector internals. Reject unknown version, invalid
enum/bool, overflow, invalid ranges/indices, truncation and trailing data.
Bound files to 8 MiB, passes to 4096, individual vectors to 65536, and decoded
scalars to 1,048,576. Require a lossless round trip before publication.

## Observer contract

Disabled cost is one cached boolean gate; no fixture object or copy exists in
the ordinary branch. Enabled collection owns the input before production
execution, runs the original optimizer exactly once, and exports up to four
inputs whose observed optimizer wall time exceeds 5ms (at least 128 commands).
Capture exceptions cannot skip/retry optimizer execution. File writes occur
after that execution; files are exclusively created, never overwritten.
Capture copies/serialization perturb scheduling: this run cannot qualify FPS.
The output directory must already exist. A missing/rejected export is not a
successful capture. Partial files/logged write failures cannot qualify evidence.

## Offline comparison

Use one executable and one authenticated input. Copy the original graph before
each trial, outside timers; exclude parsing, materialization and output
comparison from timing. Run two warmups per policy, followed by sixteen paired
OFF/ON trials with reversed order on alternate rounds. Measure the actual
`runOptimizer` and independently its default sequence `runLifetime`,
`runPassCoalesce`, `runLoadStore` using production functions. Coalescing includes
its internal post-merge lifetime refresh; do not count that work again.

Require every graph output (including ordered commands/draws, edges, flags,
resource accesses and load/store actions) to equal the unsplit OFF reference;
also require equal merge counts. Timers report both monotonic wall and native
thread CPU. Their fixed clock overhead affects very small phases. Diagnostic
counters may differ by design; they must not change graph decisions.

## Evidence limits

Exact graph equality is optimizer-output equivalence, not GPU pixel equality,
complete backend replay-plan validation, or a measured FPS gain. No scheduling
or rendering policy changes here, so new TLA transitions and GPU promotion are
not applicable. Existing optimizer/native tests remain required. Any later
algorithm change must follow the formal/refinement and GPU evidence gates.
