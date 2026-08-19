#pragma once

// DXMT9_PE_THREAD_SAMPLER (diagnostic, Tier 2 of PE 32-bit symbolication).
//
// xctrace cannot attribute Rosetta-translated PE code on Apple Silicon:
// Instruments only maps translated PCs back to origin images for dyld-known
// images, and PE modules are not dyld images, so a probe-verified module map
// still leaves ~93% of the wine game thread's samples as unmapped 64-bit
// addresses (see agents/rules/metal_debugging.rules.md). The working path is
// to sample the game thread's *true Win32 program counter* from inside the
// process and classify it against the same module map that
// DXMT9_PE_MODULE_MAP already builds.
//
// This header has two halves:
//
//   1. A pure, host-compilable classification/aggregation core (no Windows
//      dependency), unit-tested by tests/native/bridge/pe_thread_sampler_spec.
//   2. A Win32-only PeThreadSampler that owns the sampling thread. It is
//      compiled only in the PE lanes, so its correctness rests on the safety
//      contract documented on run() plus the host coverage of the core.
//
// Everything here is diagnostic. A run with the sampler enabled is NOT a
// valid performance sample: it stops the game thread `hz` times a second.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dxmt9::d3d9::pe {

// Fixed storage bounds. All sampler state is preallocated: the sampling loop
// must never allocate (see the suspend-window contract on run()).
inline constexpr std::size_t kPeSamplerMaxModules = 192;
inline constexpr std::size_t kPeSamplerModuleNameMax = 64;
// Open-addressing self-PC table. Power of two so the probe mask is an AND.
inline constexpr std::size_t kPeSamplerSelfPcSlots = 4096;
// New keys stop being admitted at 3/4 load, which is what makes linear
// probing terminate without a full-table scan.
inline constexpr std::size_t kPeSamplerSelfPcMaxUsed = (kPeSamplerSelfPcSlots * 3) / 4;
// PC bucket granularity for the self-module histogram, in bytes.
inline constexpr std::uint64_t kPeSamplerSelfPcGranularity = 64;
inline constexpr std::size_t kPeSamplerTopModules = 20;
inline constexpr std::size_t kPeSamplerTopSelfPcBuckets = 32;

inline constexpr std::size_t kPeSamplerInvalidIndex = static_cast<std::size_t>(-1);

// One module's address range plus a stable back-reference into the caller's
// name table. `nameIndex` survives normalizePeSamplerModuleRanges()'s sort, so
// per-module counters can be indexed by enumeration order (deterministic)
// rather than by post-sort position.
struct PeSamplerModuleRange {
  std::uint64_t base = 0;
  std::uint64_t size = 0;
  std::uint32_t nameIndex = 0;
};

// Sorts `ranges[0..count)` ascending by base and drops every entry that
// classifyPeSamplerPc()'s binary search could not handle: zero-size entries
// and entries overlapping an already-kept range. Sizes that would overflow
// base+size are clamped. Returns the number of kept entries, which are packed
// at the front of the array.
//
// Establishing the sorted/disjoint precondition here is what lets the
// classifier be a plain O(log n) binary search rather than a scan that has to
// defend against overlap on every sample.
inline std::size_t normalizePeSamplerModuleRanges(PeSamplerModuleRange* ranges,
                                                  std::size_t count) noexcept {
  if (ranges == nullptr || count == 0) {
    return 0;
  }
  std::size_t kept = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (ranges[i].size == 0) {
      continue;
    }
    PeSamplerModuleRange entry = ranges[i];
    const std::uint64_t maxSize = UINT64_MAX - entry.base;
    if (entry.size > maxSize) {
      entry.size = maxSize;
    }
    if (entry.size == 0) {
      continue;  // base was at the top of the address space; nothing to keep.
    }
    ranges[kept++] = entry;
  }
  std::sort(ranges, ranges + kept,
            [](const PeSamplerModuleRange& a, const PeSamplerModuleRange& b) {
              if (a.base != b.base) return a.base < b.base;
              return a.nameIndex < b.nameIndex;
            });
  std::size_t out = 0;
  std::uint64_t previousEnd = 0;
  bool havePrevious = false;
  for (std::size_t i = 0; i < kept; ++i) {
    if (havePrevious && ranges[i].base < previousEnd) {
      continue;  // overlaps a kept range: drop, first-by-base wins.
    }
    ranges[out] = ranges[i];
    previousEnd = ranges[i].base + ranges[i].size;
    havePrevious = true;
    ++out;
  }
  return out;
}

