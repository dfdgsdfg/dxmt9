---
domain: present-pacing
workload: native thread-retirement witness preflight
title: "H648 — retained Mach thread-right probe survives callable and TLS tails"
type: evidence
status: native-mechanism-qualified-production-authority-open
updated: 2026-10-17
source: traces/stkcop-thread-retirement-h648/{probe.cpp,run.py,reference.py}; traces/stkcop-thread-retirement-h648/native/{verification.json,arm64.jsonl,x86_64.jsonl}; traces/stkcop-thread-retirement-h648/reference/{manifest.json,thread.c,ipc_tt.c,ipc_kobject.c}
related: present-pacing-owner-composition.647.md; present-pacing-worker-lifecycle.643.md; present-pacing-thread-envelope.642.md
---

# H648 — observe termination without introducing an early product join

A private native fixture tests retaining the exact `mach_thread_self()` send
right, validating its `THREAD_IDENTIFIER_INFO.thread_id` against the registering
thread's native TID, and querying `mach_port_type` later. It never samples
remote CPU. No product/runtime binaries or Wine files are changed.

On this machine (macOS 27.0.1, Darwin 27.0.0, xnu-13432.1.9), eight ARM64 and
eight Rosetta x86_64 cases pass. Each stops the worker inside its body, callable
destructor and TLS destructor before allowing termination. Half the cases are
detached; the others are joined only after the terminal observation.

| Control | Result |
|---|---|
| Body/callable/TLS held-open observations | 192 send-right-present observations; no premature dead-name |
| Natural termination before joining | 16 retained rights become dead names after the TLS completion marker |
| Churn while retaining the original right | 256 intervening worker creations; original right remains a dead name |
| Missing/released token | Unknown, never accepted as retirement |
| Build | ARM64/x86_64, C++20/O2, warnings-as-errors |

The fixture's `Live` label means only **send right present**, not a proof that
the worker can still execute. A failed information query is not retirement
evidence. The fixture's dead-name result is a normal-exit observation, not yet
a generally authorized terminal event for the H642 envelope.

## Kernel-reference check and limitation

Immutable Apple XNU reference commit
`f6217f891ac0bb64f3d375211650a4c1ff8ca1ea` is retained with URL/hash provenance.
This is an external reference, not asserted to match the installed kernel.

- `thread_terminate_self` disables thread IPC early, then performs more kernel
  cleanup before its final block. An invalid `thread_info` result is therefore
  not a sufficient CPU-retirement boundary.
- `ipc_kobject_disable` clears the object pointer; it is distinct from port
  deallocation. The normal `thread_deallocate_complete` path requires
  `TH_TERMINATE2` before `ipc_thread_terminate` deallocates thread ports.
- `ipc_thread_reset` can also destroy an old control port while creating a new
  one. Consequently **dead name alone is not a universal thread-death proof**.
  The reference limits this reset path to security-context exec/corpsify;
  retained-right provenance, image/process continuity and exclusion of reset
  must be explicit before adopting this mechanism.

These checks reject the tempting shortcut of treating every failed port query
or payload retirement as zero remaining CPU. The retained-right mechanism is
promising for ordinary archive-worker exit, but requires a bounded terminal
capability contract, reset/invalid-token counterexamples, coherent observer
integration and runtime controls. H646's unknown archive tails are not silently
relabelled using this separate fixture.

No new live performance result, default promotion or whole-program CPU ranking
is established here. H641's removable Replay evidence remains separate.
