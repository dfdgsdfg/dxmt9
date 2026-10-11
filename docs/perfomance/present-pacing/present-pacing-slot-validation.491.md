---
domain: present-pacing
workload: STALKER CoP H481 original Normal source, x86 release observed replay
title: "Full-Slot validation repeats 84,587 command visits in 208 calls"
type: evidence
status: measured-candidate-no-removal-or-default-transfer
updated: 2026-10-11
source: traces/stkcop-slot-validation-h491/{run.py,build-receipt.json,build-x86-final.log,tests-x86.log,working-tree.patch,trace-r1.txt,trace-r2.txt,off.txt,off.stderr}; traces/stkcop-slot-validation-h491/{trace-r1,trace-r2}/{supply.csv,caller.json,analysis.json}
related: present-pacing-provider-stack.490.md; present-pacing-provider-phases.489.md; present-pacing-root-game.477.md
---

# Repeated validation is real, but its savings are not yet proven

The H490 product candidate now has direct same-input evidence. Two fresh
executions each call the full-Slot validator 208 times and visit 84,587 command
headers successfully. Their validator CPU envelopes are 3.166–3.387 ms over
the entire 92-raw captured interval, not per Present. No check is removed and
no rendering policy is changed.

## Instrumentation boundary

`ChunkSlot::commandPayloadsInRange` retains its complete implementation. Behind
the existing cached CPU-supply trace gate, it consults a scoped TLS callback.
The cold observed provider installs that callback only around selected replay;
the callback suppresses itself, calls the same validator, and records its bool
result. RAII restores nested callbacks on return or unwinding. There is no
Slot field, new ABI, queue lock, resource lifetime token, or validation bypass
in the production policy. OFF does not perform the TLS lookup.

Each diagnostic attempt records stored DrawParam extent, command-header extent,
and begin/end CPU and wall clocks. Addresses qualify diagnostic attempts only,
not Slot generations or resource identity. `analyze_prepared_replay_cpu.py`
requires complete same-thread nonoverlapping attempts nested in provider phase
1→2, then joins the enclosing raw to its original event ordinal. Missing,
duplicate, cross-thread, escaped and invalid-clock evidence rejects.

Only accepted calls guarantee every command header was visited. DrawParam
storage size is **not** a visit count: runs can overlap and rejected validators
can stop early. The observer does not perform a second Slot traversal to derive
statistics. Its CPU envelope includes clock/event overhead, is neither an OFF
cost nor a removable-work measurement, and excludes validator calls outside
the synchronous observed-provider scope.

## Results

| Entire original interval | Trace 1 | Trace 2 |
|---|---:|---:|
| Original raw calls | 92 | 92 |
| Validator calls / accepted / rejected | 208 / 208 / 0 | 208 / 208 / 0 |
| Successful command-header visits | 84,587 | 84,587 |
| Sum of stored DrawParam extents | 519,804 | 519,804 |
| Maximum command-header extent | 556 | 556 |
| Validator CPU, observer-inclusive | 3.387 ms | 3.166 ms |
| Planned replay CPU, including V4 and observer | 54.184 ms | 50.397 ms |

This establishes repeated prefix traversal, not a claim that 519,804 distinct
draws executed. The planned scope still includes capture/identity qualification;
do not subtract these numbers from another run, scale H490 wall samples into
CPU fractions, or infer an FPS improvement. Even eliminating this complete
measured envelope would address only that envelope; a correct replacement
still needs suffix validation and witness maintenance.

## Verification and provenance

Base commit is `5367d3e8` plus the archived working-tree patch. The full x86
release build and seven focused Meson tests pass: direct-slot provisioning,
production routing, provider V4, provider observation V4, both Python analyzers,
and install-name audit. The Python suites cover 16 prepared-replay and 42 supply
cases. Native validator tests preserve both empty acceptance and malformed
payload rejection, delegate once, and restore nested diagnostic scopes.

Two diagnostic executions and one OFF execution preserve all existing original
oracles: Normal policy, 16 restored surfaces, 12 aliases, balanced 779 wrappers,
and three original source cuts. All source/output readbacks match SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
The runner and both runtime image hashes are in `build-receipt.json`; the OFF
dyld log confirms their actual paths. The Metal skill's doctor/readback workflow
was used (Xcode 27, Apple M1 Metal 4), without Wine staging, UI interaction,
gputrace, or concurrent builds during measurement. This is x86 validation, not
a claim that the native ARM build was refreshed.

All-resource content authority remains unknown and timing qualification remains
false in the original oracle. No live-game global CPU attribution is claimed.

## Next decision

Retain validation until a generation-qualified prefix witness proves that
already-checked commands, state/uniform ranges and payload storage cannot be
invalidated by append, rollback, Slot rotation or reuse. A bounded witness
candidate can then compare complete validation against prefix reuse plus suffix
validation on this same input, retaining malformed-storage counterexamples and
the original GPU oracle. It must pass observer-OFF paired CPU/noise gates before
promotion. This is a concrete small-to-moderate candidate, not evidence that
full-Slot validation explains the overall STALKER performance gap or that the
largest default-game single-thread owner has been found.
