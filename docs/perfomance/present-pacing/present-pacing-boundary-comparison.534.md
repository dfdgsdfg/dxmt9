---
domain: present-pacing
workload: STALKER CoP controlled Day high 1280x720 late spatial window
title: "Boundary-only/all-root comparison passes the finite screen; canonical transfer remains open"
type: evidence
status: matched-observer-screen-passed-default-removal-unresolved
updated: 2026-10-14
source: traces/stkcop-boundary-comparison-h534/{live-contract.md,run.py,inputs.json,clocks.json,analyze_run.py,compare.py,comparison.json,verify_raw.py,raw-verification.json,environment-note.md}; traces/stkcop-boundary-comparison-h534/{A1,B1,B2,A2}/{receipt.json,analysis.json,catalogue-result.json,post-tail.png,restore/restored.json,recovered.json}
related: present-pacing-boundary-control.533.md; present-pacing-root-wall-live.525.md; present-pacing-source-self-thin.516.md; present-pacing-validation-live.509.md
---

# The all-root diagnostic has a matched, lower-frequency control

The frozen **A1, B1, B2, A2** comparison completes without extra attempts or
discarded runs. A uses H533's boundary-only PE self reports; B uses H523's
all-root wall envelope. Both retain exactly the H516 sparse-worker provider
and H518 self-clock service. A leaves Draw/Set/getter entries untouched, but
still wraps cold object outputs and successful Present. Neither arm is an
observer-free canonical renderer.

All three whole-thread scopes pass the predeclared finite interference screen
in the full late interval and each of its five components. This is evidence
against a large, stable all-root-observer effect at this screen's resolution,
**not proof of zero observer cost, canonical transfer, or a causal bound on
any subcomponent**. The common worker observer is not tested by this contrast.

## Same-window CPU, not summed pipeline latency

Numbers are cumulative calling-thread CPU bounds divided by observed completed
frames between spatial knots 1 and 6. No interpolation or scaling of sampled
Replay durations is used. Interval widths are endpoint uncertainty, not
statistical confidence intervals.

| Arm | Completed frames | Mixed Primary CPU ms/frame | Replay CPU ms/frame | Encode CPU ms/frame | Inclusive Primary D3D9-root CPU upper ms/frame |
|---|---:|---:|---:|---:|---:|
| A1 boundary control | 106 | 55.976–57.089 | 31.861–32.172 | 32.681–33.211 | Unavailable |
| B1 all-root diagnostic | 104 | 56.320–57.381 | 31.896–32.235 | 32.447–32.760 | 19.849 |
| B2 all-root diagnostic | 104 | 56.745–57.862 | 32.318–32.813 | 33.194–33.531 | 20.277 |
| A2 boundary control | 105 | 54.975–56.049 | 32.717–33.201 | 32.892–33.127 | Unavailable |

B's root envelope includes waits, callbacks and 200 ns per root uncertainty,
conditional on the existing finite clock qualification. It is below both
worker lower bounds in both full intervals and all ten B component intervals.
That reproduces H525's diagnostic relation with two further episodes. It does
not identify exclusive PE CPU, the process-wide largest renderer owner, or a
default-path removal amount. Replay and Encode execute concurrently; their
roughly 32–33 ms totals must not be added as frame latency. Their small rank
differences are not a basis for choosing one exclusively.

## Frozen screen and retained differences

For each scope, compare each absolute A/B midpoint difference with the larger
same-arm midpoint difference plus the widest retained endpoint uncertainty.
Both opposite-order pairs must pass. The table implements exactly that rule;
the threshold was not adjusted after observing the data.

| Scope | A1−B1 midpoint | A2−B2 midpoint | A/A difference | B/B difference | Widest uncertainty | Screen threshold |
|---|---:|---:|---:|---:|---:|---:|
| Primary | −0.319 | −1.791 | 1.020 | 0.452 | 1.117 | 2.138 |
| Replay | −0.049 | +0.394 | 0.942 | 0.500 | 0.495 | 1.437 |
| Encode | +0.343 | −0.353 | 0.064 | 0.759 | 0.529 | 1.288 |

Units are ms/observed completed frame. In particular, the A2−B2 Primary
interval is **−2.887 to −0.695 ms/frame**: do not rewrite a passed, relatively
coarse screen as equality of CPU costs. Component endpoint widths are wider;
all component values and verdicts are retained in `comparison.json`. No
confidence level or equivalence margin is inferred from this four-run sample.

Observed late cadence is 15.247–15.254, 15.311–15.317, 15.155–15.161 and
15.053–15.059 FPS in execution order. This is not a clean FPS baseline or a
README update. Background appstoreagent was observed near 88–98% CPU outside
the selected intervals; launchd and other host activity were also present.
No system service was stopped, no host CPU estimate was subtracted, and no
common-mode interference was ruled out. Equal cadence alone would not qualify
observer transfer.

## Scope, verification and restoration

- Sikarugir CX24.0.7, Day/high 1280x720, max-threads 2, snd_targets 16,
  draw-command limit 0, production-counter occlusion; product perf/frame/
  supply/capture observers OFF. These are controlled experiment policies, not
  claims about every untuned application default.
- All seven camera knots qualify per run. Native PID/birth/TID and current
  attachment epoch are checked separately in each process. There is no UI
  interaction within the selected interval.
- `verify_raw.py` independently decodes the binary rows and recomputes all
  **72 thread intervals and 12 root envelopes** without importing either
  analysis helper. `compare.py` checks input/receipt hashes, execution order,
  arithmetic and restoration; its screen has small deterministic test cases.
- Every game exits zero. Catalogue exit 1 has only `missing_capture`, covered
  by the frozen administrative qualification; this is **not a catalogue pass**.
- Post-tail screenshots are visually coherent. Computer-use is limited to
  pre-window focus requests and post-window screenshots; these are not a
  whole-run pixel oracle.
- Each run hash-restores 14 existing shared files and recovers four temporary
  service files after owned exit. Canonical renderer sources and images remain
  unchanged. No candidate, default switch, commit, push or FPS update occurs.

## Decision and still-open objective

The useful result is a repeatable diagnostic ordering: mixed Primary is large,
but its wait-inclusive D3D9-root envelope is smaller than either dedicated
worker. More attempts to order Replay versus Encode by fractions of a
millisecond are not warranted. Nor does this reopen H508's rejected live
economy gate.

The remaining work has two distinct axes: qualify residual-observer transfer
before making a **canonical/global ownership** claim, and test a **distinct,
semantics-preserving work removal** on the true late input before assigning a
savings budget. Prefer removal of a complete repeated traversal or intermediate
representation over another tiny binding cache; earlier cache experiments did
not establish a large effect. A passed observer screen alone closes neither
axis. The largest default single-thread owner and correctness-qualified
removable amount remain unresolved.
