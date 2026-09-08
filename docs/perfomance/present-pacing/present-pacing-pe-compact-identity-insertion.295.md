---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact always-wire identity insertion"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-policy-lookup-H295-20260909/analysis; experiments/output/pe-semantic-policy-H295-20260909/analysis
related: present-pacing-pe-public-com-parent.294.md; present-pacing-pe-current-hotpath.291.md
---

# Compact always-wire identity insertion

H295 starts at `53b5ad82`. The candidate removes an unused result from
compact identity insertion, not identity validation, deduplication or physical
retention. Native, linked-code and actual public-COM checks pass. Four timing
processes per version/width give mixed, overlapping draw costs, not a proven
CPU saving. The specialization is retained after actual PE pixel parity and
05/06 GT2 spots of 30.136/18.205 FPS; these do not establish causal FPS gains.

## Candidate selection

The initial metadata-lookup hypothesis is rejected before implementation:
`recordRule` and `peSemanticProducerPolicy` already lower to bounded indexed
lookups in both PE widths, despite their source-level loops. The linked bodies
retain record-tag range/hole checks and producer matching, not a 21-row scan.
`linked-lookup.json` uses the preserved H293 image pair reused by H294; it is
not an H295 before/after build comparison. Main also inspected the current
canonical PE32 qualified-preparation and rule bodies. A dense producer-kind
match could simplify the second gate, but is a separate deferred candidate.

The selected compact path calls
`PeSemanticPreparedIdentityWitnessT<45>::add` with `wire=true` and discards
`newlyWireVisible`. Its out-of-line body still branches on wire visibility and
writes the result. A shared `addImpl` transition specializes these two facts;
generic `add` keeps runtime visibility and promotion reporting, while compact
preparation calls `addWire`. Record-local dedup, current owner pin lookup,
source validation, first-wire order, quotas and settlement remain unchanged.
No new cache, wire format, transport/default or queue policy is introduced.

R-CORE-REC-7.67 binds active identity/wire prefixes and output/failure behavior.
The native tests compare the specialization with general insertion and literal
expected results, including null/malformed refs, kind/generation/pointer aliases,
promotion, independent wire-capacity failure and subsequent direct-only insertion.
Inactive uninitialized storage and padding are not compared. Existing owner
tests supply all-family retain-fault, rollback, prefix and retry coverage.
These are bounded local transition checks, not exhaustive arbitrary programs
or a new TLA lifecycle model.

## Measurement boundary

H295 freezes five current canonical images and all tracked `src/include/cross`
identities separately from the historical H293 images. `baseline.json` and
`candidate.json` bind each version; build-option parity and unchanged
bridge/provider images are qualification conditions.

The finalized H294 `main3` public-COM fixture is reused unchanged in both PE
widths. It has no private recorder headers or direct helper casts. API mixes,
32-iteration timing windows, 256-record/1,310,720-byte cadence, untimed seed,
drain, getter and FFP readback boundaries are those documented in
[H294](present-pacing-pe-public-com-parent.294.md). Four separate trace runs
prove no timed recorder flush. Logging-OFF rows price complete COM API mixes,
not descriptor counts or exclusive owner/helper cost.

The fixture's `GetThreadTimes` label must not be interpreted as thread-exclusive
CPU. The audited Sikarugir runtime uses a process-times fallback, as recorded
in [H280](present-pacing-pe-constant-economy.280.md). QPC rows, aggregate Wine
CPU and sampled game FPS remain different evidence layers. No build or native
test may overlap the performance fixture. Current README values are unchanged.

## Linked work removal, not a timing result

The exact H295 frozen-image pair has identical build options and unchanged
provider/bridge bytes. Only the owner header changes among the product sources;
both `d3d9.dll` images are rebuilt. `compact-codegen-{baseline,candidate}.json`
bind the selected insertion and parent bodies to these images.

| Compact insertion body | PE32 before → after | PE64 before → after |
|---|---:|---:|
| Bytes excluding terminal `int3` filler | 437 → 423 | 362 → 350 |
| Linear instruction count | 136 → 119 | 107 → 96 |
| Branch instruction count | 22 → 19 | 22 → 19 |

PE32's local stack subtraction drops from 16 to 12 bytes; PE64 removes one
saved-register push. The generic witness's body and the qualified-preparation,
record and append parent body metrics do not grow. These are bounded linked
body observations, not dynamic instruction or cycle counts. Raw disassembly
of other writers can contain jump-table data; it is not executed-work evidence.

The compiler still passes dead visibility/promotion arguments to the outlined
specialization. Removing their tests and result stores does not remove those
ABI arguments. A genuinely narrower outlined signature is a separate, unpriced
follow-up, not an additional saving claimed here.

## Actual public-COM economics

`economics-main.json` preserves the first two repeats; the bounded follow-up
`economics-repeat-main.json` includes four repeats per version/width. The strong
qualifier checks all 20 processes (four traces and 16 logging-OFF runs), exact
fixture/recipe/image bindings, 12 rows per process and the loaded vtable's
`DrawPrimitive` entry. Its RVA is `0x101b0` in PE32 and `0x11040` in PE64 in
both versions. Source snapshots and all five frozen images remain unchanged.

The following are medians of four process-level per-operation QPC aggregates,
in ns/iteration, for setter-plus-draw windows. The machine receipt's shorthand
"four process means" describes the input aggregates, not the cross-process
reduction; the latter is a median. Empty-QPC cost is recorded, not subtracted.

