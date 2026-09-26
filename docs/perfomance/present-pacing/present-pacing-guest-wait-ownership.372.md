---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H372: Guest engine waits reach the OpenAL global critical section"
type: evidence
status: partial-waker-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-guest-wait-h372/{debugger.log,receipt.jsonl}; traces/app-d3d9-stkcop-guest-wait-h372b/{snapshot.txt,analysis.json}; traces/app-d3d9-stkcop-guest-wait-h372c/{snapshot.c,snapshot.txt,analysis.json,ownership-receipt.json,static_receipt.py,run.py,receipt.jsonl}; experiments/output/app-d3d9-stkcop-bench-guest-wait-h372{b,c}/result.json
related: present-pacing-tuned-tail-wait.371.md; present-pacing-xray-worker-yield.369.md
---

# H372: Resolve guest wait objects before changing dxmt9 scheduling

After commit `b8d77452`, bounded guest snapshots connect the residual engine
waits to the bundled `wrap_oal.dll` global critical section. This is a concrete
audio-side supply-stall candidate, not proof that all tail stalls or audible
interruptions have the same cause. No runtime policy or audio DLL is changed.

## Method and qualification

The first WineDbg minidump attempt failed: the debugger reported a WoW64 page
fault at EIP `fff868c2`, read address `6`; no dump appeared and the experiment
failed with app exit 255. Debugger exit zero was not success. Preserve this as
failed diagnostic evidence, not a confirmed dxmt9 regression or a precisely
attributed debugger bug.

H372b/c instead use an original external 32-bit PE helper: Toolhelp enumeration,
thread start-address query, SuspendThread, GetThreadContext, bounded stack and
register-point ReadProcessMemory, then ResumeThread. Storage is preallocated;
formatting occurs after resume. There is no debugger attachment or injected
remote thread. H372c additionally reads EDI-16/ESI-16 to recover lock headers.

Both Day/high 1280x720 runs use `-max-threads 2`, perf diagnostics, and eight
snapshot rounds near observed frame 2700. Both PASS with exit zero, no reported
GPU command-buffer errors, and identical staged artifact hashes. Each has 64
thread snapshots, all with `prior=0`, `resumed=1`, `context=1`. Some stack reads
are explicitly unavailable (`bytes=0`). These invasive samples are not FPS
qualification. Frame-pointer walking cannot unwind optimized FPO frames;
recorded `args` are raw stack words, not established typed arguments.

## Identity-qualified ownership

The following identities belong to H372c guest process `0xc4`; addresses and
TIDs are run-local, not reusable constants.

| Role | Guest TID | Start / observed execution |
|---|---|---|
| Primary | `0xc8` | Start `xrEngine.exe+0x6cdc1` |
| Secondary | `0x144` | Start `MSVCR80.dll+0x5c980`; engine secondary-loop return sites |
| WinMM callback thread | `0x13c` | Start `winmm.dll+0x13b00`; repeated PCs in `wrap_oal.dll+0x1bfcd..0x1c0c8` |
| Yield helper | `0x108` | Start `xrCPU_Pipe.dll+0x3730`; closes H369's inferred module identity |

| Round | Waiter | Critical section | Observed owner | Guest return evidence |
|---|---|---|---|---|
| 0 | Primary `0xc8` | `0x038a6520` | `0x13c`, recursion 2 | `wrap_oal+0x7c9b` |
| 0 | Secondary `0x144` | `0x01775b60` | Primary `0xc8` | `xrCore+0x26ce9`, engine return word `0x45302c` |
| 4 | Primary `0xc8` | `0x01775b60` | Secondary `0x144` | `xrCore+0x26ce9`, engine return word `0x453908` |
| 4 | Secondary `0x144` | `0x038a6520` | `0x13c`, recursion 2 | `wrap_oal+0x7c9b` |

These repeated dependency observations give two patterns:

```text
Secondary → engine lock → Primary   → OpenAL global lock → WinMM callback owner
Primary   → engine lock → Secondary → OpenAL global lock → WinMM callback owner
```

They are **not an atomic wait-for graph**: each thread is suspended separately
and other threads continue. Do not infer a deadlock, continuous hold duration,
or an observed wake event by joining the rows. Another engine critical section,
`0x01775ba0`, has Secondary waiting on Primary in rounds 3/7; do not collapse
the two engine objects. Primary also executes Lua, rendering, PE/bridge and
collision code in other rounds, so audio is not its only observed activity.

## Static lock and release binding

`wrap_oal.dll` loads at `0x037a0000`; its global lock has RVA `0x106520`.
The installed binary's PE import table starts KERNEL32 IAT at RVA `0x1d000`:
entries 2/3 are `EnterCriticalSection` / `LeaveCriticalSection`. Disassembly
binds the small wrappers unambiguously:

- RVA `0x7c90`: push preferred-base address `0x10106520`, call IAT `0x1d008`.
  Return PC `0x7c9b` is exactly the observed wait frame.
- RVA `0x7ca0`: push the same lock, call IAT `0x1d00c`.

The x86 critical-section header read at `0x038a6520` contains LockCount 2,
RecursionCount 2 and OwningThread `0x13c`. Engine waits return through exported
`xrCriticalSection::Enter` at `xrCore+0x26ce0` (return `+0x26ce9`). The common
lower wait chain is `ntdll+0x56d1c → +0x5c6cc → +0x5c89d`.

The release obligation therefore belongs to the observed lock owner and must
unwind recursive ownership before another thread can acquire it. The static
Leave wrapper is **not** a recorded unlock/wake/resume event. Private callback
function names are unresolved; nearest exported disassembly labels must not
be treated as names of these internal routines. The receipt preserves binary
SHA-256, import/disassembly output and selected snapshot records.

## Decision and remaining evidence

Prioritize the OpenAL critical-section owner's execution/hold time and the
unlock → Wine wake → Primary/Secondary resume edge before another dxmt9 queue
or encode-policy change. H371 input starvation now has concrete guest lock
dependencies, but these separate diagnostic windows cannot be combined into
an exact critical-path percentage.

Next attribution needs bounded, identity-qualified timing of that lock's
recursive acquire/release and owner work, with audio timestamps if audible
causality is claimed. An audio-provider/settings comparison would be a separate
experiment preserving sound semantics, not an implicit authorization to replace
the bundled DLL. STALKER worker tuning remains workload-local; no README FPS
update or general Wine/audio workaround follows from these snapshots.
