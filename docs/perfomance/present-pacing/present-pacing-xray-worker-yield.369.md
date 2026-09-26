---
domain: present-pacing
workload: STALKER CoP Day
title: "H369: X-Ray helper-worker yield policy and bounded worker-count probe"
type: evidence
status: mechanism-confirmed-promotion-open
updated: 2026-09-26
source: traces/app-d3d9-stkcop-tail-h369/{binary.json,imports.txt,workers.txt,strings.txt,supervisor.jsonl,sample.txt,cpu.jsonl,analysis.json}; experiments/output/app-d3d9-stkcop-bench-tail-h369/{result.json,daybenchmark.result,dxmt9.log}; traces/app-d3d9-stkcop-tail-h368b/{sample-2000.txt,analysis.json}; experiments/output/app-d3d9-stkcop-bench-tail-h368b/daybenchmark.result
related: present-pacing-tail-yield-owner.368.md; present-pacing-tail-supply-audio.367.md
---

# H369: The engine already exposes a worker-count control

Read-only inspection of the installed benchmark's `xrCPU_Pipe.dll` identifies
a helper-worker idle protocol consistent with H368. A diagnostic run with the
existing engine argument `-max-threads 2` reduces the seven yield-heavy helper
threads to one. No dxmt9, Wine or game binary was patched; catalogue defaults
and the launcher remain unchanged.

## Installed-binary evidence

Binary SHA-256:
`4f13b61ade8ee04585c03631662fce44b09b105a1e0d188e96e11c03c7999303`.
The PE32 preferred base is `0x10000000`; addresses below are RVAs, not live
Rosetta PCs. `llvm-objdump`'s nearest-export annotation is not a recovered
private function name. No third-party implementation is copied into dxmt9.

| Site | Observed behavior |
|---|---|
| Worker entry RVA `0x3730` | Polls an atomic task flag; initially uses PAUSE up to 4,000 iterations, then SwitchToThread up to 1,000,000 iterations, then Sleep(1) up to 1,000 iterations, then repeated Sleep(100). Each phase can exit when a task becomes available. |
| Task acceptance/completion | Calls the supplied callback, restores the idle flag, atomically decrements shared outstanding-work count, and returns to the idle protocol. |
| `ttapi_Init`, RVA `0x3880` | Searches GetCommandLineA for `-max-threads`, parses `%u`, accepts values at least 1 and below the initial worker count, then creates count-minus-one helper threads with entry RVA `0x3730`. |
| `ttapi_RunAllWorkers`, RVA `0x3a80` | Publishes helper work, executes one callback on the caller, then PAUSE-polls the outstanding count until completion. Single-task dispatch calls directly. |

This distinguishes helper **idle polling** from the caller's **completion
barrier**. The observed helper yield storm is not dxmt9's encode-worker pool.
Live guest caller/start-PC binding is still absent, but installed code plus
the targeted worker-count intervention gives substantially stronger ownership
evidence than an unresolved native PC alone.

## Existing-option intervention

Use the existing catalogue launcher, not a custom direct-Wine path:

```sh
DXMT_STKCOP_EXTRA_ARGS='-max-threads 2' \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-stkcop-bench
```

The actual H369 run also enables frame/perf diagnostics and collects one native
sample. `supervisor.jsonl` records the live PID's command line containing the
argument. The setting is scoped to this subprocess, not exported persistently.

The run passes with zero reported GPU errors and the unchanged H368 artifact
hashes (Unix `829f3d304b314d38`, PE32 `4d30258d6e7d0c3e`). At observed frame
2003, its sample has one helper TID `12938432`, with 86 of 121 stack observations
in NtYieldExecution. The primary also has one incidental yield observation;
do not count that as a second helper. H368b had seven yield-heavy helpers.

Following CPU snapshots span about 8.1 seconds:

| Metric | H368b default | H369 max-threads 2 |
|---|---:|---:|
| Yield-heavy helpers | 7 | 1 |
| Helper CPU core equivalents | 3.130 combined | 0.897 |
| Primary CPU core equivalents | 0.553 | 0.758 |
| Encode CPU core equivalents | 0.227 | 0.536 |

These windows follow each run's frame-2000 sample, but are **not matched scene
windows**: frame ordinal depends on throughput. CPU totals include useful work
and polling. Do not treat the difference as removable yield CPU or a precise
increase in producer utilization caused by the option.

Benchmark-owned per-frame reconstruction reports 2325 frames / 146.708s =
15.848 FPS for H368b and 2875 / 128.617s = 22.353 FPS for H369. These are
single diagnostic runs with sampling interference, not paired observer-OFF
results, not README replacements, and not a qualified 41% product gain. The
candidate still has late long frames; it does not solve the entire tail.

## Primary wait site and remaining gaps

Re-examining H368b's primary thread sample identifies 101 of 117 observations
in `NtWaitForAlertByThreadId` at Unix offset `+387`. Installed ntdll disassembly
maps that return site to the `kevent` import. This narrows the native wait API,
but the repeated Wine dispatcher unwind does not recover the guest waiter or
waker. In particular, it does **not** establish that this wait is the
`ttapi_RunAllWorkers` barrier: the inspected barrier uses PAUSE polling.

No audio-event timestamp or pixel-exact comparison was obtained. Catalogue
PASS and zero reported GPU errors are not a visual correctness verdict.

## Next gate

Run observer-OFF default versus `-max-threads 2` in reversed order, comparing
whole-benchmark and tail frame-time distributions from `.result`, and obtain
visual/audio confirmation. Keep identical binaries, scene, resolution and
foreground conditions. If qualified, retain this as an explicitly labeled
workload configuration rather than silently rewriting existing baseline data.
Explore another count only after establishing reproducibility. Keep primary
waiter/waker attribution separate; do not globally replace Wine yield with
sleep or change dxmt9 queue semantics on this evidence.