| API mix | PE32 baseline → candidate | PE64 baseline → candidate |
|---|---:|---:|
| Steady draw | 468.16 → 467.97 | 306.35 → 309.67 |
| Viewport | 510.84 → 510.11 | 345.78 → 345.09 |
| Viewport + scissor | 564.28 → 563.89 | 374.98 → 366.65 |
| Four scalar setters | 931.47 → 933.28 | 549.39 → 545.75 |
| Stream offset | 662.99 → 627.64 | 422.07 → 419.43 |
| Six constant setters | 2089.94 → 1937.30 | 1278.49 → 1220.51 |

Every draw-mix range overlaps. PE64 steady's median is 1.08% higher; the larger
PE32 stream/constant reductions also overlap and do not establish savings.
Setter-only controls are mixed too: PE64 viewport/scissor is 14.02% lower with
non-overlapping ranges, while its stream-offset median is 6.54% higher with
overlap. Controls are neither subtracted nor attributed to identity insertion.
The API mix does not establish target-helper invocation counts or GT2 weighting.

Thus the retained decision is a bounded removal of
unused work with no demonstrated parent regression, **not** an economic
neutrality proof, exclusive PE CPU result or whole-game gain. The fixture's
green-center/black-corner FFP checks are not a full-image or shader-constant
oracle; constant setters additionally have getter checks.

## Correctness and build evidence

All four canonical full builds complete. Focused native tests pass 8/8 on the
host and 4/4 in the x86_64-native build, including the install-name audit.
`dxmt9-verify-tla` passes in 147.35 seconds. This reruns existing lifecycle
models; it does not introduce a new concurrency proof for a local insertion
specialization. The exact test JSON and logs are retained under `analysis/`.

Four additional real-PE pixel processes, baseline/candidate in each width,
pass 24 signatures × four sampled pixels each. Candidate values exactly equal
baseline in both widths; expected-channel tolerance is three. This covers the
default-path constant fixture, not every resource category or full-scene
temporal correctness. `pixels-{x86,x64}-{baseline,candidate}-r2/pixels.json`
bind the source, fixture, staged images and raw outputs.

Initial build invocations hit toolchain setup errors (missing mingw compiler,
then a PATH-precedence collision for Unix clang); successful `-r2` build logs
are distinct. The first pixel qualifier expected one case name instead of the
runner's four function names; that process passed its cases but is not counted
as qualified. Its recipe/log and original driver remain intact; all four `r2`
processes were rerun with the exact verdict set. Neither is a product failure.

## Observer-off GT2 integration spots

The selected run roots are
`app-d3d9-3dmark{05,06}-pe-compact-identity-H295-gt2-off-perf-r3-20260909`.
Both bind the exact candidate source, build options and five frozen/staged
images. Sikarugir `cx-24.0.7`, perf/warn, validation off, replay offload on;
inline constants, segmented transport, CPU-ready Tape and index reorder off.
PE sampling and phase diagnostics are off; renderer frame sampling remains on.
No build, native test or second benchmark overlaps the selected runs.

| Selected scene | Resolution | Selected frames | Wall interval | FPS | README |
|---|---|---|---:|---:|---:|
| 05 GT2 | 1024×768 | 1–1945 (1,945) | 64,540.412 ms | 30.136 | 30.1 |
| 06 GT2 | 1280×720 | 998–2164 (1,167) | 64,103.854 ms | 18.205 | 18.1 |

Selection is consecutive positive `wall_ms` rows with `draw_calls > 100`,
including the historical loading-tail mixture. These are renderer-sampled
averages, not `.3dr` results or process-elapsed estimates. Both are near README
(+0.12%/+0.58%); one spot each is not an A/B population or a repeatable recovery
claim. The 06 value is also near H291's separate 18.217 spot. README is unchanged.

Eight health counters are zero through 05 ordinal 1946, covering its whole
selected scene, and 06 ordinal 2160, leaving **four unreported tail frames**.
The 06 scene finishes before the catalogue-permitted post-scene GUI timeout
(return 143); 05 exits normally. Saved forest/effect images were inspected,
not used as exhaustive temporal correctness proof. The runner recovers one
05 `.3dr`; 06 emits none. Both artifacts and the absence are preserved.

The first 05 attempt used the launcher's periodic focus-keepalive default and
was deliberately interrupted/excluded. Its orphan focus helper then overlapped
the beginning of 05 `r2` before being terminated, so that run is excluded from
FPS comparison too, despite passing runner/source/health checks. The original
logs, recipes, drivers and explicit exclusion receipts remain unchanged.
The chosen `r3` begins only after both old processes are absent and explicitly
sets `DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC=0`. The 06 `r3` suffix denotes the shared
driver revision; it is the only H295 06 wild run, not a selected best of three.

## Final receipts and remaining scope

`final-audit-main.json` binds manifests/source snapshots, native/TLA logs,
linked reports, the 20-process COM comparison, four pixel reports, selected
wild receipts, exclusions and final documentation. All five shared Wine images
remain the candidate, matching the canonical build outputs. The two evidence
roots retain their initial policy-lookup names; only compact identity insertion
was changed in production.

The next bounded question is whether a genuinely narrower insertion signature
or dense producer-kind matching removes further linked work without parent
cost. Neither is implemented or priced here. Workload-weighted and exclusive
PE CPU savings, as well as repeatable whole-game improvement, remain open.
