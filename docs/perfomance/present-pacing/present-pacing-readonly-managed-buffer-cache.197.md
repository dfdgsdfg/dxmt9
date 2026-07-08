---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: producer-attribution
order: 197
title: Readonly Managed Buffer Cache Collapses The Lock Bridge Storm
date: 2026-07-08
type: no-gputrace
status: accepted-mechanism-confirm-not-fps-promotion
source: experiments/output/app-d3d9-3dmark05-h196-readonly-cache-r1-20260708/result.json; experiments/output/app-d3d9-3dmark05-h196-readonly-cache-r1-20260708/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-h196-readonly-cache-r1-20260708/3dmark05-perf-frames.csv; docs/perfomance/present-pacing/present-pacing-producer-sampling-attribution.196.md
related: docs/perfomance/present-pacing/index.md; src/d3d9/d3d9_pe_buffer_readonly_cache.hpp; src/d3d9/d3d9_pe_device_child_buffer.cpp
---

# Present-Pacing H197 - Readonly managed buffer cache runtime confirm

## Question

H196 named the largest dxmt9-owned producer lever: 3DMark05 repeatedly locks
unchanged managed buffers as `D3DLOCK_READONLY`, crossing PE->wow64->unix on
nearly every lock and re-shadowing the same bytes. Does a PE-side readonly
managed-buffer cache remove that bridge storm in the real GT1 run?

## Candidate

The candidate serves repeat `D3DPOOL_MANAGED` + `D3DLOCK_READONLY` vertex/index
buffer locks from a PE-owned range cache when the cached generation covers the
requested range. A cold readonly miss still crosses `dxmt9c_buffer_lock` once,
copies the range to the PE cache, immediately unlocks the unix buffer, and makes
the later app `Unlock()` bridge-free. Writable locks invalidate the cache, and
the raw `ProcessVertices()` destination-write path invalidates the affected
vertex buffer cache.

The cache is intentionally PE-local: it stores guest-visible shadow bytes and a
generation/range stamp, not D9C handles, Metal objects, unix pointers, or bridge
records.

## Run

Foreground no-gputrace smoke:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix h196-readonly-cache-r1-20260708 \
  --frame 60 --no-gputrace --timeout 120 \
  --frame-sampling --keep-frontmost --no-encoder-breakdown
```

The run staged both PE DLL builds and the Rosetta unix provider, used the
Sikarugir symbol-exposing Wine runtime, and finished with `status=pass`,
`returncode=0`, `timed_out=false`, `capture_error=None`. A non-perf log scan
found no fatal/assert/crash/ABI-hash rows.

## Counter movement

H196 attribution baseline vs H197 runtime confirm:

| Counter | H196 before | H197 after | Movement |
|---|---:|---:|---:|
| `d3d9_buffer_lock_calls` / present | `1,478.7` | `54.0` | `-96.3%` |
| `d3d9_buffer_lock_readonly` / present | `1,466.7` | `42.1` | `-97.1%` |
| shadow bytes / present | `14.78 MB` | `2.03 MB` (`1.93 MiB`) | `-86.3%` |
| `d3d9_buffer_lock_ms` / present | `5.005ms` | `0.744ms` | `-85.1%` |
| `map_buffer_total_ms` / present | included in the H196 unix wall | `0.028ms` | bridge leaf almost gone |
| `map_buffer_mutex_wait_ms` / present | `2.716ms` | `0.017ms` | `-99.4%` |

The H197 run still records `108,073` bridge-visible locks over `2,002` presents,
but those are the remaining cold-miss, non-managed, or writable paths. The
repeated readonly relocks that now hit the PE cache are intentionally invisible
to the unix-side `map_buffer_*` counters.

## Frame sample

Frame sampling wrote `2,002` rows. Excluding startup frames (`frame >= 10`):

| Metric | Value |
|---|---:|
| FPS mean | `20.456` |
| FPS median | `20.034` |
| FPS p05 | `13.327` |
| wall-ms mean | `51.987` |
| wall-ms median | `49.917` |
| wall-ms p95 | `74.889` |

This run is a mechanism/runtime confirm, not a clean FPS A/B against a same-day
control and **not an FPS promotion**. The useful conclusion is the bridge-storm
collapse: the H196 named producer-side lock path no longer dominates the
unix/wow64-visible counters.

## Verdict

Accepted as a mechanism confirm, **not as an FPS promotion**: the PE readonly
managed-buffer cache removes the pathological repeat readonly lock bridge
traffic from the 3DMark05 GT1 run, but the sampled `20.456` mean FPS /
`20.034` median FPS does not prove a user-visible throughput gain. The next
performance question should not spend more effort on unix `map_buffer`
lock/mutex cost unless a new counter shows it has regrown. Follow-up attribution
needs either PE-side cache hit counters or another non-perturbing producer
sample to split the now-exposed Rosetta guest / PE / Wine remainder.