// Returns the index into `ranges` of the module containing `pc`, or
// kPeSamplerInvalidIndex. Precondition: `ranges` was produced by
// normalizePeSamplerModuleRanges() (sorted ascending by base, non-overlapping,
// non-empty, non-overflowing).
inline std::size_t classifyPeSamplerPc(const PeSamplerModuleRange* ranges,
                                       std::size_t count,
                                       std::uint64_t pc) noexcept {
  if (ranges == nullptr || count == 0) {
    return kPeSamplerInvalidIndex;
  }
  // Find the first range whose base is strictly greater than pc; the only
  // candidate is the one before it.
  std::size_t low = 0;
  std::size_t high = count;
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (ranges[mid].base <= pc) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low == 0) {
    return kPeSamplerInvalidIndex;
  }
  const std::size_t candidate = low - 1;
  if (pc - ranges[candidate].base < ranges[candidate].size) {
    return candidate;
  }
  return kPeSamplerInvalidIndex;
}

inline std::uint64_t peSamplerBucketForPc(std::uint64_t pc) noexcept {
  return pc & ~(kPeSamplerSelfPcGranularity - 1);
}

// Bounded self-module PC histogram. `bucketPlusOne == 0` marks an empty slot;
// a bucket always has its low 6 bits cleared, so bucket+1 can never collide
// with the empty sentinel and can never overflow.
struct PeSamplerSelfPcTable {
  std::uint64_t bucketPlusOne[kPeSamplerSelfPcSlots]{};
  std::uint64_t samples[kPeSamplerSelfPcSlots]{};
  std::uint64_t overflow = 0;
  std::size_t used = 0;
};

