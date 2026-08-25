---
id: present-pacing-bridge-crossing-decomposition.237
title: Present-Pacing #237 - Bridge Crossing Decomposition: Lock/Unlock/Upload/Commit
status: accepted attribution (unlock split measured; core-unlock upload admission is the owner)
date: 2026-08-25
source: experiments/output/app-d3d9-3dmark05-current-bottleneck-gt2-clean-cpu-r2-20260825 (existing-run analysis); experiments/output/app-d3d9-3dmark05-bridge-unlock-split-gt2-r1 (unlock-split run)
---

# Present-Pacing #237 - Bridge Crossing Decomposition: Lock/Unlock/Upload/Commit

## Question

#236 attributed `13.9%` of the saturated GT2 producer thread to `winemetal.dll`
residence and hypothesized — without a function-level decomposition — that it is
"buffer lock/unlock, upload, and `commit_chunk` crossings". Decompose that
residence by call class, and split the one class no instrumentation covers:
the unix side of `dxmt9c_buffer_unlock`.

## Existing-run analysis (no new code)

The #236 clean run already carries the PE-side per-opcode bridge round-trip
timers (`[dxmt9-bridge-perf]` / `[dxmt9-bridge-perf-opcodes]`,
`src/winemetal/winemetal_bridge.cpp` dispatcher span, harvested into
`result.json:dxmt9_bridge_counters`). Normalized to the `2,634` presents at the
final cumulative emission:

| class | calls/present | ms/present | us/call |
|---|---:|---:|---:|
| `dxmt9c_device_commit_chunk` | `11.49` | `1.473` | `128.2` |
| `dxmt9c_buffer_unlock` | `15.32` | `1.259` | `82.1` |
| `dxmt9c_buffer_lock` | `15.32` | `0.420` | `27.4` |
| COM addref/release (buffer/texture/shader) | `~508` | `0.254` | `~0.5` |
| other (factory display-mode polling, surface, lifecycle) | `~164` | `~0.15` | - |
| **bridge total** | `713.7` | `3.558` | - |

Cross-checks inside the same run:

- commit: PE round-trip `1.473` vs unix synchronous body
  (`offload_commit_app_cpu_ms`) `1.359` -> dispatch overhead `~9.9us/call`.
- lock: PE round-trip `0.420` vs unix body (`d3d9_buffer_lock_ms`) `0.335`
  (of which shadow pre-copy `0.082`, low4gb alloc negligible) -> dispatch
  `~5.5us/call`.
- Locked bytes: `5.567 GB/run` (`2.11 MB/present`), `40,360/40,361` locks
  dynamic, `30,200` NOOVERWRITE, `8,952` DISCARD, `0` readonly — the locked
  byte volume is effectively all upload-bound.

The unmeasured residual: `dxmt9c_buffer_unlock` unix side has no timing at all
(`src/d3d9/device_c_resources.cpp:1258-1294`). Its `82.1us/call` PE round-trip
is `3x` lock's, and the two candidate owners cannot be separated:

1. the wow64 shadow->native writeback `memcpy` (`:1266`) — the actual
   "upload" byte motion, expected ~`5.57 GB/run` scale, or
2. `b->obj->unlock(!readOnly)` (`:1281`) — the core unlock, which performs the
   dxmt9-side upload admission for the dirtied range.

## Harness spec

R-237.1: `dxmt9c_buffer_unlock` gains an unconditional perf-counter family
`d3d9_buffer_unlock_*` mirroring `countD3D9BufferLock`'s shape
(calls / cpu ms / max / p50 / p95 / p99), with two child splits that sum
into the parent minus untimed residue:
  - `writeback` (the shadow->native memcpy) with ms, max, and bytes;
  - `core` (the `b->obj->unlock()` call) with ms and max.
Plus classification counts: upload (non-readonly) vs readonly vs
shadow-active calls.

