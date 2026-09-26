---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H374: OpenAL waiter to waker to wait-return trace"
type: evidence
status: partial-hold-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-wake-sync-h374/{run.py,receipt.jsonl,snapshot.txt,analyze.py,analysis.json}; experiments/output/app-d3d9-stkcop-bench-wake-sync-h374/{result.json,dxmt9.log}
related: present-pacing-openal-lock-sampling.373.md; present-pacing-guest-wait-ownership.372.md
---

# H374: Most observed delay precedes the audio owner's wake request

Use the installed Wine's existing `WINEDEBUG=-all,+timestamp,+pid,+sync`
diagnostics, without changing Wine, game DLLs or dxmt9. DTrace probe discovery
was unavailable because it requires additional privileges; no privilege or SIP
change was attempted. Existing sync traces expose object-qualified
`RtlWaitOnAddress` entry/return, `RtlWakeAddressSingle`, and target-qualified
`NtAlertThreadByThreadId`. This closes a temporal edge missing from H372/H373.

The Day/high 1280x720 `-max-threads 2` run passes, exits zero, reports zero GPU
command-buffer errors and stages the same five binaries as H373. Whole-run
Wine logging plus H372's bounded guest snapshot helper perturb timing; this
is not observer-OFF FPS qualification. No global environment or prefix setting
was changed.

## Object and thread binding

In guest PID `0xc4`, this run loads `wrap_oal.dll` at `0x039a0000`. Its lock is
`0x03aa6520`, and the lock's address-wait field is `0x03aa6530` (lock + 16).
Do not reuse the absolute address from H372/H373. Snapshot TID `0x13c` starts
at `winmm+0x13b00` and executes `wrap_oal+0x1c0bf` in round zero.

The parser tracks active address waits per guest TID, matches the waker's
nearest preceding address wake to that object and target alert, then consumes
the matching wait return. It preserves raw line identities for review.

| Completed object-qualified waits | Whole diagnostic run | Five-second log-time window after frame 2700 |
|---|---:|---:|
| Count / matched alert chains | 1,724 / 1,724 | 114 / 114 |
| Primary / Secondary waiters | 239 / 1,485 | 0 / 114 |
| Waker | `0x13c` for every matched chain | `0x13c` for every matched chain |
| Wait-entry → return median / maximum | 10 / 31ms | 10 / 29ms |
| Alert-request → return median / maximum | 0 / 11ms | 0 / 0ms |

Of all 1,724 matched waits, 1,720 return in the same millisecond timestamp
bucket as the alert. Zero does **not** mean zero latency. Wine's trace precision
is one millisecond, and logging/scheduling perturb it. The five-second window
uses Wine timestamps after the frame marker in the combined log; it is not an
exact Present interval or a join to supervisor clock timestamps.

One directly reviewed sequence (Wine time, guest IDs):

```text
582999.532  Secondary 0144  RtlWaitOnAddress(03AA6530)
582999.532  Secondary 0144  NtWaitForAlertByThreadId
582999.544  WinMM     013c  RtlWakeAddressSingle(03AA6530)
582999.544  WinMM     013c  NtAlertThreadByThreadId(0144)
582999.544  Secondary 0144  RtlWaitOnAddress returning 0x101
582999.544  Secondary 0144  next RtlWaitOnAddress(01975BC0)
```

This is observed waiter execution after the wake request, not merely a static
release obligation. It also shows that returning from the audio wait need not
immediately produce render work: the Secondary can next enter an engine wait.
The `NtWaitForAlertByThreadId` argument is zero in this runtime; object identity
must come from the surrounding `RtlWaitOnAddress`, not that syscall argument.

## What this resolves, and what it does not

The observed typical delay is before the WinMM owner's wake request, not a
large post-alert scheduler delay. This strengthens investigation of callback
work while owning the OpenAL lock over an assumption that Wine merely wakes
the game late. Rare whole-run post-alert outliers still exist.

There are no outermost Enter/Leave timestamps or recursion events here, so the
table does not measure total lock hold duration, prove fair lock acquisition,
or give a complete scheduler-delivery trace. `0x101` is a wait-return status,
not proof of completed D3D work. No audio waveform/timestamp correlation or
fraction of the frame critical path is established.

Next attribute the callback's pre-wake work (audio mixing/resampling versus
downstream audio calls) using matched execution evidence. A later policy or
audio-provider comparison must preserve sound semantics and use observer-OFF
reversed-order runs. Do not remove engine/audio locks or change dxmt9 queue
policy on the strength of this diagnostic.