inline std::uint64_t peSamplerMixBucket(std::uint64_t bucket) noexcept {
  // splitmix64 finalizer: the low bits of a PC bucket are all zero, so an
  // unmixed mask would collapse every sample into a handful of slots.
  std::uint64_t x = bucket + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// Counts one sample at `pc`. Existing buckets always count; a *new* bucket is
// admitted only below the load cap, otherwise the sample lands in `overflow`.
inline void recordPeSamplerSelfPc(PeSamplerSelfPcTable& table,
                                  std::uint64_t pc) noexcept {
  const std::uint64_t bucket = peSamplerBucketForPc(pc);
  const std::uint64_t key = bucket + 1;
  const std::size_t mask = kPeSamplerSelfPcSlots - 1;
  std::size_t index = static_cast<std::size_t>(peSamplerMixBucket(bucket)) & mask;
  for (std::size_t probe = 0; probe < kPeSamplerSelfPcSlots; ++probe) {
    const std::size_t slot = (index + probe) & mask;
    if (table.bucketPlusOne[slot] == key) {
      ++table.samples[slot];
      return;
    }
    if (table.bucketPlusOne[slot] == 0) {
      if (table.used >= kPeSamplerSelfPcMaxUsed) {
        ++table.overflow;
        return;
      }
      table.bucketPlusOne[slot] = key;
      table.samples[slot] = 1;
      ++table.used;
      return;
    }
  }
  // Unreachable while the load cap holds; counted rather than asserted so a
  // diagnostic can never take the process down.
  ++table.overflow;
}

struct PeSamplerTopBucket {
  std::uint64_t bucket = 0;
  std::uint64_t samples = 0;
};

struct PeSamplerTopModule {
  std::uint32_t nameIndex = 0;
  std::uint64_t samples = 0;
};

// Fixed-capacity descending selection. Ties break on the secondary key
// ascending (bucket address / name index) so repeated emissions of the same
// state are byte-identical.
inline std::size_t selectTopPeSamplerSelfPc(const PeSamplerSelfPcTable& table,
                                            PeSamplerTopBucket* out,
                                            std::size_t outCap) noexcept {
  if (out == nullptr || outCap == 0) {
    return 0;
  }
  std::size_t filled = 0;
  for (std::size_t slot = 0; slot < kPeSamplerSelfPcSlots; ++slot) {
    if (table.bucketPlusOne[slot] == 0 || table.samples[slot] == 0) {
      continue;
    }
    const PeSamplerTopBucket candidate{table.bucketPlusOne[slot] - 1,
                                       table.samples[slot]};
    if (filled == outCap) {
      const PeSamplerTopBucket& worst = out[outCap - 1];
      const bool better = candidate.samples > worst.samples ||
                          (candidate.samples == worst.samples &&
                           candidate.bucket < worst.bucket);
      if (!better) {
        continue;
      }
    } else {
      ++filled;
    }
    std::size_t insertAt = filled - 1;
    while (insertAt > 0) {
      const PeSamplerTopBucket& previous = out[insertAt - 1];
      const bool candidateWins = candidate.samples > previous.samples ||
                                 (candidate.samples == previous.samples &&
                                  candidate.bucket < previous.bucket);
      if (!candidateWins) {
        break;
      }
      out[insertAt] = previous;
      --insertAt;
    }
    out[insertAt] = candidate;
  }
  return filled;
}

inline std::size_t selectTopPeSamplerModules(const std::uint64_t* samplesByModule,
                                             std::size_t moduleCount,
                                             PeSamplerTopModule* out,
                                             std::size_t outCap) noexcept {
  if (samplesByModule == nullptr || out == nullptr || outCap == 0) {
    return 0;
  }
  std::size_t filled = 0;
  for (std::size_t i = 0; i < moduleCount; ++i) {
    if (samplesByModule[i] == 0) {
      continue;
    }
    const PeSamplerTopModule candidate{static_cast<std::uint32_t>(i),
                                       samplesByModule[i]};
    if (filled == outCap) {
      const PeSamplerTopModule& worst = out[outCap - 1];
      const bool better = candidate.samples > worst.samples ||
                          (candidate.samples == worst.samples &&
                           candidate.nameIndex < worst.nameIndex);
      if (!better) {
        continue;
      }
    } else {
      ++filled;
    }
    std::size_t insertAt = filled - 1;
    while (insertAt > 0) {
      const PeSamplerTopModule& previous = out[insertAt - 1];
      const bool candidateWins = candidate.samples > previous.samples ||
                                 (candidate.samples == previous.samples &&
                                  candidate.nameIndex < previous.nameIndex);
      if (!candidateWins) {
        break;
      }
      out[insertAt] = previous;
      --insertAt;
    }
    out[insertAt] = candidate;
  }
  return filled;
}

inline constexpr std::uint32_t kPeSamplerDefaultHz = 250;
inline constexpr std::uint32_t kPeSamplerMinHz = 50;
inline constexpr std::uint32_t kPeSamplerMaxHz = 1000;

inline std::uint32_t clampPeSamplerHz(std::uint32_t hz) noexcept {
  if (hz < kPeSamplerMinHz) return kPeSamplerMinHz;
  if (hz > kPeSamplerMaxHz) return kPeSamplerMaxHz;
  return hz;
}

// One consistent read of the sampler's aggregate, taken under its lock and
// then formatted without it. Small by construction (~2 KiB): the 64 KiB
// histogram is reduced to its top rows while the lock is held.
struct PeSamplerSnapshot {
  std::uint64_t samples = 0;
  std::uint64_t suspendFailures = 0;
  std::uint64_t contextFailures = 0;
  std::uint64_t resumeFailures = 0;
  std::uint64_t selfPcOverflow = 0;
  std::uint32_t hz = 0;
  bool moduleTableReady = false;
  std::size_t moduleRows = 0;
  PeSamplerTopModule topModules[kPeSamplerTopModules]{};
  char moduleNames[kPeSamplerTopModules][kPeSamplerModuleNameMax]{};
  char selfModuleName[kPeSamplerModuleNameMax]{};
  std::size_t selfPcRows = 0;
  PeSamplerTopBucket topSelfPc[kPeSamplerTopSelfPcBuckets]{};
};

}  // namespace dxmt9::d3d9::pe

