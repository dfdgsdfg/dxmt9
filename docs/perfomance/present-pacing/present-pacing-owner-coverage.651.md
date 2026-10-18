---
domain: present-pacing
workload: H650 frozen STALKER owner population; offline entry and policy audit
title: "H651 — bind the known API and conditional-worker surfaces to the population"
type: evidence
status: api-and-launch-policy-audited-global-coverage-open
updated: 2026-10-17
source: traces/stkcop-owner-coverage-h651/{audit.py,audit.json}; traces/stkcop-owner-coverage-h651/{O0,R0,R1,O1}-launcher-reconstruction.json; traces/stkcop-owner-coverage-h651/{i686,x86_64}-{baseline,diagnostic}-exports.txt; traces/stkcop-retirement-live-h650/verification.json
related: present-pacing-retirement-live.650.md; present-pacing-thread-envelope.642.md; present-pacing-root-union.628.md; present-pacing-scheduler-preflight.652.md
---

# H651 — configuration and entry evidence, not an absent-event argument

The offline audit rechecks4,210 hashes, including H650 raw verification,
H628's PE/native qualification and the relevant H639 frozen source inputs.
It performs no game run, DLL patch, staging, performance measurement or product
change.

Both original and diagnostic PE32/PE64 images export exactly the same12 names.
The retained H628 manifest and actual-object qualification cover320 C++ COM
slots plus6 validator slots across16 concrete tables. Thunks enclose the
original call, and successful interface outputs install the appropriate
concrete table; unknown/unreadable/multiple concrete identities flag incomplete
coverage. This checks the known public D3D9 surface, not every possible loader,
TLS or bridge-export entry.

H650 R0/R1 each has two attachment epochs on one native TID. Only the current
epoch is active; the old epoch has no recorded work. Both parsed maps conserve
roots/clock reads/checkpoints and have no coverage fault. There is no second
registered public D3D9 caller in either run.

## Conditional surfaces

| Surface | Source/configuration evidence |
|---|---|
| Parallel render children | Missing `DXMT9_RENDER_PARTITION_MODE` resolves to `IdentitySerial`; child dispatch requires `ExplicitParallel`. |
| Scheduling watchdog | `DXMT_PERF_COUNTERS=0`; threshold variable absent. Constructor returns before starting the sampler. |
| PE thread sampler | `DXMT9_PE_THREAD_SAMPLER` absent; start routine first checks the cached flag. |
| Vertex-position completion callback | Probe directory absent; configuration returns disabled before admitting the callback path. |
| Async/pre-acquire presenter | All three acquire selectors absent resolve to `Sync`. Both alternate launch sites are additionally H643-observed; roles9/10 are absent. |

The H650 runner strips inherited DXMT/H5/H6 configuration and records explicit
overrides. Its four receipts contain none of the seven conditional selectors.
The current catalogue, game launcher, common launcher and Sikarugir shim do not
introduce them. Four bounded executions of the actual common launcher function
substitute `/usr/bin/env` for Wine and the app, preserving the recorded inputs;
they retain perf counters0 and absent selectors. This invokes no game launcher
staging/config writes and creates no prefix.

The reconstruction is a **code-derived launch-policy check**, not a retroactive
dump of the engine's native/guest environment. The common launcher and Wine shim
were pinned by this audit, not by H650's original input manifest. Do not silently
upgrade this evidence into an in-process resolved-policy receipt or infer
disabled state merely from missing worker events.

## Remaining boundary

API inventory does not alone price DLL/TLS teardown or all platform-internal
asynchronous CPU after owned-block disposal. Observer-OFF transfer remains
separate. H650 still ranks all observed composed owners, not an exhaustive
global default path. H652 therefore tests an independent scheduler-interval
route to bound otherwise-unobserved native threads instead of adding more
per-draw instrumentation or unqualified game-DLL logging.
