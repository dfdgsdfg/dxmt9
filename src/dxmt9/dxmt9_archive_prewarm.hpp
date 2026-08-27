#pragma once

// R-BACK-3.7 / R-BACK-3.8 / R-BACK-3.9 / R-BACK-3.10 / R-BACK-3.11 /
// R-BACK-4.8 — MTLBinaryArchive prewarming with cross-process safety.
//
// This module owns:
//   * Resolution of the per-(ABI, GPU-family) archive path under cache_root.
//   * Selection of the prewarm mode (full / lazy / disabled) from build
//     defaults overridable by the DXMT9_PREWARM env var.
//   * Cross-process POSIX flock contract (LOCK_EX for serializeToURL,
//     LOCK_SH with bounded retry for the load read).
//   * Graceful failure handling per design §6.1: missing → empty start;
//     corrupt/schema → rename to *.corrupt / *.outdated and start empty;
//     lock_busy beyond 100 ms → fall through to compile-only.
//   * R-BACK-3.9 — a configurable size guard (DXMT9_ARCHIVE_MAX_PREWARM_MB)
//     plus the async-load orchestration that keeps a successful `Full`
//     prewarm off the device-init critical path, and the process-scoped
//     backfill queue that preserves pipeline-cache archive writes which
//     raced the in-flight load.
//   * R-BACK-3.10 — the pure `shouldMilestoneSave` decision plus the
//     write-side LOCK_EX helpers `dxmt9::shaders::Archive` uses to persist
//     the archive mid-session without waiting for clean shutdown.
//   * Calls into the pre-existing perf::countPrewarm* counter slots.
//
// R-BACK-3.9 contract: device init must never block on archive I/O for a
// successful `Full` prewarm. The blocking `MTLDevice.newBinaryArchive(url:)`
// deserialize call — the actual cost the 125 MB 3DMark05 incident measured
// (docs/perfomance/present-pacing/present-pacing-inline-const-delta.201.md)
// — runs on a background thread started by beginAsyncFullLoad(). Until that
// thread calls its completion callback, the archive handle passed to
// pipeline-cache builders stays empty/falsy, which is already the existing
// "compile fresh, no archive attach" fallback (dxmt9_pipeline_cache.cpp's
// `if (archive && *archive)` guards) — no new code needed for the fallback
// itself. `dxmt9::shaders::Archive` attaches the loaded archive from the
// background thread once it completes; jobs whose archive-add step raced
// the still-empty handle are queued via queueArchiveBackfill() and replayed
// (their PSO recompiled, discarding the result — only the archive-add side
// effect matters) once the archive attaches.
//
// Thread-safety note: MTLBinaryArchive is not documented thread-safe.
// dxmt9::shaders::Archive serializes its own `ref_` writes behind a mutex;
// `MTLBinaryArchive_serialize` / `addRenderPipelineFunctionsWithDescriptor:`
// / `addComputePipelineFunctionsWithDescriptor:` are additionally serialized
// in-process by a mutex in src/winemetal/unix/winemetal_private_api.mm since
// multiple PSO-compile worker threads (dxmt9_pipeline_cache.cpp's
// CompileQueue, 1-8 workers) can otherwise race those ObjC calls on the same
// archive object. The ~50 pre-existing call sites that read
// `*archive`/`archive->reference()` (a plain `WMT::Reference<WMT::BinaryArchive>`
// handle field) are NOT retrofitted with locking: `obj_handle_t` is a single
// pointer-sized, naturally-aligned field, the empty→loaded transition happens
// at most once per process, and the source object is independently retained
// by the background thread before the attach assignment runs, so a reader
// that observes the pre-attach (empty) state for one extra instant simply
// takes the already-supported "no archive" branch — never a torn handle or a
// use-after-free. This is a deliberate, documented micro-race accepted to
// avoid refactoring every PSO-builder call site; see the R-BACK-3.9..3.11
// commit message for the full trade-off discussion.

#include "../winemetal/Metal.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace dxmt9::archive_prewarm {

// dxmt9 archive ABI version. Bump whenever the MSL emitter output, the
// variant-key encoding, the MTLBinaryArchive schema we depend on, or the
// FFP key bit layout changes. Embedded into the filename so a version
// drift never reads from a stale archive. Keep this in step with
// pipeline::kShaderEmitterVersion / kShaderSourceLayoutVersion when those
// source-identity fields change the generated MSL contract.
inline constexpr std::uint32_t kArchiveAbiVersion = 6;

