---
domain: present-pacing
workload: STALKER Day/high 1280x720 late-window episode
title: "Default-policy image: same-episode CPU owner bounds"
type: evidence
status: observed-owner-qualified
updated: 2026-10-16
source: traces/stkcop-default-owner-h571/{contract.md,build.json,coverage-audit.md,capture-verification.json,live-verification.json,raw-verification.json}; traces/stkcop-default-owner-h571/A0/{analysis.json,receipt.json,self.bin,workers.bin,post-tail.png}
related: present-pacing-shader-count-repair.570.md; present-pacing-owner-live.564.md; present-pacing-clear-live.559.md
---

# Encode leads the observed population, not a certified default maximum

H571 composes root, Replay, Encode and other worker-body observations in one
predeclared A0 episode. It removes the dormant H549 Clear candidate and H568
failure logging from the private image while retaining the H570 shader-count
repair. Archive comparison against canonical finds only six runtime observer
members and two provider members (Replay observer and shader repair); the
bridge archive is identical. PE uses the H563 root observer.

Day/high 1280x720, max-threads2 and snd_targets16 are controlled application
settings. Command limit 0 and production-counter occlusion match their current
unset defaults. Product diagnostics are OFF, but private ownership observers
remain ON. This is a default-policy diagnostic image, not observer-free timing.

## Same-episode bounds

The selected knots 1–6 contain 128 completed frames. Values are enclosing
self-CPU intervals or conservative root/body uppers, not confidence intervals.

| Owner | CPU ms / observed completed frame | Interpretation |
|---|---:|---|
| Encode | 29.390–29.727 | Self-thread CPU interval |
| Replay | 28.144–28.522 | Self-thread CPU interval |
| Primary, mixed | 48.015–48.836 | Game + Wine + renderer; not renderer-only |
| Primary renderer roots | ≤17.502 | Inclusive elapsed upper, including waits/callbacks and endpoint allowance |
| Largest other observed worker body | ≤0.6945 | Per-body upper, not sum or whole foreign-thread CPU |

Encode's lower bound exceeds Replay's upper bound by 0.868 ms/frame in the
full interval. Encode separates from Replay in two of the five component
intervals; the other three overlap. Root and remaining body uppers stay below
both main workers throughout the full interval and components. Do not claim
Encode is always the larger worker or sum parallel CPU into frame latency.

The observed root envelope contains 1,957,605 calls. Its allowance is not
measured observer overhead. Likewise, neither the mixed Primary difference
nor a process-CPU residual identifies an unobserved renderer owner.

## Verification and restoration

- Original-capture observer OFF/ON preserves exact output SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`:
  seven sources, 670 created/released wrappers and matching query checks.
- Live game exit 0, no timeout, all seven spatial knots. The post-tail image
  shows coherent building geometry; it is not a full-run pixel oracle.
- Catalogue exit 1 is solely the predeclared `missing_capture`: the delayed
  capture lies outside this bounded episode. No other failure is waived.
- Fifteen worker instances / 656 rows; no unbounded observed worker body.
- Separate raw arithmetic recomputes all six intervals, three self owners,
  root envelopes and worker bounds without importing analysis helpers;
  108 self/body checks pass, plus six root checks and artifact hashes.
- All 14 shared files restored, four temporary services recovered, and owned
  Wine processes exited. Shared staging is not left on this diagnostic image.

The collector retains `timing_qualified=false`: it validates correctness and
coverage. The separate `analysis.json` supplies the limited observed-owner
timing authority. Neither report certifies observer-free default transfer.

## Remaining decisions

1. Quantify observer/default transfer in a separately declared comparison;
   do not extend A0 after seeing its result. Root entry work is high-frequency,
   so disabling only worker-body checkpoints is insufficient.
2. Cover foreign dispatch callbacks and closure startup/destruction outside
   measured bodies. `execute_on_main` and the ordinary mipmap shared-event
   listener remain concrete scope gaps; cached predicates do not prove zero
   calls in this episode.
3. Reprice the failure-safe Clear candidate on matched current images.
   H559's 1.579–2.072 ms/frame savings are historical matched-image evidence,
   not a removable budget for H571.

The close Replay/Encode costs make large whole-pipeline gains from an
Encode-only reduction uncertain. This observation selects the next comparison;
it does not establish a throughput ceiling, FPS gain, global CPU maximum or
safe removable amount. The full investigation remains open.
