---
domain: present-pacing
workload: 3DMark06 GT2
title: "Lock-scope decline attribution: historical binary recovery"
type: evidence
status: promotion-withheld
updated: 2026-09-09
source: experiments/output/lock-scope-H311-20260909/analysis; experiments/output/lock-scope-H309-20260909/analysis; experiments/output/lock-scope-H309-20260909/fixture/winemetal_dxmt9.so; experiments/output/lock-scope-H310-20260909/batch-cost
related: present-pacing-lock-scope-wild.309.md; present-pacing-payload-count-batch-cost.310.md
---

# Lock-scope decline attribution

The large [H309](present-pacing-lock-scope-wild.309.md) 06 GT2 deficit is not
reproduced by the exact historical provider in this follow-up. Both the old
and current providers recover to about 18 FPS. Do not attribute the earlier
5.95–9.31 FPS population to a deterministic cost of removing locks, or credit
[H310](present-pacing-payload-count-batch-cost.310.md)'s payload fix with the
recovery. Run environment/execution state is a major confounder; the exact
external process, cache, frequency or scheduling contribution is unresolved.

`DXMT9_LOCK_SCOPE_REFINEMENT` stays default-off. This is a bounded attribution
result, not a statistical non-regression or broad correctness promotion.
README is unchanged.

## Frozen-image comparison

All runs use the H309 official 06 GT2 recipe: Sikarugir cx24.0.7, 1280x720,
Warn/perf frame sampling, replay offload ON, Tape/segmented/inline delta/opaque
depth-index optimization OFF, lock and native CPU observers OFF. There are
no concurrent project builds, tests or other benchmarks from this task.
The host was asked to remain idle, but no explicit user confirmation was
received; these are lower-background-load runs, not guaranteed host quiescence.

H311 `analysis/run.py` inherits the H309 driver with a new output label and
adds a process-identity-qualified `ps` sample every ten seconds. `run_old.py`
also supplies an explicit nested `--unix-build-dir` artifact tree. It does
not replace any canonical build image. Both drivers retain the source/build,
Wine, all-five-image and staged-image post-run guards. Historical binary
origin and current live-source guards are separately identified in recipes.

The historical provider is the retained H309 pixel-fixture image, not a
rebuild from today's source. Its SHA256 is
`587db86dd4798d10a7e6cf1de31fed78900e59dcd8999737582f49f3cc796dd7`.
The current H310 provider SHA256 is
`f02452eccc4c81f67a437171b683cd2a1dc25c662ba31f79c1399379fef07c77`.
The four PE DLL hashes and build configurations are identical. The source
receipts differ only in the payload-count implementation/helper and its native
test. HEAD remains `1f3f0358` plus the documented uncommitted overlay; neither
provider is described as bare HEAD. An independent read-only audit found no
recipe, image or historical staging mismatch.

`analysis/summarize.py` rechecks recipe and artifact hashes and reconstructs
scene rows from consecutive positive-wall frames with `draw_calls > 100`.
These are frame-sampled perf results, not fully observer-free `.3dr` FPS.

| Execution order | Provider / gate | Frames | FPS | p95 / p99 ms | Uncovered health tail |
|---:|---|---:|---:|---:|---:|
| 1 | H310 OFF1 | 1164 | 18.145 | 74.139 / 100.774 | 39 |
| 2 | H310 ON1 | 1179 | 18.424 | 72.748 / 98.340 | 14 |
| 3 | Historical H309 ON1 | 1175 | 18.347 | 73.174 / 99.145 | 35 |
| 4 | Historical H309 OFF1 | 1172 | 18.302 | 72.655 / 96.218 | 0 |
| 5 | H310 OFF2 | 1166 | 18.200 | 73.270 / 92.819 | 40 |

The final OFF repeat differs from the first by +0.30%, unlike H309's large
within-session drift. Historical ON/OFF differs by +0.24%; current ON is
+1.23–1.54% relative to its two OFF controls. These spots show no large
deficit, not a demonstrated FPS gain or statistical zero-regression bound.
All five official result artifacts pass, with zero values in all eight
checked health counters at their covered endpoints. The table explicitly
excludes unobserved health tails. All complete the approximately 64-second
scene before the expected result-GUI timeout (exit 143). There is no fresh
05 repeat, full temporal pixel comparison, or current application-thread CPU
qualification in this follow-up.