R-237.2: Counter-only observer — zero behavior change to the unlock path,
no new hot-path signature parameters or return values, cost limited to the
same steady_clock reads `countD3D9BufferLock` already pays on lock
(the perf-counter sink's internal gate is the only conditional). Passes
`audit_perf_counter_table.py` and `audit_perf_counter_callsites.py`.

R-237.3: The measurement run reports, per present: PE-observed unlock
round-trip (bridge opcode row) vs unix `d3d9_buffer_unlock_cpu_ms` vs its
writeback/core children, so dispatch overhead and the writeback/core split
are both derivable from one run.

R-237.4 (follow-up split, after the core-unlock verdict below): the core
unlock span decomposes into an unconditional `d3d9_buffer_upload_*` /
`d3d9_buffer_unmap_*` family instrumented in `Buffer::unlock`
(`src/d3d9/core_buffer.cpp`) around the three backend calls:
  - full uploads (`uploadBufferData`): calls / ms / max ms / bytes, with a
    Default-vs-Managed pool byte-and-call split (the range path is
    Default-only by construction, so only full uploads need the split);
  - range uploads (`uploadBufferDataRange`): calls / ms / max ms / bytes;
  - `unmapBuffer`: calls / ms / max ms.
Same observer constraints as R-237.2; the three child spans plus untimed
residue must account for the parent `d3d9_buffer_unlock_core_ms`.

## Method (planned)

GT2, perf profile, no-gputrace, 120s supervised probe with
`DXMT_3DMARK05_ARGS="-gt2 -nosplash -nosysteminfo -noscreens"`,
`DXMT_PERF_COUNTERS=1` (profile default), frame sampling for scene fps
anchoring. Same-build staging; unix provider rebuilt with the new counters;
PE DLLs unchanged (no ABI/schema change).

## Result

Run: `app-d3d9-3dmark05-bridge-unlock-split-gt2-r1` (GT2, perf profile,
no-gputrace, 120s supervised, status pass, `1,714` presents at final counter
harvest; note this window carries more bridge calls per present than the #236
clean run — `1,053` vs `714` — so cross-run per-present rates are not directly
comparable; all splits below are within-run).

Per present, PE-observed round-trip vs unix body vs children:

| class | PE round-trip ms | unix body ms | children (ms) | dispatch us/call |
|---|---:|---:|---|---:|
| `buffer_unlock` (`21.7` calls) | `2.018` | `1.981` | core `1.864` (94.1% of body), writeback memcpy `0.113` (5.7%) | `~1.7` |
| `commit_chunk` (`15.9` calls) | `1.252` | `1.085` | (phase split in `.37`) | `~10.5` |
| `buffer_lock` (`21.7` calls) | `0.673` | `0.499` | shadow pre-copy `0.123`, core (incl. DISCARD zero-fill + mapBuffer) `0.376` | `~7.8` |
| bridge total (`1,053` calls) | `4.587` | - | - | - |

**Verdict: the winemetal.dll bridge residence is owned by the core unlock
upload admission, not by byte motion across the wow64 shadow and not by
crossing overhead.** `Buffer::unlock`'s `b->obj->unlock(!readOnly)` span is
`3,194.6ms/run` = `94.1%` of the unix unlock body (`85.7us/call`); the
shadow->native writeback memcpy is `193.2ms` for `5.38GB` (`~28GB/s`, memcpy-
bound, healthy); the unix-call dispatch overhead is `1.7-10.5us/call` across
the three classes and is not a lever.

Two write-amplification mechanisms are visible in the same run
(`src/d3d9/core_buffer.cpp:56-79`):

1. **Full-buffer uploads.** `Buffer::unlock` takes the exact-range
   `uploadBufferDataRange` path only for Default-pool dynamic NOOVERWRITE
   locks; every other upload sends the whole `storage_`. The managed pool
   alone proves the amplification: `1,224` uploads moved
   `managed_buffer_upload_bytes = 8.93GB` (avg `7.3MB/upload`) from only
   `1,209` managed locks — more than `1.6x` the total locked byte volume of
   all `37,260` locks combined. Default-pool full uploads (DISCARD and plain
   locks) are not byte-counted yet.
2. **DISCARD zero-fill on lock.** `Buffer::lock` under DISCARD does
   `storage_.assign(max(size, desc.size), 0)` — a full-buffer zero write on
   each of the `8,952` DISCARD locks, inside the lock core span.

## Result (R-237.4 split)

Run: `app-d3d9-3dmark05-bridge-upload-split-gt2-r1` (GT2, perf profile,
no-gputrace, 120s, status pass, `1,796` presents, `38,767` unlocks).

Core unlock `3,449.6ms/run` decomposes with `9.7ms` untimed residue:

| child | calls | ms/run | ms/present | share of core | bytes | effective GB/s |
|---|---:|---:|---:|---:|---:|---:|
| full uploads (`uploadBufferData`) | `9,669` | `2,860.3` | `1.593` | **`82.9%`** | `15.57GB` | `5.4` |
| - Default pool | `8,446` | - | - | - | `6.64GB` | - |
| - Managed pool | `1,223` | - | - | - | `8.92GB` | - |
| range uploads (`uploadBufferDataRange`) | `29,098` | `576.1` | `0.321` | `16.7%` | `1.65GB` | `2.9` |
| `unmapBuffer` | `38,767` | `3.5` | `0.002` | `0.1%` | - | - |

**Verdict: full-buffer uploads own the bridge-residence wall.** Total
uploaded bytes are `17.22GB` against `5.66GB` locked — `3.0x` overall write
amplification; the full-upload path alone locks `~4.0GB` (total minus the
range path's exact `1.65GB`) but uploads `15.57GB`, a `3.9x` amplification.
The `5.4GB/s` effective rate (vs `~28GB/s` for the healthy shadow memcpy)
says the span is not pure byte motion — arena allocation/page-fault/hazard
overhead rides with it — but the byte volume is the first-order lever
either way.

Ceiling estimate: range-limiting the full-upload paths to the locked span
would cut upload volume from `15.57GB` to `~4GB`; at the measured rate that
is `~1.2ms/present` of producer-thread time against GT2's `~33ms` frame —
a `~+3.5%` class candidate before adding the lock-side DISCARD zero-fill
saving, i.e. larger than the admission-slab ceiling (`~+2.7%`) at far lower
contract risk.

## Reconciliation with `.38` and backend semantics

`state-churn-encode-append-decomposition.38` (committed in parallel the same
day, from the current-cap run) reaches the same ledger shape — unlock
`1.622ms/Present`, bare crossing `~0.3us`, "the plausible remaining value is
inside buffer mutation execution" — and requires a composition observer with
a time split before any mutation-stream design. The R-237.4 split above
delivers `.38`'s time-split requirement and sharpens its verdict: the
first-order term is not composable adjacent mutations but **full-upload
write amplification**, a per-call upload-span question that needs no
mutation reordering or composition algebra.

Backend semantics (`Pool::uploadBufferData{,Range}` and
`finalizeBufferMap`, `src/dxmt9/dxmt9_resource_pool.cpp:1451-1560`) split
the full-upload population into two different problems:

- **Managed (`8.92GB`, `1,223` calls, avg `7.3MB`)**: writable unlock
  rotates the rename ring; this run's selection ledger is `in_place 164 /
  reuse 919 / fresh 140`, i.e. `86.6%` land on a different backing that
  genuinely needs full contents under the current rename design — the full
  copy is load-bearing there. But the byte ledger proves partial-lock
  amplification: total locked bytes across ALL `38,767` locks are `5.66GB`,
  of which the range path accounts for `1.65GB`, so managed locks locked at
  most `~4GB` yet uploaded `8.92GB` twice (shadow + contents). The managed
  fix class is dirty-range tracking / deferred upload-at-use (real D3D9
  managed-pool semantics), not a range-limited immediate upload — a larger
  design.
- **Default DISCARD (`6.64GB`, `8,446` calls)**: the backing rotation
  already happened at lock time (`finalizeBufferMap`, R-BACK-5.8), so the
  unlock full copy uploads the zero-filled remainder of a freshly rotated
  backing. Contract-wise the unwritten region is undefined after DISCARD;
  range-limiting the upload (and skipping the lock-side zero-fill +
  wow64 shadow pre-copy) is the clean candidate, with the caveat that
  undefined bytes change from zeros to stale garbage for out-of-contract
  readers — the visual gate family applies. The capture read-set
  (`record.shadow`) mirror invariant must be restated for a range-limited
  writer before implementation.

## Follow-ups

- Candidate mechanisms, now sized (bounded contracts, not yet licensed):
  1. **Range-limit non-DISCARD full uploads** (Managed + plain Default
     locks): `storage_` mirrors the backend copy outside the locked span by
     construction, so uploading only `[lockedOffset_, lockedSize_)` is
     semantically sufficient if the mirror invariant is proven (every prior
     byte reached the backend before). Managed alone is `8.92GB/run` from
     `1,223` calls.
  2. **Range-limit DISCARD uploads**: prior content is undefined by
     contract, but current behavior uploads the zero-fill, so stale GPU
     bytes replace zeros for out-of-contract readers — needs the visual
     gate family (`v0.0.3` anchor), not just counters.
  3. **Skip the DISCARD zero-fill + shadow pre-copy on lock** (lock-side
     companions of 2, `d3d9_buffer_lock_shadow_copy_ms` + the
     `storage_.assign` inside lock core).
- Each candidate needs: mirror-invariant argument (or zero-fill contract
  decision), a native spec pinning the upload-range choice, and a GT2
  ABBA with visual gates before promotion.

## Lane spec: `DXMT9_DISCARD_RANGE_UPLOAD` (V1, experimental candidate)

R-237.5: Behind env `DXMT9_DISCARD_RANGE_UPLOAD` (default off, unset/`0`
selects the byte-identical full-upload path), `Buffer::unlock` routes a
writable unlock whose lock was Default-pool + Dynamic + `DISCARD` (and
whose recorded `lockedOffset_`/`lockedSize_` are in bounds) through
`uploadBufferDataRange(handle_, storage_, lockedOffset_, lockedSize_)`
instead of the full `uploadBufferData(handle_, storage_)`. `NOOVERWRITE`,
Managed, plain, and readonly paths are unchanged. V1 deliberately keeps
the lock-side DISCARD zero-fill and the wow64 shadow pre-copy unchanged
(V2 candidates, separate correctness arguments).

Correctness argument (why no ordering/formal layer):
- No command, submission, or interleaving order changes; the same single
  upload happens at the same point with a smaller span. Per
  `agents/rules/rendering_correctness.rules.md` this is a value transform;
  the formal layer is recorded as not applicable for V1.
- In-flight safety: for Default dynamic rename buffers the backing was
  rotated at lock time (`finalizeBufferMap`, R-BACK-5.8), so the in-place
  range write lands on the freshly selected backing exactly as the current
  full write does; the range write touches a strict subset of the bytes
  the full path writes today. No new hazard class is introduced.
- Content semantics: bytes outside the locked span on the new backing
  change from uploaded zeros to the rotated backing's prior content. D3D9
  defines post-DISCARD content as undefined; contract-abiding apps read
  only bytes written since the DISCARD. Out-of-contract readers are the
  risk the visual gate family covers.
- Mirror note: core `storage_` (zero-filled) and the pool capture shadow
  (range-patched) diverge outside the locked span while the env is set;
  both hold undefined-region bytes. Render Tape capture accuracy for those
  bytes is a diagnostic-lane caveat, recorded here, not a production
  contract change.

Mechanism proof counters: `d3d9_buffer_upload_range_discard_calls` /
`_bytes` split inside the existing range family, so an A/B can gate on
the mechanism being exercised and on `d3d9_buffer_upload_full_bytes`
dropping by the DISCARD share.

Evidence plan before any default flip: native spec pinning route selection
(env off = full; env on = range with exact offset/size; NOOVERWRITE /
Managed / plain unaffected), D3D9 conformance suite clean, GT1 visual
anchor sanity, GT2 matched A/B with mechanism + FPS + upload-byte gates.

### V1 A/B result — mechanism proven, FPS null, default-off kept

Runs: `app-d3d9-3dmark05-discard-range-{off,on}-gt2-r1` (same build, both
`status pass`, both `1,814` presents, `gpu_command_buffer_errors=0` both)
and `app-d3d9-3dmark05-discard-range-on-gt1-visual-r1` (GT1, pass, `7,116`
discard-range calls, normal frame verified against the visual anchor
expectations, zero GPU errors).

- Mechanism: exact. `d3d9_buffer_upload_range_discard_calls=8,440` with
  `full_default_calls` `8,382 -> 0`; total uploaded bytes
  `17.14GB -> 14.48GB` (`-2.66GB`, `-15.5%`), matching the DISCARD
  locked-span share (`3.92GB` locked vs `6.59GB` previously uploaded).
- CPU: null. Full-upload span `-459.8ms/run` but the range span rose
  `+360.9ms/run`; net upload CPU `-98.9ms/run` (`-0.055ms/present`),
  unlock core `-3.1%`. Scene fps harmonic `27.775 -> 27.679` (`-0.35%`),
  median `32.719 -> 32.810` (`+0.28%`) — inside noise.
- Attribution correction the ON run makes possible: with Default DISCARD
  isolated away, the remaining full-upload wall is **Managed alone** —
  `1,225` calls, `8.94GB`, `2,157ms/run` = `1.19ms/present` at
  `1.76ms/call` (`~4.1GB/s` across the rotation plus two `7.3MB` copies).
  Default DISCARD full uploads cost only `~460ms/run` (`~0.25ms/present`,
  `~14GB/s` marginal — memcpy-cheap); the earlier byte-volume framing
  overstated them as a wall owner.

Disposition: keep `DXMT9_DISCARD_RANGE_UPLOAD` as an opt-in default-off
candidate (inline-const-delta precedent — proven mechanism, no measured
win). Conformance and the full visual-anchor family remain required only
if a default flip is ever proposed. **The lane's real continuation is the
Managed dirty-range / deferred-upload-at-use design, now sized at
`1.19ms/present` (`~+3.3%` GT2 mathematical ceiling).** That design is
specified as `specs/backend/buffer-mutation-offload/` (R-BACK-44.1..44.8,
`DXMT9_MANAGED_MUTATION_OFFLOAD`): logical rotation stays synchronous so
commit-time capture is unchanged, byte materialization moves to the replay
FIFO at its producer-order position, and the R-BACK-2.51(d) unlock drain
is replaced by ordering for the Managed case.

**Outcome (2026-08-25, implemented as R-BACK-44):** the mode landed
default-off with its full evidence stack (`BufferMutationOffload.tla` +
counterexamples, shared predicates, transaction spec) and the GT2 matched
A/B delivered the predicted win: harmonic scene fps `27.605 -> 28.864`
(`+4.6%`), p50 `32.81 -> 31.92ms`, producer unlock CPU
`3,295 -> 1,390ms/run`, managed full uploads `1,227 -> 0`, zero GPU
errors (`mutation-offload-{off,on}-gt2-r{1,2}`). One field lesson worth
keeping: the first ON run *regressed* `-2.6%` because a conservatively
broad guard (the NOOVERWRITE bypass mutation probe, applied without a
pool check) converted ~30k/run Default NOOVERWRITE bypasses into scoped
waits — the drain-site sink attributed the `+2.30ms/present` to
`dxmt9c_buffer_unlock` in one diagnostic run, and scoping the probe to
the Managed pool restored the win. Promotion gates before any default
flip are tracked in the topic gap.
