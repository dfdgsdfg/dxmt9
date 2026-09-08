---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Fixed-granule compact descriptor emission economics"
type: evidence
status: rejected
updated: 2026-09-09
source: experiments/output/pe-descriptor-inline-H293-20260909/analysis
related: present-pacing-pe-descriptor-table.292.md; present-pacing-pe-current-hotpath.291.md
---

# Fixed-granule compact descriptor emission economics

H293 removes the variable-size copy call implicated by
[H292](present-pacing-pe-descriptor-table.292.md), but does not qualify the
general replacement. A loop of constant 16-byte inline copies improves dense
tables while small-table append still costs more, especially in PE64.
**The entire candidate product-header change is withdrawn.** No candidate
pixel or wild-FPS promotion follows the failed economic gate.

## Candidate and observable contract

The baseline is `965eb66e`. `baseline.json` freezes its current canonical
images, complete tracked product-source identities, copied headers, build
options and utility thin-archive members. These are new exact image identities;
the restored canonical PE timestamps differ from H292's older frozen baseline.
H292's final audit is provenance, not an assertion of raw-image identity.

Only the compact overload of `PeSemanticBatchOwner::writeSparsePlanPayloadQualified`
in `src/d3d9/d3d9_pe_semantic_owner.hpp` changes. The candidate preserves the
full table-range check before visitors, all section/payload/padding checks,
source qualification, resource identity/pin/remap checks and visitor order.
After exact visitor/count/cursor conservation, it copies each active descriptor
with constant-size `__builtin_memcpy_inline`, guarded by `__has_builtin` and a
portable `std::memcpy` fallback. Compile-time size/alignment/field-offset and
representation locks bind prepared and wire descriptors. There is no count
threshold, runtime flag, public unchecked seam or inactive-tail read.

These are speculative owner bytes, not a new publication boundary. Existing
append settlement and checkpoint rollback own their visibility and cleanup.
R-CORE-REC-7.66 now explicitly permits fixed-granule realizations of the same
optional contract. This is a value-materialization refinement; no lifecycle,
interleaving, transport or TLA transition changes. The already committed H292
independent-builder, scalar-subset, dense indexed 23 / indexed-UP 24 and late
rollback/retry tests are reused unchanged. Generic 25-section coverage remains
separate from lawful compact timing inputs.

## Actual PE economics

`economics-main.json` binds eight successful processes: PE32/PE64, baseline
and candidate, two repeats each, under Sikarugir `cx-24.0.7`. Versions alternate
within each width; no build or native/formal test runs concurrently. The exact
H292 final fixture is reused, with all four new fixture-build receipts binding
the compiler, source, generic-builder/producer inputs, frozen DLL and headers,
and thin archive members. Per-run receipts verify the actual staged images.

Each process checks the independent canonical-builder oracle and measures
0/1/2/4/12/23/24 active descriptors through two distinct paths: the actual
loaded private DLL writer and a separately compiled matched-header public
append. The latter is not the loaded production parent. Bindings are lawful
null rows, not a retain-heavy workload. Writer timing uses 1,000 warm and
100,000 timed calls; append uses eight warm groups and 2,000 timed groups of
64 appends. Planning/reset/seed/oracle work is outside each QPC pair. Empty-pair
calibration is recorded, not subtracted.

Percent changes below compare two-process medians. Positive means slower.
These are scoped raw QPC elapsed times, not exclusive CPU or FPS.

| Active descriptors | PE32 writer | PE32 public append | PE64 writer | PE64 public append |
|---|---:|---:|---:|---:|
| 0 | -10.55% | +3.42% | +1.16% | +15.14% |
| 1 | +4.76% | +2.80% | +16.07% | +11.55% |
| 2 | +6.15% | +1.44% | +6.90% | +10.75% |
| 4 | -4.11% | -1.21% | +5.31% | +4.90% |
| 12 | -4.52% | -1.44% | -9.02% | -4.19% |
| 23, indexed | -4.94% | -2.84% | -9.06% | -7.28% |
| 24, indexed-UP | -4.74% | -4.72% | -11.38% | -6.83% |