// R-BACK-3.8 — prewarm mode selection.
enum class Mode {
  Disabled,   // never read or write the archive (debug)
  Lazy,       // skip the load step, but still configure path/lock; compile
              // path writes the archive on subsequent runs (dev default)
  Full,       // load the archive at device init and populate caches
              // (shipping default)
};

// Build-default prewarm mode. dxmt9's meson tree has no `dev_build`
// option at the time of writing, so we branch on the standard `NDEBUG`
// macro that release builds normally `#define`: shipping builds default
// to Mode::Full (warm caches → fewer cold compiles on the first frame),
// debug / unoptimized builds default to Mode::Lazy (skip the load step,
// keeps device init off the I/O critical path while still letting the
// compile path write the archive on shutdown). The DXMT9_PREWARM env
// var continues to override either default.
Mode buildDefaultMode();

// Final mode: env DXMT9_PREWARM=full|lazy|disabled overrides the build
// default. Unknown values fall back to the build default.
Mode resolveMode();

// Resolve `${cache_root}` per design §6.1:
//   $DXMT9_CACHE_DIR → $XDG_CACHE_HOME/dxmt9 → $HOME/Library/Caches/dxmt9
// Creates the directory (and any missing parents) on demand. Returns an
// empty string if no candidate is usable.
std::string resolveCacheRoot();

// Compose the archive filename:
//   ${cache_root}/dxmt9-shaders-v${ABI}.${gpu_family}.metallib-archive
// `gpuFamily` should be the sanitized device-name token (lowercase,
// spaces → underscores, ASCII-only). `cacheRoot` may be empty, in which
// case the returned path is empty (caller treats as "archive disabled").
std::string composeArchivePath(const std::string& cacheRoot,
                               const std::string& gpuFamily,
                               std::uint32_t abiVersion = kArchiveAbiVersion);

// Read MTLDevice.name() and sanitize it: lowercase, replace whitespace
// with '_', drop non-[a-z0-9_-.] characters. Empty input yields
// "unknown_gpu". Used as the gpu_family token in the archive filename.
std::string sanitizeGpuFamilyToken(WMT::Device device);

// Resolve the archive path for a given device using all of the above.
// Returns an empty string when prewarm is Disabled or when no cache root
// could be resolved (e.g. unwritable HOME). The returned path is also
// suitable for the existing shaders::Archive ctor.
std::string resolveArchivePath(WMT::Device device, Mode mode);

// Prewarm the archive at the given path against the given device. This
// runs the design §6.1 failure-mode table:
//   * Mode::Disabled → no-op.
//   * File missing → countPrewarmFailureMissing(); leave archive empty.
//   * File present but corrupt / schema mismatch → rename *.corrupt /
//     *.outdated, count failure class, start empty.
//   * Lock unavailable beyond 100 ms → countPrewarmFailureLockBusy();
//     fall through to compile-only.
//   * Mode::Lazy → skip load, but still validate that path is writable.
//     The compile path will write the archive on subsequent runs.
// On a successful Full load it calls countPrewarmEntriesLoaded(),
// countPrewarmLoadCpuTime(), and countArchiveBytes(). It never throws.
//
// Returns the loaded (or, on a classified corrupt/schema failure, the
// empty-fallback) `MTLBinaryArchive` reference produced by the
// classification probe itself — the probe already performs the real
// `newBinaryArchive(path)` deserialize, so R-BACK-3.9's async caller
// (beginAsyncFullLoad) reuses this single load instead of paying for a
// second one. Mode::Disabled, Mode::Lazy, an empty path, a missing file,
// or a lock_busy timeout all return an empty reference (caller treats
// that as "archive unavailable this session", exactly like today's
// disabled/lazy behavior).
WMT::Reference<WMT::BinaryArchive> run(WMT::Device device,
                                       const std::string& archivePath,
                                       Mode mode);

// Convert mode to a human-readable token for diagnostic logs. Stable
// strings: "full", "lazy", "disabled".
const char* modeName(Mode mode);

