---
domain: present-pacing
workload: STALKER CoP Benchmark 1.6.02 installed R2 renderer
title: "H539: Installed query consumers expose thresholds and cooperative polling"
type: evidence
status: static-consumer-confirmed-live-causal-join-open
updated: 2026-10-15
source: experiments/output/rea/stkcop-query-h539/{commands.md,audit.py,audit.json,provider.json,overview.json,pe-headers.txt,disassembly.txt,imports.txt,query-strings.json,occq-get-function.json,occq-consumer-function.json}
related: present-pacing-stalker-visibility-gap.401.md; present-pacing-stalker-occlusion-ab.408.md; present-pacing-production-query-diagnostic.506.md; present-pacing-prefix-live-preflight.538.md
---

# The installed game has result-dependent work and a synchronous polling helper

Bounded REA analysis identifies how this installed renderer consumes occlusion
results. It does not establish a late-frame CPU owner or explain the D9VK FPS
gap. No game was launched, no target bytes changed, and no runtime policy was
promoted. Existing H538 live work remains separate and unfinished.

## Identity and method

| Field | Evidence |
|---|---|
| Catalogue | `app-d3d9-stkcop-bench`, standalone benchmark 1.6.02 |
| Owning module | `experiments/apps_3rd/app-d3d9-stkcop-bench/app/bin_test/xrRender_R2.dll` |
| SHA-256 | `6e1914e047075409b727bf9fd3087ae6cb2404ac85fb8cfd2e797550ef3cc776` |
| Format / preferred base | PE32 x86 / `0x10000000` |
| Analysis | REA 6.4.0, Ghidra 12.1.4, Node 24.21.0; explicit Homebrew Ghidra root |
| Source inspection | HEAD `0db622f37bb2356cc643c955fc87f6952731dbc0`, dirty worktree |

All addresses below are RVAs, not file offsets or live Wine addresses. Add the
preferred base for the saved static VA; use the actual loaded base for a live
join. The ignored artifact directory retains full instructions, decompiler
evidence, commands, limitations and an independent 50-instruction byte audit.
The audit also counts slots from the installed MinGW D3D9 declarations:
`CreateQuery` is device slot 118 (`+0x1d8`), `Issue` query slot 6 (`+0x18`),
and `GetData` query slot 7 (`+0x1c`). Offsets assume four-byte x86 pointers.

Ghidra's private function names and recovered types are provisional. Its
getter pseudocode gives inconsistent names to the same output stack slot;
both GetData calls actually address the initialized DWORD at the same stack
location. Instructions, rather than those names, establish the output flow.
LLVM's nearest exported-symbol labels do not name the private functions.

## Query creation and polling

The pool constructor at RVA `0x24960` calls device `CreateQuery` at `0x249a9`
with type 9 (`D3DQUERYTYPE_OCCLUSION`), up to `0x600` queries, stopping on a
failed HRESULT. The allocation helper at `0x24af0` issues BEGIN (`2`) at
`0x24b94`. Type-8 EVENT creation also exists at `0x5db2` and `0x6181`;
those objects must not be conflated with this occlusion pool.

The getter at `0x24bb0` has the following verified behavior:

1. Disabled pool or handle `0xffffffff` returns `0xffffffff` immediately.
2. Otherwise it initializes a DWORD output to zero and calls
   `GetData(output, 4, D3DGETDATA_FLUSH)` at `0x24c6e`.
3. Exactly `S_FALSE` (`1`) enters a retry loop. It calls `SwitchToThread`,
   then `Sleep` only if that call returns zero. Sleep uses the integer at
   RVA `0xce250`, registered as `r2_wait_sleep` with bounds 0–1. The active
   runtime setting is not established here.
4. The loop calls imported `CTimer::GetElapsed_ms`; unsigned elapsed `>500`
   sets output to `0xffffffff`. Otherwise it retries FLUSH at `0x24cbd`.
5. `D3DERR_DEVICELOST` also forces `0xffffffff`. Other HRESULTs do not have
   this explicit override; do not describe all errors as fail-visible.
6. The normal exit recycles the pool entry, clears the active pointer and
   caller handle, then returns the output DWORD.

