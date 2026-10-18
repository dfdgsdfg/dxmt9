---
domain: present-pacing
workload: STALKER observation preparation, synthetic PE32 marker fixture
title: "H631 — return-address-preserving guest marker passes bounded Wine ABI checks"
type: evidence
status: synthetic-abi-qualified-game-installation-open
updated: 2026-10-16
source: traces/stkcop-guest-shim-h631/qualified/{shim.S,fixture.S,test.c,build.py,build.json,run.py,run.json,fixture.stdout,mutant-eax.stdout,mutant-fp.stdout,mutant-error.stdout}; traces/stkcop-guest-shim-h631/diagnosis/{run.json,fixture.stdout}; traces/stkcop-guest-shim-h631/{verify.py,verification.json,marker-disassembly.txt}
related: present-pacing-guest-boundaries.630.md; present-pacing-owner-union-live.629.md
---

# H631 — qualify the marker before instrumenting the game

A private synthetic x86 PE fixture passes under Sikarugir Wine. Its entry
marker preserves the original callee-visible return address, EAX input,
general registers, tested flags, Win32 LastError and x87/SSE state. Three
deliberate preservation omissions are detected. **No game hook, event logger,
CPU result or performance improvement is established.**

The original `xrEngine.exe` and `xrRender_R2.dll` retain H630's SHA-256 values.
There is no DLL staging or product-source change. This fixture does not run
the STALKER workload; direct Wine invocation is only for the standalone ABI
test. All four qualification processes exit, and postflight finds no remaining
Wine processes.

## Marker shape

At a synthetic five-byte `CALL rel32`, the fixture installs `JMP rel32` to a
marker while no other thread can execute that site. The marker saves flags,
GPRs, LastError and aligned FXSAVE state, clears DF, and calls a hostile test
observer. It restores the saved state, pushes the **original** call return
address, then jumps to the original target. There is no marker return frame
left beneath the target, unlike a conventional call wrapper.

This shape is relevant to H630's hidden EAX dispatcher input and End tail
jump. It is only a **before-call marker**, not an after-return timing wrapper.
The eventual coarse phases would be before Begin → before dispatch → before
End → caller continuation. These include inter-call work; failed Begin skips
dispatch and End. They are not exact individual function durations or complete
D3D9 activity coverage.

## Actual qualification

The PE32 binaries use llvm-mingw `i686-w64-mingw32-clang`, `-O2`,
`-Wall -Wextra -Werror`, `-msse2 -mno-avx`. Frozen source/image hashes and
commands accompany each run. This is functional testing, not a timed A/B.

| Check | Outcome |
|---|---|
| 128 arithmetic/DF flag combinations × normal/tail paths | 256 paired state cases pass |
| Target and observer invocation counts | Exactly once per selected arm |
| Callee-visible return address and caller stack sentinel | Match the original CALL path |
| Target `longjmp`, baseline and marker arms | Two cases pass; fixture restores caller FP state before entering C |
| Wrong expected patch bytes | Rejected without changing the call |
| Original instruction restoration | Pass after each redirected arm |
| Omit EAX restoration | Exit 7, EAX input becomes zero |
| Omit FP restoration | Exit 8, FP state comparison fails |
| Omit LastError restoration | Exit 7, error becomes observer's `0xdeadbeef` |

FP comparison includes architectural FXSAVE fields, x87 register payloads and
eight XMM registers, excluding reserved bytes/padding. The observer deliberately
changes volatile registers, flags/DF, LastError, x87, MXCSR and SSE registers;
its nonvolatile GPRs obey the marker's callback contract. Postflight also
checks the emitted 20-instruction marker and authenticates the frozen inputs,
four outcomes and unchanged game images.

### Preserved fixture failure

The initial test failed **after** the call while all before-call state matched.
A diagnostic rerun isolated the difference to the post-call stack word:
`00000000` versus `0012fa5c`; registers, flags, LastError and FP state matched.
That word was the C driver's saved EDI, which legitimately differed between
arms, not a target-owned stable input.

The corrected fixture seeds and checks an explicit caller stack sentinel.
The initial and diagnostic failures remain under `initial-failure/` and
`diagnosis/`; `qualified/` contains the corrected population. This is a test
defect correction, not discarding an unfavorable performance sample.

## Remaining gates before game observation

- Qualify the final continuation marker at engine RVA `0x538ac`; it starts
  with a six-byte instruction, not one of the three five-byte calls. Preserve
  both normal End return and failed-Begin paths.
- Add bounded per-thread event storage and phase validation. Missing events,
  overflow, nesting, wrong identity/thread and aborted phases must invalidate
  attribution rather than manufacture a duration.
- Define actual observer FP/exception behavior. The tested observer initializes
  its own FP environment; the marker does not yet supply a clean C FP
  environment. FXSAVE does not preserve AVX upper state. Logger faults,
  arbitrary SEH/unwind, stack exhaustion and general nonlocal exits are not
  qualified by two controlled `longjmp` cases.
- Qualify installation before the game can execute the sites, including loaded
  image identity, exact bytes, instruction-cache publication and rollback.
  Single-thread self-patching is not proof of safe concurrent game patching.
- Join CPU endpoints to the existing qualified native clock/thread identities.
  Wall timestamps alone cannot separate game, Wine, PE and synchronous Unix CPU.
- Freeze unmodified/inert-marker/armed controls and price observer perturbation
  before interpreting any live timing. No live population was started here.

Use decompilation to locate boundaries, not as source to rebuild or redistribute
the game DLL. Prefer a reversible private marker with binary records over text
logging per draw. H629's default-path owner transfer and positive removable CPU
budget remain open; the ABI fixture discharges neither obligation.
