---
domain: present-pacing
workload: STALKER CoP Day
title: "H328: covered-Pending ON wild run"
type: evidence
status: measured
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r653-day/result.json; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r653-day/run-policy.json; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r653-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r653-day/actual.png; experiments/output/app-d3d9-stkcop-bench-covered-pending-on-r653-day/xrEngine_dxmt9.log
related: present-pacing-query-mutex-owners.327.md
---

# H328: candidate ON passes one wild run; performance delta remains unknown

Commit `92ac354c`, Sikarugir cx24.0.7, Day High `renderer_r2.5`, 1280x720
windowed, perf profile, validation OFF, Info logging and health counters ON.
`DXMT9_QUERY_COVERED_PENDING=1`, actual occlusion and asynchronous fences ON;
END coalescing, query/receipt/backend/split observers, queue mutex split and
frame sampling OFF. `run-policy.json` records the explicit launch settings.
The canonical runner uses timeout 300s, capture delay 120s and suffix
`covered-pending-on-r653-day`.

An initial staging attempt failed before Wine launch: prefixing PATH with
llvm-mingw caused native framework checks to select an incompatible linker for
the Xcode 27 SDK. A full Unix build with the normal native PATH and dynamic
SDKROOT succeeded; the retry staged normally. No runtime source change was
needed. All build/test work finished before launch; spot process checks found
no concurrent Ninja/TLC/profiler. User/OS activity and continuous foreground
residency are not proved. No focus restoration was performed.

## Result

- Runner PASS, returncode 0, no timeout, capture error or failure markers.
- 2,511 benchmark frames, reconstructed duration 128.108s, **19.601 FPS**.
- Whole-process duration 227.654s, 2,600 submitted Presents including loading.
- All eight health counters present and zero: chunk rejection, wire rejection,
  GPU command-buffer errors, missing-pipeline draws and four pipeline failures.
- Captured app window shows buildings, vegetation, road and characters without
  obvious missing geometry or black-screen corruption. One still image cannot
  verify flicker or full-sequence equivalence. The user subsequently confirmed
  that the run worked correctly; this is qualitative wild confirmation, not
  frame-aligned pixel equivalence.

| Reconstructed interval | FPS | Maximum frame ms |
|---|---:|---:|
| 40–50s | 34.70 | 73.9 |
| 70–80s | 18.00 | 429.1 |
| 90–100s | 6.60 | 430.3 |
| 100–110s | 8.30 | 333.5 |
| 110–120s | 10.30 | 339.9 |

The late slowdown remains. Windows use `summarize_stkcop_fps.py`'s reconstructed
frame completion time, not a cross-run scene identity. There is no matched OFF
run here: neither a speedup nor a regression can be attributed to the candidate.
Observer-OFF runtime does not count fast-path hits; this run proves enabled
configuration and integration, not its hit rate. The native gated fixture owns
direct evidence that eligible polls bypass the queue mutex.

## Staged identity and decision

All five staged size/hash prefixes match canonical build outputs:

| Image | SHA-256 prefix |
|---|---|
| PE32 d3d9 | `53a8bb8566dbf2a8` |
| PE32 bridge | `703c1c7add89c02d` |
| PE64 d3d9 | `461664d2e48a226a` |
| PE64 bridge | `d4b4f31c1f67da33` |
| Unix provider | `5989dc2f8e197a36` |

ABI handshake succeeds. Candidate is ON for this run only; the code default
remains OFF. Do not infer default promotion or whole-frame correctness from a
single spot image and clean health counters. User qualitative confirmation is
now recorded. Matched same-image OFF/ON evidence and an explicit promotion
decision remain open.