#if defined(_WIN32)

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace dxmt9::d3d9::pe {

// Marker whose address must lie inside our own PE d3d9.dll: this is how the
// sampler decides which enumerated module is "self" for the PC histogram,
// without hardcoding a module name.
inline void peSamplerSelfMarker() {}

// Owns the sampling thread and all its state. Heap-allocated and referenced
// ONLY by the sampler thread and by the device that started it — never by any
// other device object — so that a join timeout can leak the block safely
// instead of deadlocking teardown (see stopAndRelease()).
class PeThreadSampler {
 public:
  static constexpr DWORD kJoinTimeoutMs = 2000;

  PeThreadSampler(const PeThreadSampler&) = delete;
  PeThreadSampler& operator=(const PeThreadSampler&) = delete;

  // Starts one sampler for `targetThreadId`. Returns nullptr when a sampler is
  // already live in this process (an app that creates several devices must not
  // get several threads all stopping the same game thread), when the target
  // cannot be opened, or when thread/event creation fails.
  static PeThreadSampler* startForThread(DWORD targetThreadId,
                                         std::uint32_t hz) noexcept {
    bool expected = false;
    if (!processSamplerLive().compare_exchange_strong(expected, true)) {
      return nullptr;
    }
    auto* sampler = new (std::nothrow) PeThreadSampler(targetThreadId, hz);
    if (sampler == nullptr) {
      processSamplerLive().store(false);
      return nullptr;
    }
    if (!sampler->initialize()) {
      sampler->destroyResources();
      delete sampler;
      processSamplerLive().store(false);
      return nullptr;
    }
    return sampler;
  }

  // Signals the sampler and waits a bounded time for it to exit.
  //
  // On a clean join the block is destroyed and the process-wide guard is
  // released. On timeout the block is deliberately LEAKED and the guard stays
  // set: the sampler thread may still be inside a wineserver
  // Suspend/GetContext/Resume round-trip that references this state, and
  // freeing it under a live thread would turn a diagnostic into a
  // use-after-free during teardown. destroyResources() is skipped with it, so
  // the target-thread, sampler-thread and stop-event HANDLEs leak as well.
  // Leaking ~85 KiB and three handles once, in a run that is already not a
  // valid perf sample, is the cheaper failure — and it can only happen once,
  // because the guard below stays set and no second sampler can start.
  static void stopAndRelease(PeThreadSampler* sampler) noexcept {
    if (sampler == nullptr) {
      return;
    }
    sampler->stopRequested_.store(true, std::memory_order_relaxed);
    if (sampler->stopEvent_ != nullptr) {
      SetEvent(sampler->stopEvent_);
    }
    if (sampler->thread_ != nullptr) {
      if (WaitForSingleObject(sampler->thread_, kJoinTimeoutMs) != WAIT_OBJECT_0) {
        return;  // leak; see the contract above.
      }
    }
    sampler->destroyResources();
    delete sampler;
    processSamplerLive().store(false);
  }

  // Lets a failed startForThread() be reported as "already running" rather
  // than as an OS error with a stale GetLastError() behind it.
  static bool processSamplerIsLive() noexcept {
    return processSamplerLive().load();
  }

  std::uint32_t hz() const noexcept { return hz_; }
  DWORD targetThreadId() const noexcept { return targetThreadId_; }
  std::uint32_t intervalMs() const noexcept { return intervalMs_; }

  void snapshot(PeSamplerSnapshot& out) const noexcept {
    out = PeSamplerSnapshot{};
    out.hz = hz_;
    AcquireSRWLockShared(&lock_);
    out.samples = totalSamples_;
    out.suspendFailures = suspendFailures_;
    out.contextFailures = contextFailures_;
    out.resumeFailures = resumeFailures_;
    out.selfPcOverflow = selfPc_.overflow;
    out.moduleTableReady = moduleTableReady_;
    out.moduleRows = selectTopPeSamplerModules(samplesByModule_, nameCount_,
                                               out.topModules,
                                               kPeSamplerTopModules);
    for (std::size_t i = 0; i < out.moduleRows; ++i) {
      const std::uint32_t nameIndex = out.topModules[i].nameIndex;
      if (nameIndex < nameCount_) {
        std::memcpy(out.moduleNames[i], names_[nameIndex],
                    kPeSamplerModuleNameMax);
      }
    }
    if (selfNameIndex_ != kPeSamplerInvalidIndex) {
      std::memcpy(out.selfModuleName, names_[selfNameIndex_],
                  kPeSamplerModuleNameMax);
      out.selfPcRows = selectTopPeSamplerSelfPc(selfPc_, out.topSelfPc,
                                                kPeSamplerTopSelfPcBuckets);
    }
    ReleaseSRWLockShared(&lock_);
  }

 private:
  PeThreadSampler(DWORD targetThreadId, std::uint32_t hz) noexcept
      : targetThreadId_(targetThreadId), hz_(clampPeSamplerHz(hz)) {
    intervalMs_ = 1000u / hz_;
    if (intervalMs_ == 0) {
      intervalMs_ = 1;
    }
    InitializeSRWLock(&lock_);
  }

  ~PeThreadSampler() = default;

  static std::atomic<bool>& processSamplerLive() noexcept {
    static std::atomic<bool> live{false};
    return live;
  }

  bool initialize() noexcept {
    targetThread_ = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                               FALSE, targetThreadId_);
    if (targetThread_ == nullptr) {
      return false;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
      return false;
    }
    thread_ = CreateThread(nullptr, 0, &PeThreadSampler::threadProc, this, 0,
                           nullptr);
    return thread_ != nullptr;
  }

  void destroyResources() noexcept {
    if (thread_ != nullptr) {
      CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (stopEvent_ != nullptr) {
      CloseHandle(stopEvent_);
      stopEvent_ = nullptr;
    }
    if (targetThread_ != nullptr) {
      CloseHandle(targetThread_);
      targetThread_ = nullptr;
    }
  }

  static DWORD WINAPI threadProc(LPVOID param) noexcept {
    static_cast<PeThreadSampler*>(param)->run();
    return 0;
  }

  // Suspend-window safety contract.
  //
  // Between SuspendThread() and ResumeThread() this loop does exactly one
  // thing: GetThreadContext() into a CONTEXT that was zero-initialized once,
  // outside the loop. In that window it does not allocate, log, format, take
  // this object's SRWLOCK, call any CRT function, or touch any lazily-resolved
  // import. That matters because the suspended thread is the game thread and
  // may be holding, at the instant we stop it:
  //
  //   * the process heap lock (any malloc/new here would self-deadlock),
  //   * the loader lock (any LoadLibrary / first-touch of an unresolved import
  //     thunk here would self-deadlock),
  //   * the CRT stdio/locale lock and dxmt9's own logger state (any logging
  //     here would self-deadlock),
  //   * this sampler's SRWLOCK, taken by the emission path that runs on the
  //     game thread at Present.
  //
  // The last one is the reason aggregation is outside the window rather than
  // merged into it: aggregate() takes the lock, and the emission path on the
  // game thread takes the same lock, so acquiring it while that thread is
  // stopped would be an unconditional deadlock. Acquiring it *after* Resume is
  // safe in both directions — the sampler never holds the lock across a
  // suspend, so the game thread can always make progress out of it.
  //
  // Import resolution is pre-warmed below rather than reasoned about: PE
  // imports are bound by the loader at DLL load, but a pre-warm costs one call
  // and removes the question from the window entirely.
  //
  // Accuracy caveat, not a safety one: under Wine these three calls are
  // wineserver round-trips and suspension is delivered asynchronously, so a
  // sampled PC is "where the thread was around now", not an exact instant, and
  // a GetThreadContext that loses the race is counted as a context failure
  // rather than retried. Treat the histogram as a distribution, never as an
  // instruction-exact trace.
  void run() noexcept {
    if (targetThreadId_ == GetCurrentThreadId()) {
      return;  // never sample ourselves.
    }
    buildModuleTable();

    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    // Pre-warm the two calls that will run inside the suspend window.
    // GetThreadContext on the running current thread returns unusable values
    // but is safe; ResumeThread on a thread whose suspend count is zero is a
    // documented no-op returning 0.
    (void)GetThreadContext(GetCurrentThread(), &context);
    (void)ResumeThread(targetThread_);

    while (!stopRequested_.load(std::memory_order_relaxed) &&
           WaitForSingleObject(stopEvent_, intervalMs_) == WAIT_TIMEOUT) {
      context.ContextFlags = CONTEXT_CONTROL;

      // ---- suspend window opens ----
      const DWORD previousSuspendCount = SuspendThread(targetThread_);
      if (previousSuspendCount == static_cast<DWORD>(-1)) {
        // Suspend failed: nothing to resume. Counted outside any window.
        noteFailure(&suspendFailures_);
        continue;
      }
      const BOOL contextOk = GetThreadContext(targetThread_, &context);
      const DWORD resumeResult = ResumeThread(targetThread_);
      // ---- suspend window closes ----

      if (resumeResult == static_cast<DWORD>(-1)) {
        // The game thread is now stuck suspended and we cannot fix it. Stop
        // sampling immediately rather than compounding it; the counter is the
        // only report, because logging from here could block on a lock the
        // suspended thread holds.
        noteFailure(&resumeFailures_);
        stopRequested_.store(true, std::memory_order_relaxed);
        break;
      }
      if (!contextOk) {
        noteFailure(&contextFailures_);
        continue;
      }
#if defined(_WIN64)
      const std::uint64_t pc = static_cast<std::uint64_t>(context.Rip);
#else
      const std::uint64_t pc = static_cast<std::uint64_t>(context.Eip);
#endif
      aggregate(pc);
    }
  }

  void noteFailure(std::uint64_t* counter) noexcept {
    AcquireSRWLockExclusive(&lock_);
    ++*counter;
    ReleaseSRWLockExclusive(&lock_);
  }

  void aggregate(std::uint64_t pc) noexcept {
    AcquireSRWLockExclusive(&lock_);
    ++totalSamples_;
    const std::size_t rangeIndex = classifyPeSamplerPc(ranges_, rangeCount_, pc);
    const std::size_t nameIndex = (rangeIndex == kPeSamplerInvalidIndex)
                                      ? unknownNameIndex_
                                      : ranges_[rangeIndex].nameIndex;
    if (nameIndex < nameCount_) {
      ++samplesByModule_[nameIndex];
    }
    if (rangeIndex != kPeSamplerInvalidIndex && nameIndex == selfNameIndex_) {
      recordPeSamplerSelfPc(selfPc_, pc);
    }
    ReleaseSRWLockExclusive(&lock_);
  }

  // Enumerates the loaded modules once, on the sampler thread, before the
  // sampling loop starts. Limitation: a DLL loaded *after* this point is never
  // in the table, so its samples land in the synthetic "unknown" row rather
  // than being misattributed. Device creation happens well after module init,
  // so in practice this only affects late plugin/codec loads.
  void buildModuleTable() noexcept {
    PeSamplerModuleRange scratch[kPeSamplerMaxModules]{};
    char scratchNames[kPeSamplerMaxModules][kPeSamplerModuleNameMax]{};
    std::size_t discovered = 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
      MODULEENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      if (Module32FirstW(snapshot, &entry)) {
        do {
          if (discovered >= kPeSamplerMaxModules - 1) {
            break;  // one slot is reserved for the synthetic "unknown" row.
          }
          char* name = scratchNames[discovered];
          const int written = WideCharToMultiByte(
              CP_UTF8, 0, entry.szModule, -1, name,
              static_cast<int>(kPeSamplerModuleNameMax), nullptr, nullptr);
          if (written <= 0) {
            std::snprintf(name, kPeSamplerModuleNameMax, "<unnamed>");
          }
          name[kPeSamplerModuleNameMax - 1] = '\0';
          scratch[discovered].base =
              static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                  entry.modBaseAddr));
          scratch[discovered].size =
              static_cast<std::uint64_t>(entry.modBaseSize);
          scratch[discovered].nameIndex = static_cast<std::uint32_t>(discovered);
          ++discovered;
        } while (Module32NextW(snapshot, &entry));
      }
      CloseHandle(snapshot);
    }

    const std::size_t kept = normalizePeSamplerModuleRanges(scratch, discovered);
    const std::uint64_t selfAddr = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&peSamplerSelfMarker));
    const std::size_t selfRange = classifyPeSamplerPc(scratch, kept, selfAddr);

    AcquireSRWLockExclusive(&lock_);
    for (std::size_t i = 0; i < discovered; ++i) {
      std::memcpy(names_[i], scratchNames[i], kPeSamplerModuleNameMax);
    }
    std::snprintf(names_[discovered], kPeSamplerModuleNameMax, "unknown");
    unknownNameIndex_ = discovered;
    nameCount_ = discovered + 1;
    for (std::size_t i = 0; i < kept; ++i) {
      ranges_[i] = scratch[i];
    }
    rangeCount_ = kept;
    selfNameIndex_ = (selfRange == kPeSamplerInvalidIndex)
                         ? kPeSamplerInvalidIndex
                         : static_cast<std::size_t>(scratch[selfRange].nameIndex);
    moduleTableReady_ = true;
    ReleaseSRWLockExclusive(&lock_);
  }

  HANDLE targetThread_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  DWORD targetThreadId_ = 0;
  std::uint32_t hz_ = kPeSamplerDefaultHz;
  std::uint32_t intervalMs_ = 4;
  std::atomic<bool> stopRequested_{false};

  // Everything below is guarded by lock_. The sampler thread takes it
  // exclusively (aggregation / failure counts / table publish) and the game
  // thread takes it shared (emission). It is NEVER held across a suspend.
  mutable SRWLOCK lock_{};
  PeSamplerModuleRange ranges_[kPeSamplerMaxModules]{};
  std::size_t rangeCount_ = 0;
  char names_[kPeSamplerMaxModules][kPeSamplerModuleNameMax]{};
  std::size_t nameCount_ = 0;
  std::size_t unknownNameIndex_ = kPeSamplerInvalidIndex;
  std::size_t selfNameIndex_ = kPeSamplerInvalidIndex;
  bool moduleTableReady_ = false;
  std::uint64_t samplesByModule_[kPeSamplerMaxModules]{};
  std::uint64_t totalSamples_ = 0;
  std::uint64_t suspendFailures_ = 0;
  std::uint64_t contextFailures_ = 0;
  std::uint64_t resumeFailures_ = 0;
  PeSamplerSelfPcTable selfPc_{};
};

}  // namespace dxmt9::d3d9::pe

#else  // !_WIN32

namespace dxmt9::d3d9::pe {

// Host/unix stub so callers need no #ifdef. src/d3d9/meson.build only compiles
// d3d9_pe_device.cpp on the windows lane, so this exists for the same reason
// dxmt9PeDumpModuleMap() has a non-Windows no-op: to keep the file compilable
// if that wiring ever changes, and to let the host unit test include this
// header for the pure core above.
class PeThreadSampler {
 public:
  static PeThreadSampler* startForThread(unsigned long, std::uint32_t) noexcept {
    return nullptr;
  }
  static void stopAndRelease(PeThreadSampler*) noexcept {}
  static bool processSamplerIsLive() noexcept { return false; }
  std::uint32_t hz() const noexcept { return 0; }
  unsigned long targetThreadId() const noexcept { return 0; }
  std::uint32_t intervalMs() const noexcept { return 0; }
  void snapshot(PeSamplerSnapshot& out) const noexcept { out = PeSamplerSnapshot{}; }
};

}  // namespace dxmt9::d3d9::pe

#endif  // _WIN32
