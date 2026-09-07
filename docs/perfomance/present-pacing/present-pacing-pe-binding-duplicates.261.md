---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact binding duplicate validation reuse has no observed opportunity"
type: evidence
status: candidate-rejected-for-current-samples
updated: 2026-09-08
source: experiments/output/pe-binding-distribution-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-binding-distribution-8edb415d-gt2-n127-r1-20260908
related: present-pacing-pe-validated-constant-range.260.md; present-pacing-pe-identity-distribution.248.md; ../../../specs/d3d9/recorder/spec.md
---

# Compact binding duplicate validation reuse has no observed opportunity

## Decision

Do not add a per-attempt duplicate physical-pin validation cache for the current
GT2 workloads. The sampled prepared streams contain 103,369 pinned binding
occurrences across 39,570 compact preparations, with **zero duplicate pinned
occurrences**. All seven binding families have zero duplicates. This is a
negative economics result, not a production optimization or FPS improvement.
`makeQualifiedBinding`, `pinPreparedQualifiedBinding`, first-retain ordering,
generation checks and rollback remain unchanged.

The samples do not prove that every unsampled record or another workload is
duplicate-free. Sampling is deterministic, not randomized; no statistical
confidence interval or universal zero-work claim follows. The evidence is
enough to reject adding a cache whose intended opportunity was not observed.

## Observation contract and bounded verification

Commit `8edb415d` extends R-CORE-REC-7.39's existing cold phase-split observer.
Successful compact preparation supplies seven contiguous active ranges in
production emission order. One record-wide ordinal set separates null/unbound
rows, first pinned identities and duplicate pinned occurrences, including
stream/index-buffer and render-target/depth-stencil repeats. Per-family sums,
duplicate-containing record counts, maxima and per-record 0/1/2/3+ buckets
conserve rows and pinned identities. Schema-3 identity lines remain compatible;
new schema-1 binding lines explicitly name sampled prepared-stream scope.

Malformed ranges, inactive-prefix access, invalid local/mapping ordinals,
wire mismatch or oversized prefixes make the observation unmeasured. They
do not change production append acceptance. A later acquisition/settlement
failure may prevent some prepared occurrences from executing: these counts
are not committed pin-helper calls. CapacityPre retries are distinct attempts.
Generic and constant records do not manufacture compact binding samples.

Native tests exercise all 2,187 one-row-per-family null/A/B combinations,
cross-kind identity separation, cross-family repeats, empty and repeated
streams, histogram/max conservation, real max-family distinct/mixed fixtures,
malformed witnesses, and compact observer ON/OFF canonical-byte/retain equality.
Final native focused tests pass 9/9; x86_64 focused tests pass 5/5. All four
canonical builds pass. `native-final.log` and `x86_64-final.log` retain results.
This is cold value-classifier instrumentation: no new concurrency, resource
lifetime or pixel algorithm is introduced, and no new TLA/GPU proof is claimed.

## Linked boundary and artifact identity

The new distribution exists only in cold observer state and its outlined
observed adapter, never in a production prepared witness or hot signature.
When diagnostics are disabled, `makePeDiagnosticsState` returns null. When a
diagnostic owner exists, its inline phase statistics grow by 1,200 bytes; this
is not a claim of zero memory growth in diagnostic runs. Both PE DLLs grow by
4,096 bytes.

`main-hot-metrics.json` compares all 12 manifest bodies per PE width: raw body
byte counts, decoded instruction counts and direct-call counts are unchanged.
The independent `audit_unobserved_owner_paths.*` is explicitly only a
normalized entry-prefix comparison through the first textual return; it is
not a whole-CFG proof and normalizes immediates. Main's `full-owner-metrics.json`
also compares full symbol spans for six selected owner functions per width.
Those metrics match except PE32's writer span (+1 decoded byte/-1 decoded
instruction): the span includes relocated jump-table data disassembled as
instructions, so those differences must not be read as executed work. Neither
audit proves all callee semantics or runtime cost. Source review confirms that
the reconstruction call remains inside `tryAppendOwnedRecordObserved`'s sampled
successful-preparation branch, with unobserved paths unchanged.

