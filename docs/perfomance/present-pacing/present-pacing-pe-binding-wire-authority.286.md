---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Single binding wire-ordinal authority in PE admission"
type: evidence
status: bounded-representation-economy
updated: 2026-09-08
source: experiments/output/pe-settlement-economy-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-locator-authority-9ca31f4f-gt2-off-perf-r1-20260908
related: present-pacing-pe-stream-binding-reuse.285.md; present-pacing-pe-layout-dynamic-economy.284.md; ../../../specs/d3d9/recorder/requirements.md
---

# Single binding wire-ordinal authority

H286 removes the duplicate local wire index from `PeSemanticBindingLocator`.
The private prepared identity witness is its sole authority. This reduces
active occurrence writes, duplicate comparisons and call-local storage;
it does not remove identity validation, retention or canonical emission.
Actual-DLL PE32 helper measurements show a small binding-path saving, with
an empty-path cost. Whole-PE CPU economy and README recovery remain open.

## Authority and semantic boundary

Under R-CORE-REC-7.16, an occurrence retains its checked mapping ordinal,
occurrence ordinal and wire/pin disposition. Its identity witness supplies
the wire ordinal. Every binding family still checks the fresh source's full
qualified identity and object pointer, nullable/occurrence shape, wire-count
and global-remap bounds, and actual physical pin as applicable.

Public `prepareAdmission` produces an inspection-only witness. No mutating
entry accepts it: ordinary and observed append construct a new local witness
behind private, owner/frontier-bound `PreparedAppendAuthority`. Retain and
settlement callbacks receive no alias to that witness. The inverse
`wireOrdinals` relation is established by private witness construction;
forging an inspection copy does not forge an append capability.

Consequently malformed-witness tests belong to cold reconstruction, not a
new per-binding product check. Cold reconstruction now checks identity/wire
count bounds and wire-visible inverse mappings; compact occurrence checks
remain. This does not promise validation of unused bytes in a generic
non-wire-visible identity. Source mutation and actual append failure/retry
remain runtime concerns and retain their existing guards and tests.

The change adds no heap ownership, shared state, ABI field, scheduling
transition or runtime flag. Canonical wire bytes and PE/unix policy are
unchanged. It is a local representation refinement, not a new TLA+ progress
or resource-lifetime proof.

## Linked code and storage

Frozen baseline is H285 `9ca31f4f`; both PE widths are rebuilt with the
canonical release configuration. Complete lexical families include outlined
lambdas, which contain most of the changed work:

| Family | PE32 bytes / instructions, before → after | PE64 bytes / instructions, before → after |
|---|---:|---:|
| Qualified prepare | 3000 / 856 → 2806 / 793 | 3305 / 820 → 3177 / 783 |
| Qualified sparse writer | 20638 / 6443 → 20451 / 6387 | 21825 / 5659 → 21778 / 5614 |
| Unobserved compact append | 307 / 81 → 307 / 81 | 349 / 80 → 365 / 81 |

Static direct-call counts are unchanged in these families. The twelve audited
hot-entry metric rows are unchanged; that does not mean complete binary
identity or identical executed instruction counts. Writer family totals
include uncommon paths and must not be summed as per-call execution cost.

The locator shrinks 16→12 bytes. The 45-identity compact prepared value shrinks
2992→2808 bytes on PE32 and 3040→2864 on PE64. Its enclosing compact append
stack subtraction shrinks 3032→2848 and 3136→2960 bytes respectively; these
are prologue observations, not whole-call maximum stack bounds. Owner size
is unchanged at 240/424 bytes. Both DLL files shrink by 2048 bytes.

## Actual loaded-PE helper economy

`analysis/locator_economy.cpp` invokes the frozen DLL's actual
`prepareAdmissionQualified<45, PeSemanticCompactAdmissionPlan>` by checked
symbol RVA and code prefix, with PE32 `thiscall` or the PE64 ABI. Separate
baseline/candidate fixtures use their matching headers because the prepared
value layout changes. The baseline header tree comes from a retained
`git archive 9ca31f4f`; the build receipt binds source, archive, flags,
support-library and executable hashes. All sixteen qualified processes
reconcile fixture and three staged runtime images against the five-image
frozen manifests; bridges and Unix provider are identical between images.

The fixture builds real compact sparse plans and obtains genuine qualified
source callbacks. A hand-written shape/locator oracle and untimed matching
header differential check empty, one-texture, two-unique-texture and
duplicate-texture inputs. Two duplicate binding rows mean two occurrences,
one identity and one wire handle. Fake resource pointers are never
dereferenced; unexpected retain/release stubs abort. The empty-owner fixture
does not execute writer, COM reference operations, seal or Metal.

Each cell warms 1000 calls. Timing includes required witness reset, the
loaded helper, loop and outcome/checksum consumption. Order is
baseline/candidate/candidate/baseline per width, first at 3 million iterations
and then 20 million. Checksums agree within each width/iteration population.
No sampler or product diagnostic clocks are enabled.

Long-batch mean QPC ns per call, with both process values retained:

| Shape | PE32 before → after | PE64 before → after |
|---|---:|---:|
| Empty binding set | 72.201 → 73.077 | 50.170 → 51.155 |
| One texture | 108.017 → 104.023 | 69.936 → 68.503 |
| Two unique textures | 139.478 → 135.883 | 88.310 → 84.109 |
| Duplicate texture | 125.227 → 121.983 | 79.120 → 79.550 |

PE32 binding means improve 3.2–4.0 ns, but the two-unique ranges overlap:
baseline 136.747–142.209, candidate 132.942–138.824 ns. The empty path costs
0.876 ns PE32 and 0.985 ns PE64. PE64 duplicate-texture is mixed, with
candidate 77.556–81.544 versus baseline 79.059–79.180 ns. Do not describe
every path or every repeat as faster.