This is a synchronous game helper around a nonblocking API. Its timer is
checked **between** calls, not while inside GetData; 500 ms is not an absolute
wall-time ceiling. Repeated Pending calls can include both provider work and
guest yield/sleep time. This mechanism alone gives no frequency or CPU price.

## Result consumers

Seven direct getter call sites are present in the disassembly. This count is
not a proof that no indirect caller exists.

| Getter call RVAs | Predicate | Verified consequence |
|---|---|---|
| `0xc9ae`, `0xd191`, `0xd226`, `0xd388`, `0xd48c` | unsigned result **>4** (`cmp` / `seta`) | Store Boolean at object `+0x184`, clear pending byte `+0x185`; subsequent branches skip work when false. |
| `0x32af5`, `0x3763e` | zero versus nonzero | Zero/nonzero paths update different object/list counters. Their full subsystem meaning remains unresolved. |

The five threshold sites belong to Ghidra's function at `0xc950`. For its
first list, false removes the entry before later processing; true invokes
`0x369d0`. Later lists gate calls to `0x642a0` and `0x65f40` on the same stored
Boolean. These are concrete work-selection branches, not merely statistics.
Private object types and complete downstream draw expansion are not recovered.

The threshold paths also write object `+0x178` differently: false stores a
value loaded through the imported `Device` object's `+0x18` plus one; true
adds ten plus a helper's signed remainder modulo ten. This suggests different
retest scheduling, but the field's reader and helper output range are not
closed here. Do not claim an exact 10–19-frame interval from this alone.

`0xffffffff` passes both unsigned >4 and nonzero tests. Thus the observed
timeout/disabled sentinel selects these consumers' true branches. It is not a
general proof that every engine visibility decision is conservative.

## Connection to dxmt9, and the counterevidence

The current `src/dxmt9/dxmt9_query_policy.hpp` / `metalOcclusionEnabled`
defaults OFF. `src/d3d9/core_resources.cpp` / `Device::issueQuery` resolves
default occlusion from `activeOcclusionCount_`; draw paths in
`src/d3d9/core_draw.cpp` increment that counter by `draw.primitiveCount`.
Completion-fence readiness and the meaning of the result bytes are separate.
Primitive count is not a depth-tested visible-sample count and can select a
different branch in these installed consumers.

This provider-policy difference is **not newly discovered**. H401/H402 already
identified it, and [H408](present-pacing-stalker-occlusion-ab.408.md) tested
real visibility: diagnostic draws/Present fell about 9.51%, but CBs more than
doubled and visibility ON alone did not establish a large FPS improvement.
Its unmatched whole-run populations do not give a per-consumer saving.
H539 adds exact installed-game branch thresholds and retry behavior, not a
reason to repeat that broad toggle or bypass its correctness gates.

[H506](present-pacing-production-query-diagnostic.506.md) reproduces the
H502/r5 capture's nine Issues, five polls and four results of 160. A value of
160 would pass both predicates above. However, the retained record lacks the
guest consumer caller RVA, so those four results cannot yet be attributed to
the five threshold sites rather than the two nonzero sites or another path.
Same-input replay reproduces the resulting command stream; it does not rerun
the game to regenerate commands after a different query result.

## Next discriminating evidence

Before another broad FPS campaign, collect a bounded **caller-qualified query
episode** in the existing spatial late-scene envelope:

- Bind loaded module hash/base, thread, query identity/generation/Issue ordinal,
  END/publication/submit/completion identity, first/last poll and actual result.
- Attribute the caller **of the getter helper**, not only the immediate
  GetData return address: all seven consumers share its two COM call sites.
  A validated guest stack or a bounded helper-entry observation is necessary.
- Count results in `0`, `1..4`, `5..0xfffffffe`, and sentinel `0xffffffff`
  buckets; separate timeout, disabled and device-lost when observable.
- Measure helper CPU separately from yield/sleep and provider wait wall.
  Correlate downstream command counts, without equating time adjacency with
  causality or modifying recorded query values.

If this distinguishes result-driven extra work, qualify a same-scene hardware
visibility comparison with the existing lifetime/pixel gates and report its
publication cost separately. If the episode is dominated by Pending inside
the helper, use the existing END-to-completion timeline to identify the owner.
Neither outcome is established by static reachability. The largest default
single-thread CPU owner and correctness-qualified removable amount remain open.