Frozen final PE32 SHA-256:
`8b84e8b498cb20e32afce13d4b30e3db35decc5a57798c17c9fbaf5fc537fbab`;
PE64: `a80bc1fc15088fbe282584ef7f7d56c871d634496f1065535d7e7d18152c3336`.
Both actual runs match these five-artifact staging receipts; the Unix provider
and both bridge DLLs retain H260's hashes. The analysis root preserves baseline
and candidate PEs, scripts and reports.

## Actual PE distribution

Sequential named GT2 runs use Sikarugir cx-24.0.7, 1024x768 for 05 and
1280x720 for 06. Perf/Info, frame sampling, phase split and N=127 decimation
are enabled; full recorder stats, copy ledger, module map, thread sampler,
inline constants, segmented transport and index reorder are OFF. Offload is
ON. No build, test suite or other benchmark runs during the scenes. These
diagnostic runs are not FPS baselines or host-isolation evidence.

| Product | Verified cumulative groups | Observed Presents | Sampled compact preparations | Binding rows | Pinned / first | Null/unbound | Duplicates |
|---|---:|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 29 | 1,740 | 23,237 | 60,601 | 60,227 | 374 | 0 |
| 06 GT2 | 35 | 2,100 | 16,333 | 44,335 | 43,142 | 1,193 | 0 |

| Family | 05 pinned occurrences | 06 pinned occurrences |
|---|---:|---:|
| Textures | 4,564 | 5,449 |
| Streams | 22,862 | 16,691 |
| Shaders | 12,849 | 7,123 |
| Vertex inputs | 6,689 | 4,201 |
| Index buffers | 12,937 | 9,389 |
| Render targets | 224 | 171 |
| Depth stencils | 102 | 118 |

`qualify_distribution.py` checks every group, all seven families, histogram
conservation, monotonic cumulative counts, sample/preparation denominators,
runtime/profile and staging identity. `distributions-qualified.json` is the
combined authoritative report. All reported preparation rejections and
unmeasured failures are zero; the eight reported chunk/GPU/pipeline/no-pipeline
error counters are zero. 05 exits normally. 06's permitted timeout closes the
completed result GUI; `.3dr` emission is not required for these counts.

Do not sum cumulative groups. 05's final perf counter reports 1,780 Presents
and the frame log ends at frame 1,779, beyond the last PE report at 1,740.
06's last perf counter snapshot remains at 2,100 while its frame log reaches
2,134. Those tails are not covered by the PE distribution; the last reported
error snapshot is not proof about an unreported tail. 06's cumulative prefix
also includes startup/loading activity. H248's occurrence-minus-unique count
was only a null-plus-duplicate upper bound; here the difference is entirely
null/unbound rows in the observed samples.

## Remaining performance direction

The latest detailed-observer-OFF timings remain
[H260](present-pacing-pe-validated-constant-range.260.md), including its requested
same-binary 06 repeat. This observer-only wave does not update README or prove
reduced CPU consumption, measured gain or repeatable recovery.

Production retainer callback inspection finds synchronous typed forwarding to
retainer bookkeeping and atomic resource AddRef implementations; it finds no
owner-prefix reset/rollback callback. That supports a bounded future lexical
proof, not arbitrary Wine reentry or removal of physical-pin validation.
The new distribution closes the duplicate-cache candidate, not all binding
costs. A useful next investigation is whether **first-occurrence** immutable
source/identity facts can be carried once from admission into emission without
revalidation. It must distinguish immutable facts from current owner-pin,
capacity, generation and post-failure authority, retain all failure/alias
semantics, and price real linked/PE work before introducing another cache or
capability. No such elimination is implemented or authorized by these counts.
