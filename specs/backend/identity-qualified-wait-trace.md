---
type: "Spec"
title: "Identity-qualified blocking wait trace"
description: "Bounded diagnostic evidence for CPU replay and polling waits."
tags: [backend, queries, diagnostics]
---

# Identity-qualified wait trace (R-BACK-WAIT-1)

The diagnostic must preserve production wait predicates, lock ordering,
submission obligations and failure results. It is not an optimization or a
new synchronization authority. Disabled entry points must perform at most a
cached gate, without clocks, diagnostic payload construction or TLS access.

The trace selects an inclusive process-global encoded-Present window. It must
record every condition-variable wait entered in that window, subject to an
explicit fixed capacity, rather than sample every Nth poll. Mutex acquisition
intervals above a declared threshold may also be recorded; elapsed lock
acquisition does not by itself prove blocking or identify a lock holder.

Each replay row must identify a process-local ledger incarnation, target,
captured raw ordinal, initial/final admitted and completed frontiers, terminal
classification and calling thread. The target address alone is not identity.
Ledger address reuse must receive a new incarnation. Query context adds the
immutable query identity; backend queue acquisition additionally carries its
queue and ordinary END prefix. Observed completion frontiers are evidence of
predicate settlement, not a claim about which notification woke the OS thread.

Mutex acquisition and condition-variable wait/reacquisition intervals must
be separate. A wait count means a pending predicate entered the CV operation;
spurious wakeups and scheduler delay remain included. No reported interval
may be labelled exclusive CPU, GPU time or removable FPS cost.

Storage must be bounded and diagnostic-owned, with no allocation, log output
or sink mutex in the measured locked interval. Export occurs outside the
window. Missing window closure, truncated output, capacity overflow, unknown
identity and in-flight writers must make complete attribution unqualified.
Existing evidence files must not be overwritten. Cross-window waits must
retain their entry/exit frames and be excluded from interior totals.

Native tests must cover ready/no-wait, genuinely pending predicates, terminal
failure, supersession, owner reuse, bounds, output completion and disabled
behavior. This observation-only change adds no rendering/refinement policy;
existing replay receipt predicates and TLA progress checks remain authoritative.
GPU pixel qualification is not newly required by the observer, and enabled
wild-run FPS must not serve as an OFF-path performance baseline.

Initial coverage is receipt and resource-scoped replay waits plus backend
query queue acquisition. Global drains, queue-space holder attribution and
game/Wine wait stacks remain separately qualified surfaces; absent hooks
must not be treated as zero time.
