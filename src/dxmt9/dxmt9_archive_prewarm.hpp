#pragma once

// R-BACK-3.7 / R-BACK-3.8 / R-BACK-4.8 — MTLBinaryArchive prewarming with
// cross-process safety.
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
//   * Calls into the pre-existing perf::countPrewarm* counter slots.
//
// The prewarm flow is non-fatal: device init must never block on archive
// load failure. All counters live behind perf::; the module surface is
// the resolveArchivePath() value-transform plus a Prewarm::run() that
// owns the I/O.

#include "../winemetal/Metal.hpp"

#include <cstdint>
#include <string>

namespace dxmt9::archive_prewarm {

// dxmt9 archive ABI version. Bump whenever the MSL emitter output, the
// variant-key encoding, the MTLBinaryArchive schema we depend on, or the
// FFP key bit layout changes. Embedded into the filename so a version
// drift never reads from a stale archive.
inline constexpr std::uint32_t kArchiveAbiVersion = 1;

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
// `archive` is the already-constructed shader archive; the prewarm step
// only validates the path / file content. The actual load currently
// piggybacks on the existing winemetal `MTLDevice_newBinaryArchive`
// call — which already attempts URL-load with empty-archive fallback —
// so this routine does the file-existence probe + lock acquisition +
// rename-on-corrupt steps that wrap that call.
void run(WMT::Device device, const std::string& archivePath, Mode mode);

// Convert mode to a human-readable token for diagnostic logs. Stable
// strings: "full", "lazy", "disabled".
const char* modeName(Mode mode);

}  // namespace dxmt9::archive_prewarm