PE64 one-section append is 114.60 -> 127.83 ns, and two-section append is
126.37 -> 139.96 ns. Both candidate repeats exceed both baseline repeats for
these cases. PE32 two-section append ranges overlap; its +1.44% median is not
a robust universal regression claim. Conversely, the broad PE64 small-input
penalty is enough to withhold a general replacement without a long wild run.

Zero-section append must not be attributed to descriptor-copy execution:
there is no copy at count zero. Its increase, and the difference between the
writer and append columns, leave parent code shape and surrounding execution
as separate questions. Call-free descriptor emission does not by itself
establish a cheaper complete transaction. Dense-case gains do not provide workload weighting or a
safe small/dense dispatch policy.

## Verification and codegen

`build-r1.json` passes all four canonical build directories; `native-r1.json`
passes eight focused specs. `tla-r1.json` passes the unchanged TLA suite in
146.56 seconds, regression evidence for the existing protocol rather than a
new proof of concrete bytes or CPU economics. `freeze-audit-main.json`
confirms an owner-header-only product delta, unchanged build options and
utility members, and identical bridges/provider.

The first static decoder stopped at the first `ret`, missing branch-reachable
tail blocks. Original reports and the intermediate CFG reports are retained,
but their first-return body counts are superseded by
`baseline-codegen-h293-cfg.json` and `codegen-report-h293-cfg-qualified.json`.
Both versions now use entry-reachable instruction accounting within exact
next-text-symbol bounds, including resolved switch targets. Unreachable table
bytes and padding are not counted as reachable instructions.

The corrected writer main grows from 1,733 to 1,886 reachable bytes in PE32
and from 2,110 to 2,175 in PE64. The candidate includes respectively 51 and
149 reachable bytes after the first return. Its main span is 2,016/2,176 bytes;
prologue stack subtraction falls 276 -> 240 and 536 -> 472. Direct calls are
24 -> 25 and 24 -> 24, with zero external copy calls in both mains. The active
descriptor loop uses inline 16-byte `movups` load/store pairs, compiler-unrolled
bulk iterations and a bounded remainder. The writer is not otherwise call-free.

Outlined helper sums shrink by 872/2,458 reachable bytes. Each width still has
two external `memcpy` and nineteen `memset` helper callsites for other work.
The audited production materializer, admission, append and public-root families
retain their code-size/call metrics. This is linked DLL evidence, not a claim
that the separately compiled public-append fixture has identical surrounding
code. The larger main, smaller helper family and removed copy call do not
independently explain the complete append timing.

After withdrawal, `build-r2.json` and `native-r2.json` pass all four builds and
the same eight specs. Restored-baseline PE32 and PE64 readbacks pass 24 pixel
signatures each; these are restoration checks, not rejected-candidate pixel
proof. `final-audit-qualified.json` binds the preserved candidate, all eight immutable
economics runs, corrected CFG reports and the five exact baseline images
restaged in the Wine root. Rebuilt canonical DLLs differ from the frozen
baseline only in COFF and CodeView timestamps; complete image bytes match
after normalizing those fields. Raw and normalized identities remain distinct.
The earlier `final-audit.json` consumed an intermediate CFG report before
crossing-edge provenance was finalized. It and its exact referenced reports
remain preserved; the qualified audit rechecks the final script/report hashes
after the worker stops. Runtime/economics evidence and numeric verdict are unchanged.

## Remaining scope

Do not add an unmeasured count threshold or architecture-specific dispatch to
rescue this candidate. A follow-up needs production-parent/call-site economics
and genuine descriptor cardinality, with the same independent bytes and
rollback pins. Linked code must include branch-reachable blocks after early
returns; smaller first-return text is not evidence of a smaller function.

No new game FPS is measured for the rejected candidate. The latest qualified
OFF spots remain 05 GT2 29.809 FPS (H290) and the desktop-activity-excluding
06 GT2 rerun 18.217 FPS (H291). The latter has 34 frames outside the last full
health snapshot. They are not H293 results or repeatable CPU/FPS improvement.
README remains unchanged.
