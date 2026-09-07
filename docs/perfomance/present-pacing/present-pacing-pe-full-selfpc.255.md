---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Complete bounded PE self-PC reporting"
type: evidence
status: native-linked-verified-runtime-pending
updated: 2026-09-07
source: experiments/output/pe-full-selfpc-20260907/analysis; experiments/output/pe-current-attribution-20260907/analysis
related: present-pacing-pe-constant-bitcompare.254.md; ../../../specs/d3d9/recorder/spec.md
---

# Complete bounded PE self-PC reporting

## Decision and contract

At baseline `242da3cf`, H254's 05/06 GT2 samples report only 344/1,459 and
221/922 PE samples in their top-32 self-PC rows. Zero histogram overflow does
not mean the printed profile covers all recorded samples. The diagnostic
already retains up to 3,072 distinct 64-byte buckets, so recover those before
selecting another optimization from a minority of the distribution.

`DXMT9_PE_THREAD_SAMPLER_FULL_SELFPC=1`, with the sampler enabled, gives the
cold report call a nothrow, caller-owned 49,152-byte row buffer. It copies live
rows while holding the existing sampler lock and sorts/formats after release.
The sampler never holds this lock, allocates or logs while the target thread
is suspended. The suspension/aggregation loop, renderer policy, hot PE state
and default 32-row output selection are unchanged. Full reporting adds cold
allocation, sorting and logging cost; it is not a performance mode.

Each new group starts with `selfpc_coverage=1` and ends with a summary:
reported + omitted + histogram-overflow samples = independently captured
self-module samples. The self count is **not** reconstructed from the three
terms being checked. The footer records row/storage bounds and `top`, `full`
or allocation-failed `full_fallback` mode. Completeness requires zero omitted
and overflow samples and no allocation fallback. The parser rejects missing
footers, malformed totals and fallback instead of silently treating the prior
group as current. Legacy logs remain readable with unverified coverage.

This closes reporting truncation, not sample-time uncertainty, module-table
late loads, 64-byte symbol-boundary ambiguity, missing final tails, or the
difference between stopped-thread PC shares and exclusive CPU time. No runtime
CPU saving or README recovery follows merely from better observation.

## Verification

Four canonical builds pass. Native sampler, diagnostics, ABI-source and parser
targets pass (four unique Meson targets); the parser suite has 15 tests,
including full 3,072-row and malformed/truncated fixtures. The x86_64 sampler,
diagnostics and install-name targets pass 3/3. Native coverage pins full/short/
null capture, exact load cap, repeats, overflow versus omission, stable full
sorting and top-32 parity. An initial invocation used a nonexistent ABI test
name; the corrected `dxmt9-pe-device-abi-source-spec` passes. Logs retain both.

Both linked PE images preserve exports, vtable/RTTI owners, build options and
the 12 manifest hot-entry codegen metrics. Diagnostic code increases PE32 by
13,824 bytes and PE64 by 10,752; unchanged hot metrics are not byte-identical
images or an FPS-neutrality proof. The Unix provider and both bridges are
byte-identical to H254. No rendering/queue transition changed; no new TLA run
or Win32 scheduler proof is claimed.

Candidate PE32 SHA-256:
`272ec2c1c66267c0ee77fb021fd72a114ff260bea48eb49f8432fc7f5efc60d6`.
PE64 SHA-256:
`e54d84b423d37750761d190ff91abe06319a88afaa21da79895792be31a538d0`.

Fresh actual 05/06 GT2 full-distribution sampling is the next gate. Sampler
runs must not contribute FPS values to the README.