The last run restages the current five images. A subsequent full provider
SHA256 check confirms the shared Wine runtime again matches H310, not the
historical fixture. No canonical build was overwritten.

## What the evidence does and does not identify

1. **The earlier large deficit is not a stable property of the binary.**
   H309 ON previously measured 6.773 and 5.952 FPS; the exact same provider
   now measures 18.347. H309 OFF also drifted 9.308 to 6.553 within that
   session, and 05 OFF drifted 19.858 to 16.615. Those observations invalidate
   treating the original pair differences as independently caused by the
   compound lock-scope candidate. They do not prove zero candidate cost under
   all contention patterns or identify one external process as the sole cause.
2. **The payload recount was real unnecessary work, but not the demonstrated
   cause of this recovery.** Current ON is only 0.42% above historical ON in
   these non-randomized spots. There is no isolated FPS gain claim for H310.
3. **CB/pass fragmentation is not the observed signature.** H309's old slow
   population and the new population both stay near four CB and 19.86 passes
   per frame. The current OFF1/ON1 GPU CB averages are 12.016/11.828 ms/P,
   versus H309's approximately 14.84–15.11 ms/P. Even GPU elapsed time changed
   across sessions; stable CB counts do not isolate host frequency, cache or
   shared-system effects.
4. **Singleton bookkeeping loses in the synthetic Pool probe, but is not an
   explanation for the large FPS deficit.** Multiplying H309's 3,867.5 ON
   UpdateMany calls/P by H310's largest cross-run singleton delta (69.36 ns)
   gives about 0.268 ms/P. This is only an all-singleton sensitivity estimate,
   not a bound on production cost: batch distributions, initialization reuse,
   pre-lock walks and contention are unknown. Actual x86_64 batches of 8–64
   amortize uncontended locks; deleting batching globally is unsupported.

H309 launch snapshots show substantial Orca Renderer and WindowServer CPU.
H311 additionally records cumulative CPU deltas using PID plus process birth
identity, not differences of decayed `%cpu`. The traces span the whole launch
and result collection, not an exact scene interval; transient unobserved
processes and sub-ten-second scheduling remain outside this observer. These
records support lower background load, not exclusive causal attribution to
Orca, WindowServer, or a particular thread.
Across the five full-run traces, Orca Renderer averages 0.23–0.87% of one CPU
core and WindowServer 5.85–8.04%. These are interval-derived whole-run values;
do not equate them with H309's launch-only decayed CPU percentages or with
scene-local critical-path CPU savings.

## Selected next direction

Measure actual captured-stamp batch economics before changing lock primitives
or broadening the batch scope:

- Separate ingress resource marking from final publication scanning. Count
  batch constructions, append attempts, exact unique stamps, duplicate hits,
  nonempty flush sizes and flush causes (capacity, active/logical or override
  effect, command fence, final drain). Construction count matters because a
  reused batch is not a fresh zero-initialized batch per flush.
- Use disabled-by-default thread-local aggregate counters with no per-stamp
  clocks or shared atomics. Pin count conservation and unchanged stamp order,
  full generation/backing identity and final sequence watermarks in native
  tests. Keep timing runs separate from histogram runs.
- If singleton/short flushes dominate, evaluate a scalar singleton application
  and reused bounded storage. Do not relax a semantic flush fence just to make
  larger batches. Retain the existing TLA/native/pixel oracles, with mutants
  for any altered boundary. TLA proves permitted effects, not mutex economics.
- Before promotion, qualify 05/06 CPU, latency and FPS on a stable baseline.
  If a deficit returns, isolate range-upload queue elision, captured-backing
  batching and pre-lock preparation independently. Fewer acquisitions alone
  do not pass `R-BACK-SYNC-5`.

The subsequent replay optimization target is repeated resource-closure and
binding preparation, selected from actual cost/population evidence. No new
product policy, always-on instrumentation or default change is made here.