All initial short-batch results remain in `economy-qualified.json`. PE32
binding means improve 3.254/5.700/3.402 ns, empty costs 0.294 ns. PE64
one-texture candidate varies 67.724–98.754 ns versus baseline
69.790–69.813; its mean is worse. The long batch was collected because of
this variation, not to silently discard that process. PE64 short means for
empty/two-unique/duplicate change +1.158/−0.258/−1.502 ns.

QPC is warm isolated elapsed time, not exclusive CPU. Raw `GetThreadTimes`
fields are retained but excluded from economic conclusions: this Sikarugir
runtime has the audited process-time fallback described in
[H280](present-pacing-pe-constant-economy.280.md). No measured workload shape
weights, all-family economics, full-append timing or in-situ CPU saving is
available here; these helper deltas cannot be converted into FPS recovery.

## Native and actual rendering evidence

All four full canonical builds pass. Six focused suites pass on each of
native arm64 and x86_64: semantic owner, ledger variants 0/1, semantic batch,
producer differential and retainer. Existing all-family source-mutation,
rollback/retry and canonical-byte differentials are preserved. New generic
and compact cold negatives cover reversed wire-order and identity-count
overflow; compact sentinel/wire-count probes and active-prefix storage tests
use the single authority. No new TLC run is attributed to H286.

Four actual PE32/PE64 baseline/candidate processes run the checked-in
`d3d9_inline_constants` fixture. Every process passes its four registered
checks, 24 readback rows and two zero device-release receipts. All 96 pixels
per candidate width match the corresponding baseline exactly. The independent
expected-color oracle uses its existing per-channel tolerance of 3; the
observed maximum difference from ideal expected color is 1. This is bounded
default-path delayed-readback evidence, not every binding-family or concurrent
device-lifecycle validation.

Preserved setup failures are not evidence against the candidate: the first
Meson invocation used an unavailable uv module; a native test reused the
cleared diagnostic output after a negative probe; micro smoke first lacked
canonical postprocessing stamps, then expected one rather than two duplicate
occurrences. Final r3 builds/tests and r2 fixture binaries are the qualified
set. The first pixel parser incorrectly demanded exact ideal colors even
though the checked-in fixture passed its tolerance; offline requalification
retains that same baseline log and candidate readbacks still compare exactly.

## Sampler-OFF GT2 spots

Both candidate runs use the catalogue runner, Sikarugir PE32, the `perf`
Warn profile and frame sampling 1. Offloaded replay is ON; inline constants,
segmented transport, CPU-ready tape and index reorder are OFF. No PE sampler,
phase observer, external CPU observer, build or concurrent Wine measurement
runs alongside the scenes. 06 uses focus-free named GT2 autorun.

| Product | Resolution | Frames | Wall interval | Sampled scene FPS | Versus README |
|---|---|---:|---:|---:|---:|
| 05 GT2 | 1024×768 | 1920 (1–1920) | 64.850 s | 29.607 | −1.64% vs 30.1 |
| 06 GT2 | 1280×720 | 1164 (1013–2176) | 64.040 s | 18.176 | +0.42% vs 18.1 |

FPS is selected frames divided by their positive `wall_ms` sum, not
benchmark-owned `.3dr` or process-elapsed FPS. 05 includes all positive-wall
frames; 06 uses the contiguous `draw_calls > 100` population and retains its
loading-tail mixture. Both harness verdicts pass; all eight reported
chunk/GPU/pipeline health counters are zero. 05 exits normally with final
coverage through Present 1921. 06 reaches the permitted post-scene GUI timeout;
counters end at Present 2160, leaving 16 frames outside the health snapshot.
This does not establish complete scene correctness.

Compared with the preceding H285 spots, FPS changes −0.43%/−0.03% for 05/06.
There is no fresh matched baseline or CPU sample in this increment. These
small single-run differences establish neither regression, repeatable net
economy nor README recovery. README remains unchanged. Full before/after
source, frozen/staged five-image and runtime hashes reconcile in the two
`*-candidate-perf-r1-measurement.json` receipts.

## Provenance and remaining target

The runtime patch against `9ca31f4f` is SHA-256
`c9dc93adf6e96f6c8bdd6028ca34eba91da6000dc00fefe2cfbb81fb9a5f8e6d`.
PE32 changes `dfa760cf99cd9c6d`→`194ef33937008d2e` and PE64
`c13b30ce6fac93ee1`→`1d70384fccd06351`; manifests retain full hashes.
`qualify_economy.py` reconciles micro and pixel evidence without launching
Wine; `family-codegen.json` retains exact disassemblies. Auxiliary harnesses
and runtime copies are ignored local evidence, not installed product APIs.
The original manually authored build receipt has one invalid 62-digit header
digest, missing `d4`. `build-receipt-correction.json` preserves that receipt hash
and independently binds identical header bytes from the baseline Git blob,
retained source archive and current file; other recorded hashes match. No raw
build-time hash receipt exists for that header, and none is retroactively claimed.
`final-audit.json` binds that explicit correction, final native/doc checks,
micro/pixel receipts, both GT2 scenes and the restored candidate Wine images.

The larger remaining target is complete owner admission/materialization,
not another layout-only saving. Retainer epoch expiry is separately bounded:
the clean no-expiry shortcut already exists, while expiry compacts survivors
and rebuilds the index. Current timing cannot distinguish expiry, survivors
and rollback tombstones. Price those shapes with the existing cold observer
before changing the index; preserve release order, warm epochs and rollback.
