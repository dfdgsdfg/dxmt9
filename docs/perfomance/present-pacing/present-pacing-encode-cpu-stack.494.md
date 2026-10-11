---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, x86 release Time Profiler feasibility
title: "Native Encode CPU ancestry exposes repeated disabled counter gates"
type: evidence
status: cpu-stack-discovery-no-default-owner-or-saving
updated: 2026-10-11
source: traces/stkcop-encode-cpu-stack-h494/{contract.md,run.py,receipt.json,record.log,run.txt,toc.xml,time-sample.xml,time-profile.xml,export-profile.log,analyze.py,analysis-v2.json,encode-observations-v2.json,bind-counter-instructions.txt}; traces/stkcop-encode-cpu-stack-h494/run/{supply.csv,caller.json,analysis.json}
related: present-pacing-v4-pending-lookup.493.md; present-pacing-normal-source-cost.486.md; present-pacing-default-unwind-feasibility.467.md; present-pacing-root-game.477.md
---

# A product-path candidate from actual CPU ancestry

One bounded Time Profiler launch successfully resolves native Encode ancestry
on the unchanged original Normal input. Unlike H467's failed live Primary
ancestry experiment, this is a native provider runner with no PE/game execution.
It does not reopen that closed method or resolve mixed Primary ownership.

The actionable discovery is `countTextureBind → countBaseStateBind` with
`DXMT_PERF_COUNTERS=0`. Release disassembly confirms ten dynamic calls to
`perf::enabled()` on the disabled path of a single binding recorder. The
immutable flag is rechecked separately for all ten fields, including zero
increments. This is real unnecessary default-path work, not capture-only
identity qualification. The sample does not price its total cost.

## Run and provenance

The frozen H493 candidate runner and runtime image hashes are checked before
and after execution. The profiler launches PID 95316 with the H481 manifest
and explicit frozen library paths. Target output records both loaded image
paths. Recording is bounded to 30 seconds and ends normally after 15.557 s
when the target exits. No runtime source, policy or input changes during this
measurement; no Wine staging or UI interaction occurs.

The original source/output SHA-256 remains
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
Normal policy, 16 restored surfaces, 12 aliases, 779 created/released wrappers
and all three original source cuts pass. The strict provider reducer retains
92 raws, 208 accepted validators and 84,587 command visits. Metal skill doctor
and readback checks use Xcode 27, Apple M1 / Metal 4. All artifacts are retained.

## CPU samples, not a removable-cost estimate

TOC discovery precedes raw and modeled export. Both exports succeed; no retry
or custom Rosetta unwinder is used. The analyzer resolves XML references and
joins each modeled Encode observation to a raw **Running / Timer Fired** row
by timestamp and TID. The unique Encode TID 891040 joins the same process's
CPU-supply trace and original source reducer. This is not an opaque libproc
handle equated numerically with a Darwin TID.

All 62 modeled Encode observations are Running / Timer Fired. Disjoint known
ancestry, classified draw first, then prefetch, then other encodeChunk:

| Ancestry | Observations |
|---|---:|
| Prefetch, including cold shader translation | 34 |
| encodeDraw | 9 |
| Other encodeChunk | 12 |
| Other or unknown | 7 |

Unknown translated PCs remain unknown. These observations include setup and
cold compilation and are not joined to source time windows. No sample count
is multiplied by CPU milliseconds or interpreted as a confidence interval,
exclusive function cost, removable fraction or default-game rank.

Separately, coarse source-qualified CPU for the large 556-command source is
56.942 ms Encode, including 35.873 ms prefetch; its command body is 19.206 ms
and planning/setup is 1.156 ms. These are nested, observer-inclusive scopes.
The prefetch result repeats H486's fresh-cache limitation, not a steady-state
removal candidate. The numbers are not added across concurrent threads.

## Product candidates and observer contamination

The nine encodeDraw-ancestry observations include resource surface lookup,
sampler lookup, transient upload, Metal driver resource allocation, the binding
counter and an active `__os_signpost_emit_impl` call. They do not establish
which is largest; several leaves remain untranslated addresses.

`encodeDraw` unconditionally enters its per-draw signpost interval. The
profiler run contains active signpost work even though product counters are
OFF. This is not proof of the same emission cost when no Instruments consumer
exists, and H494 must not be called observer-OFF. Future numerical A/B runs
must run without Time Profiler; do not subtract this sample as a calibrated
profiler tax.

The narrow next product candidate is the three binding recorder functions in
`dxmt9_perf_counters.cpp`: `countBaseStateBind`, `countBaseStateBindSkip` and
`countBaseStateBindSkipExtended`. One outer immutable enable decision can
replace repeated per-field decisions; existing `addEnabledNonZero` can avoid
zero-valued relaxed atomic increments when enabled. Rendering, resource
lifetime and queue transitions need not change. Required evidence is exact
OFF/ON counter mapping and concurrent accumulation, release instruction
verification, original GPU oracles and an unprofiled same-input body comparison.
No largest default-thread owner, FPS change or removable milliseconds are
claimed by this discovery.