// ---------------------------------------------------------------------
// R-BACK-3.9 — size guard.
// ---------------------------------------------------------------------

// Default guard when DXMT9_ARCHIVE_MAX_PREWARM_MB is unset/unparseable/0:
// generous enough that a healthy per-app archive never trips it, but
// bounded so a probe-campaign-bloated archive (the 125 MB incident) can't
// re-block device init once Full prewarm becomes async.
inline constexpr std::uint64_t kDefaultMaxPrewarmMb = 512;

// Pure parse of the DXMT9_ARCHIVE_MAX_PREWARM_MB value. Returns nullopt
// for a null/empty/unparseable/zero string — callers substitute
// kDefaultMaxPrewarmMb in that case (matches the repo's "0/unparseable
// falls back to default" numeric-env convention).
std::optional<std::uint64_t> parseMaxPrewarmMb(const char* env) noexcept;

// Reads DXMT9_ARCHIVE_MAX_PREWARM_MB once and returns the guard in bytes
// (parseMaxPrewarmMb() result, or kDefaultMaxPrewarmMb, times 1<<20).
std::uint64_t resolveMaxPrewarmBytes();

// Pure decision: true when `archiveBytes` exceeds `maxPrewarmBytes` and
// the Full-mode load should demote to lazy behavior (skip the load,
// diagnostic log, keep the same path so an eventual save self-heals the
// oversized file) instead of paying for the deserialize.
bool shouldDemoteForSize(std::uint64_t archiveBytes,
                         std::uint64_t maxPrewarmBytes) noexcept;

// ---------------------------------------------------------------------
// R-BACK-3.9 — async load orchestration + backfill queue.
// ---------------------------------------------------------------------

// Begins the async Full-mode prewarm. Synchronously stat()s `archivePath`
// (cheap) and, when shouldDemoteForSize() trips, counts
// countPrewarmDemotedBySize(), logs one warning, and invokes `onAttach`
// with an empty reference without spawning a thread (the demoted case
// never attempts the expensive deserialize). Otherwise spawns and RETURNS
// a joinable std::thread that calls run(device, archivePath, Mode::Full),
// invokes `onAttach` with the result, then drains the backfill queue
// (queueArchiveBackfill jobs are just re-run; their return value is
// discarded, only the archive-add side effect matters). The caller
// (dxmt9::shaders::Archive) owns the returned thread and MUST join() it
// before the object that `onAttach` captures is destroyed — join()ing at
// shutdown may block briefly on in-flight I/O, which is an accepted
// trade-off (shutdown, not device-init, per R-BACK-3.9's scope).
//
// Sets archiveLoadInFlight() to true for the duration (from this call
// until `onAttach` and the backfill drain complete), including the
// synchronous demoted-by-size path so a compile that races a
// same-instant demotion still queues for backfill rather than assuming
// the (never coming) archive attach already happened.
std::thread beginAsyncFullLoad(
    WMT::Device device, std::string archivePath,
    std::function<void(WMT::Reference<WMT::BinaryArchive>)> onAttach);

// True while a beginAsyncFullLoad() call has not yet finished attaching
// (including backfill drain). Process-scoped: dxmt9 hosts exactly one
// shader archive per process, so this and queueArchiveBackfill() live
// here rather than as per-Archive-instance state, letting the
// pipeline-cache compile chokepoint (dxmt9_pipeline_cache.cpp
// submitPipelineBuild) query it without needing a reference back to the
// owning `shaders::Archive` object.
bool archiveLoadInFlight() noexcept;

// Queue a compile job whose archive-add side effect may have raced an
// in-flight load (it observed the archive handle as empty/unattached).
// `job` is invoked once, on the load thread, after attach; its return
// value (if any) is discarded — only calling it again (which re-runs
// `MTLDevice_newRenderPipelineStateWithDescriptor` with the now-attached
// archive handle, triggering `addRenderPipelineFunctionsWithDescriptor:`)
// matters. Bounded to kMaxArchiveBackfillQueue entries; excess jobs
// during a pathological compile burst are dropped (the corresponding PSO
// simply isn't preserved into the archive this session — a bounded,
// graceful degradation, not a crash or corruption).
inline constexpr std::size_t kMaxArchiveBackfillQueue = 256;
void queueArchiveBackfill(std::function<void()> job);

