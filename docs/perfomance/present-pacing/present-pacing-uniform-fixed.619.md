---
domain: present-pacing
workload: STALKER H502/r5 original captured input
title: "H619 — fixed projection copy removed, stable CPU saving unproved"
type: experiment
status: bounded-negative-screen
updated: 2026-10-16
source: traces/stkcop-uniform-fixed-h619/{contract.md,candidate-contract.md,fixed.hpp,derivation.json,native.json,build.json,codegen.json,candidate-codegen.json,codegen-verified.json,preflight.json,timing-contract.md,timing-build.json,timing-runs.json,timing-analysis.json}
related: present-pacing-replay-cache-live.618.md; present-pacing-fixed-projection-integration.390.md; present-pacing-fixed-projection-validation.391.md; present-pacing-run-construction-removal.451.md
---

# H619 — a real redundant copy, not a demonstrated live saving

A private direct-destination writer removes the intermediate fixed-uniform
projection at both assignment sites in
`src/d3d9/core_draw.cpp`, `Device::cachedBaseDrawStateForSubmissionBatch`.
Native differential tests and original-input GPU effects pass. However, the
fixed captured-body CPU screen does not establish a stable saving: six of
eight blocks favor the candidate, but two reverse and the median contrast is
smaller than OFF/OFF variation. No product promotion or live FPS claim follows.

## Mechanism and correctness

Both canonical and H618 linked code retain stack gathering followed by a
2,468-byte memcpy and trailing clip-field stores into the final cache. The
fixed payload is 2,568 bytes; the full uniform payload is 10,840 bytes.
The candidate writes all twelve fixed fields directly into the existing cache.
It does not skip refreshes, reuse state, change generations, or alter hashes,
validity settlement, allocation, resource ownership or ordering.

Actual linked native and Unix images both inline the writer. Each changed
gate's candidate branch has five array memcpy calls instead of the reference
branch's six: the final 2,468-byte memcpy disappears, and scalar/matrix/clip
stores target the cache directly. Required field copies remain. The binary
contains both branches, so its total static memcpy-site count increases; that
count is not dynamic cost. `verify-codegen.py` checks both gates in both images
using Mach-O indirect-symbol targets, not misleading disassembler stub labels.
The candidate retains a cached gate and one-shot witness guard; no claim is
made about an unconditional production build's stack frame or timing.

This is a pure value transform between distinct live cache subobjects, not a
temporal reuse optimization. Native property tests are the applicable first
layer rather than a new TLA model. ARM64 and x86_64 each pass 4,104 cases:
seeded fields, default values and floating-point edge patterns including NaNs
and signed zero. The test uses the production semantic field comparator,
excludes Light padding and verifies the source remains unchanged. Final test
fixtures use valid boolean representations; archived `native-initial.*` files
are not the final qualification receipt.

Downstream fixed-payload matching in `src/dxmt9/dxmt9_backend_types.hpp`,
`drawUniformFixedPayloadMatches`, uses the separately computed unchanged hash
and member equality, not padding-sensitive object memcmp. The fixed writer
does not change those comparisons or constant-table handling.

The two preflight runs authenticate loaded images and preserve the original
source/output SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
670 created/released wrappers, seven mapped sources, normal restoration and
the 9/5/4/5 issue/poll/result/readiness transcript. The candidate one-shot
witness appears once with ON and never with OFF. Query validation uses the
production counter profile, not an actual-Metal occlusion oracle.

Unix image SHA-256:
`afaa37bdf3f4e3ebf0e1dd0baeb1ecea6ce4eec5b73c86767451aaf42ec2532f`.
Only a private copy of `core_draw.cpp.o` differs in the core archive. No
product source, shared class, Wine staging or runtime policy is changed.
H390/H391 removed a temporary in the queue comparison predicate; this trial
targets cache assignment instead. H451's run constructor is another distinct,
previously tested materialization site, not newly discovered here.

## Frozen CPU screen

The same image/input runs two warmups, eight OFF sham executions, then eight
OFF/ON/ON/OFF blocks: 32 measured executions, 16 per arm. There are no
replacements or extensions. All 42 runs pass the original effect/identity
checks. A separate post-run verifier re-reads logs and artifacts, reproduces
endpoint arithmetic and performs 4,116 hash checks.

Two calling-thread CPU reads bracket the armed callback through the closed
callback. That interval includes captured traversal, readback and cleanup,
but excludes earlier setup. It is **not pure Replay CPU, all-thread CPU, a
default-live frame or the pipeline critical path**. Blocked wall and another
thread's CPU are not attributed to this calling thread. No per-draw timing,
product performance counters or H584/H602/H617/H618 observer is enabled.

CPU contrasts below are OFF minus ON, milliseconds per captured interval.
Positive means less observed CPU with ON. Bounds cover endpoint read/4us
quantization uncertainty, not run-to-run variability or confidence intervals.

| ABBA block | Point contrast | Endpoint interval |
|---|---:|---:|
| 0 | +6.483 | +6.459 to +6.505 |
| 1 | -1.018 | -1.040 to -0.993 |
| 2 | +2.486 | +2.462 to +2.509 |
| 3 | -3.411 | -3.436 to -3.388 |
| 4 | +3.734 | +3.711 to +3.758 |
| 5 | +2.753 | +2.732 to +2.777 |
| 6 | +6.126 | +6.104 to +6.147 |
| 7 | +3.128 | +3.106 to +3.151 |

The median point contrast is +2.941ms. Four adjacent OFF/OFF contrasts are
+3.380, -6.753, -8.127 and +3.782ms; their maximum absolute endpoint bound is
8.155ms. The predeclared advancement screen requires all eight block lower
bounds positive and the median lower bound above that sham envelope. Both
conditions fail. This deliberately conservative screen is not a statistical
test and does not prove the intrinsic copy cost is zero.

## Decision

The redundant materialization is established and the bounded candidate is
correctness-qualified on this input. A robust captured-body CPU reduction,
live benefit and a global default-live removal budget are not established.
Keep the candidate private; do not repeat this population until favorable or
multiply copy bytes by historical call counts to claim saved live time.

H618's uniform cost envelope includes necessary construction and hashing as
well as this projection; it cannot all be assigned to the removed copy. A
future reconsideration needs a new discriminant that isolates actual cache
construction from cleanup/other captured-body work, not another identical
broad timing run. Global Replay/Encode dominance remains open.