// R-BACK-3.10 quiescence-window activity signal. Called once per
// pipeline-cache compile-miss submission (regardless of pending/attached
// archive state or eventual compile success) so `Archive::notePresent()`
// can tell "new archive entries are still arriving" apart from "the
// compile set has stabilized". Process-scoped for the same reason as the
// backfill queue above.
void noteArchiveEntryCompiled() noexcept;
std::uint64_t archiveEntriesCompiledTotal() noexcept;

// ---------------------------------------------------------------------
// R-BACK-3.10 — milestone save.
// ---------------------------------------------------------------------

inline constexpr std::uint64_t kDefaultMilestonePresents = 300;
inline constexpr std::uint64_t kDefaultQuiescencePresents = 120;
// Minimum count of newly-compiled entries since the first mid-session
// save required to arm a second one ("a later large batch of new entries
// arrives" per R-BACK-3.10). Chosen well above normal per-scene trickle
// (a handful of one-off PSOs) so the second save is reserved for a
// genuinely new content area (level load, new effect), not steady-state
// noise.
inline constexpr std::uint64_t kDefaultReArmBatchEntries = 50;

// Pure value-transform state for the milestone-save decision. Owned by
// `dxmt9::shaders::Archive` (one instance per archive); passed by value
// so the decision function has no side effects and is unit-testable
// without a live Archive/Metal device.
struct MilestoneState {
  // Presents observed since this Archive was constructed.
  std::uint64_t presentCount = 0;
  // presentCount value the last time archiveEntriesCompiledTotal() moved
  // (0 = never observed a new entry yet).
  std::uint64_t lastNewEntryPresent = 0;
  // Snapshot of archiveEntriesCompiledTotal() as of the most recent
  // notePresent() call.
  std::uint64_t entriesAddedThisSession = 0;
  // entriesAddedThisSession value as of the most recent successful save.
  std::uint64_t entriesAddedAtLastSave = 0;
  // Saves already performed by this Archive instance this process.
  std::uint32_t savesPerformed = 0;
  // R-BACK-3.11 — true when this session's shader debug-env key is
  // non-default; all saves must be skipped regardless of the other
  // fields.
  bool savePoisoned = false;
};

// True when a mid-session save is due right now, given `state`. Rules
// (R-BACK-3.10):
//   * savePoisoned → never (R-BACK-3.11).
//   * No new entries since the last save → never (nothing to persist).
//   * presentCount must have reached `presentMilestone`.
//   * The compile set must be quiescent: at least `quiescencePresents`
//     presents must have elapsed since the last new entry arrived.
//   * savesPerformed == 0 → yes (the first bounded mid-session save).
//   * savesPerformed == 1 → yes only if at least `reArmBatchEntries` new
//     entries arrived since the first save (a genuinely new batch).
//   * savesPerformed >= 2 → never (bounded to at most two per process).
bool shouldMilestoneSave(
    const MilestoneState& state,
    std::uint64_t presentMilestone = kDefaultMilestonePresents,
    std::uint64_t quiescencePresents = kDefaultQuiescencePresents,
    std::uint64_t reArmBatchEntries = kDefaultReArmBatchEntries) noexcept;

// ---------------------------------------------------------------------
// R-BACK-3.10 — write-side POSIX flock (LOCK_EX). Mirrors the existing
// LOCK_SH reader contract (see run()'s implementation) but for writers,
// filling the gap between specs/backend/spec.md §6.1's documented
// "Writers acquire LOCK_EX on the archive file before serializeToURL:"
// design intent and the pre-R-BACK-3.10 code, which had no writer-side
// lock at all. `serializeToURL:`'s own atomic temp-file-plus-rename
// behavior (also documented in spec.md §6.1) is unchanged — this only
// adds the missing cross-process mutual exclusion around it.
// ---------------------------------------------------------------------

// Same bounded-retry contract as the reader lock (100 ms, 5 ms poll).
// Returns an owned fd on success, or -1 if the lock could not be
// acquired within `timeout` (or the path is empty / can't be opened).
// The fd must later be passed to releaseArchiveWriteLock().
int acquireArchiveWriteLock(const std::string& path);
void releaseArchiveWriteLock(int fd);

}  // namespace dxmt9::archive_prewarm
