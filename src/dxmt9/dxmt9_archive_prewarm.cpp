#include "dxmt9_archive_prewarm.hpp"

#include "dxmt9_perf_counters.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dxmt9::archive_prewarm {

namespace {

// 100 ms cap on the read-side flock retry loop, per design §6.1.
constexpr auto kReaderLockTimeout = std::chrono::milliseconds(100);

// Polling interval inside the lock retry loop; cheap because almost all
// production runs hit the very first attempt.
constexpr auto kReaderLockPoll = std::chrono::milliseconds(5);

bool envFlagSet(const char* name) {
  const char* v = std::getenv(name);
  return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

bool fileExists(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::uint64_t fileSize(const std::string& path) {
  if (path.empty()) {
    return 0;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(st.st_size);
}

// Best-effort recursive mkdir. Returns true if the directory exists at
// the end. Permissions match XDG-style 0700; cache files only need to
// be visible to the owning user.
bool mkdirRecursive(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (::mkdir(path.c_str(), 0700) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  if (errno != ENOENT) {
    return false;
  }
  // Walk up to the parent.
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return false;
  }
  if (!mkdirRecursive(path.substr(0, slash))) {
    return false;
  }
  return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

// Ensure the parent directory of `filePath` exists.
bool ensureParentDir(const std::string& filePath) {
  const auto slash = filePath.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  return mkdirRecursive(filePath.substr(0, slash));
}

// Append-without-trailing-slash helper.
std::string joinPath(std::string a, const char* b) {
  if (a.empty()) {
    return b ? std::string(b) : std::string{};
  }
  if (a.back() == '/') {
    a.pop_back();
  }
  if (!b || b[0] == '\0') {
    return a;
  }
  a.push_back('/');
  a.append(b);
  return a;
}

}  // namespace

const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Disabled: return "disabled";
    case Mode::Lazy:     return "lazy";
    case Mode::Full:     return "full";
  }
  return "unknown";
}

Mode buildDefaultMode() {
  // dxmt9 has no `dev_build` meson option at the time of writing, so
  // pick the build-default off the standard `NDEBUG` macro:
  //   * Release / shipping builds (`buildtype=release|releaseplus`)
  //     define NDEBUG → Mode::Full so the first-frame PSO compile
  //     ratio benefits from the on-disk archive.
  //   * Debug / unoptimized builds leave NDEBUG undefined → Mode::Lazy
  //     so device init is never blocked on archive I/O during dev
  //     iteration. The compile path still writes back on shutdown so
  //     subsequent runs can promote to Full via DXMT9_PREWARM.
  // The env var DXMT9_PREWARM=full|lazy|disabled overrides either.
#ifdef NDEBUG
  return Mode::Full;
#else
  return Mode::Lazy;
#endif
}

Mode resolveMode() {
  const char* env = std::getenv("DXMT9_PREWARM");
  if (!env || env[0] == '\0') {
    return buildDefaultMode();
  }
  // Case-insensitive token match. Stable tokens documented in design.
  std::string token(env);
  std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (token == "full" || token == "shipping" || token == "1" || token == "on") {
    return Mode::Full;
  }
  if (token == "lazy" || token == "dev") {
    return Mode::Lazy;
  }
  if (token == "disabled" || token == "off" || token == "0" || token == "none") {
    return Mode::Disabled;
  }
  return buildDefaultMode();
}

std::string resolveCacheRoot() {
  if (const char* explicitRoot = std::getenv("DXMT9_CACHE_DIR");
      explicitRoot && explicitRoot[0] != '\0') {
    std::string root(explicitRoot);
    if (mkdirRecursive(root)) {
      return root;
    }
  }
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0] != '\0') {
    std::string root = joinPath(std::string(xdg), "dxmt9");
    if (mkdirRecursive(root)) {
      return root;
    }
  }
  if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
    std::string root = joinPath(joinPath(std::string(home), "Library"), "Caches");
    root = joinPath(std::move(root), "dxmt9");
    if (mkdirRecursive(root)) {
      return root;
    }
  }
  return {};
}

std::string sanitizeGpuFamilyToken(WMT::Device device) {
  std::string raw;
  if (device) {
    auto name = device.name();
    raw = name.getUTF8String();
  }
  if (raw.empty()) {
    return std::string("unknown_gpu");
  }
  std::string token;
  token.reserve(raw.size());
  for (char rawCh : raw) {
    auto ch = static_cast<unsigned char>(rawCh);
    if (std::isalnum(ch)) {
      token.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (ch == '_' || ch == '-' || ch == '.') {
      token.push_back(static_cast<char>(ch));
      continue;
    }
    if (std::isspace(ch)) {
      // Collapse runs of whitespace into a single underscore.
      if (!token.empty() && token.back() != '_') {
        token.push_back('_');
      }
      continue;
    }
    // Drop other punctuation.
  }
  // Trim trailing underscore.
  while (!token.empty() && token.back() == '_') {
    token.pop_back();
  }
  if (token.empty()) {
    return std::string("unknown_gpu");
  }
  return token;
}

std::string composeArchivePath(const std::string& cacheRoot,
                               const std::string& gpuFamily,
                               std::uint32_t abiVersion) {
  if (cacheRoot.empty()) {
    return {};
  }
  char filename[128]{};
  std::snprintf(filename, sizeof(filename),
                "dxmt9-shaders-v%u.%s.metallib-archive",
                static_cast<unsigned>(abiVersion),
                gpuFamily.empty() ? "unknown_gpu" : gpuFamily.c_str());
  return joinPath(cacheRoot, filename);
}

std::string resolveArchivePath(WMT::Device device, Mode mode) {
  if (mode == Mode::Disabled) {
    return {};
  }
  // Legacy escape hatch — keeps the existing
  // DXMT_DEBUG_DISABLE_SHADER_ARCHIVE behaviour: empty path → archive
  // module no-ops the open + serialize.
  if (envFlagSet("DXMT_DEBUG_DISABLE_SHADER_ARCHIVE")) {
    return {};
  }
  const auto cacheRoot = resolveCacheRoot();
  if (cacheRoot.empty()) {
    return {};
  }
  const auto family = sanitizeGpuFamilyToken(device);
  return composeArchivePath(cacheRoot, family);
}

namespace {

// flock(LOCK_SH | LOCK_NB) with a bounded retry budget. Returns the
// owned descriptor on success, or -1 when the lock could not be
// acquired within `timeout`. On other failures (open() error etc) also
// returns -1; the caller treats the absence of a descriptor uniformly
// as "unable to lock — fall through to compile-only".
int acquireSharedLock(const std::string& path, std::chrono::milliseconds timeout) {
  if (path.empty()) {
    return -1;
  }
  // O_RDONLY is enough — we only need flock(); we don't read through fd.
  // O_CLOEXEC keeps the lock from leaking into helper processes.
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
      return fd;
    }
    if (errno != EWOULDBLOCK) {
      ::close(fd);
      return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::close(fd);
      return -1;
    }
    std::this_thread::sleep_for(kReaderLockPoll);
  }
}

void releaseSharedLock(int fd) {
  if (fd < 0) {
    return;
  }
  ::flock(fd, LOCK_UN);
  ::close(fd);
}

// Atomically rename a damaged archive aside so the next attempt starts
// with an empty file. Best-effort — failures are silent.
void renameAside(const std::string& path, const char* suffix) {
  if (path.empty() || !suffix) {
    return;
  }
  std::string aside = path + suffix;
  // unlink any stale aside file from a prior session before rename so
  // ::rename does not silently overwrite different content on macOS.
  ::unlink(aside.c_str());
  ::rename(path.c_str(), aside.c_str());
}

}  // namespace

namespace {

// Mirror of MTLBinaryArchiveError (Metal/MTLBinaryArchive.h). Inlined here
// so the prewarm module does not pull <Metal/Metal.h>; the values are ABI
// for the framework and have not changed since macOS 11.
//   MTLBinaryArchiveErrorNone               = 0
//   MTLBinaryArchiveErrorInvalidFile        = 1  → corrupt
//   MTLBinaryArchiveErrorUnexpectedElement  = 2  → schema (closest
//       semantic match: an element from a different emitter / schema is
//       unexpected by the current Metal / dxmt9 ABI). The public Metal
//       header has no dedicated VersionMismatch code, so we treat
//       UnexpectedElement as the schema-mismatch signal.
//   MTLBinaryArchiveErrorCompilationFailure = 3  → ignored at prewarm
//       time (compile-on-load failures only surface during actual PSO
//       creation).
//   MTLBinaryArchiveErrorInternalError      = 4  → not classified;
//       reported through the existing magic-size fallback when present.
constexpr int64_t kMTLBinaryArchiveErrorInvalidFile       = 1;
constexpr int64_t kMTLBinaryArchiveErrorUnexpectedElement = 2;

enum class FailureClass {
  None,
  Corrupt,
  Schema,
};

// Classify NSError code under MTLBinaryArchiveDomain. Domain string isn't
// matched explicitly — Metal returns these codes only under its own
// domain, and any other domain (NSCocoaErrorDomain for I/O, etc.) is
// caught by the magic-size + abi-stamp fallbacks below.
FailureClass classifyArchiveError(int64_t errCode) {
  switch (errCode) {
    case kMTLBinaryArchiveErrorInvalidFile:
      return FailureClass::Corrupt;
    case kMTLBinaryArchiveErrorUnexpectedElement:
      return FailureClass::Schema;
    default:
      return FailureClass::None;
  }
}

}  // namespace

void run(WMT::Device device, const std::string& archivePath, Mode mode) {
  if (mode == Mode::Disabled || archivePath.empty()) {
    return;
  }
  // Make sure the parent directory exists so the compile path can
  // serialize on shutdown. Cheap, idempotent.
  ensureParentDir(archivePath);

  if (mode == Mode::Lazy) {
    // Lazy: do not actively load. The compile path still writes the
    // archive on subsequent runs via shaders::Archive::~Archive.
    return;
  }

  // Mode::Full from here on.

  if (!fileExists(archivePath)) {
    perf::countPrewarmFailureMissing();
    return;
  }

  const int lockFd = acquireSharedLock(archivePath, kReaderLockTimeout);
  if (lockFd < 0) {
    perf::countPrewarmFailureLockBusy();
    return;
  }

  const auto loadStart = std::chrono::steady_clock::now();
  const auto sizeBytes = fileSize(archivePath);

  // Primary classification: probe the real MTLBinaryArchive load and
  // inspect the returned NSError under MTLBinaryArchiveDomain. The
  // bridge falls back to creating an empty archive when the URL load
  // fails, so we can't rely on a null `archiveProbe` to mean "failed";
  // instead we look at `probeError.code()` directly.
  //
  //   MTLBinaryArchiveErrorInvalidFile        → corrupt
  //   MTLBinaryArchiveErrorUnexpectedElement  → schema mismatch
  //   any other / no error                    → treat as success here;
  //                                             fall through to the
  //                                             cheap fallbacks below.
  //
  // The probe load is independent of the shaders::Archive instance the
  // caller already constructed — Metal allows multiple archives over
  // the same URL, and the probe ref drops at end of scope. The cost is
  // dominated by the I/O the caller already paid; we keep it inside
  // the same shared-lock scope for cross-process consistency.
  bool classifiedFailure = false;
  {
    WMT::Error probeError{};
    auto archiveProbe = device.newBinaryArchive(archivePath.c_str(), probeError);
    (void)archiveProbe;
    const auto failure = classifyArchiveError(probeError.code());
    if (failure == FailureClass::Corrupt) {
      perf::countPrewarmFailureCorrupt();
      renameAside(archivePath, ".corrupt");
      classifiedFailure = true;
    } else if (failure == FailureClass::Schema) {
      perf::countPrewarmFailureSchema();
      renameAside(archivePath, ".outdated");
      renameAside(archivePath + ".abi", ".outdated");
      classifiedFailure = true;
    }
  }

  // Fallback probes — only consulted when the NSError-route did not
  // classify. These catch failure classes that nil out before the
  // MTLBinaryArchiveDomain code is set (older Metal versions, truncated
  // files that the framework refuses early without a domain code, or
  // dxmt9 ABI bumps the framework cannot detect on its own):
  //
  //   * `corrupt` (magic-size): a real archive contains at least a
  //     macho-style header plus one entry. Anything under
  //     kMinArchiveBytes was truncated by a crash or written by a bug.
  //   * `schema` (.abi stamp): a companion file records the writer's
  //     kArchiveAbiVersion. A mismatch means the on-disk archive was
  //     written against a different emitter / variant-key encoding —
  //     Metal's UnexpectedElement does not fire on a dxmt9-only ABI
  //     bump because the underlying MSL is still well-formed.
  //
  // Both fallbacks are conservative: false negatives just mean the
  // archive will be silently reset on the next compile path serialize.
  constexpr std::uint64_t kMinArchiveBytes = 64;

  if (!classifiedFailure && sizeBytes > 0 && sizeBytes < kMinArchiveBytes) {
    perf::countPrewarmFailureCorrupt();
    renameAside(archivePath, ".corrupt");
    classifiedFailure = true;
  }

  if (!classifiedFailure) {
    const std::string abiStamp = archivePath + ".abi";
    if (fileExists(abiStamp)) {
      std::uint32_t stampedAbi = 0;
      if (FILE* fp = std::fopen(abiStamp.c_str(), "r"); fp) {
        if (std::fscanf(fp, "%u", &stampedAbi) != 1) {
          stampedAbi = 0;
        }
        std::fclose(fp);
      }
      if (stampedAbi != kArchiveAbiVersion) {
        perf::countPrewarmFailureSchema();
        renameAside(archivePath, ".outdated");
        renameAside(abiStamp, ".outdated");
        classifiedFailure = true;
      }
    } else {
      // No stamp yet — write one for this writer's lifecycle so future
      // readers can detect the schema bump cheaply. Best-effort; failure
      // here is silent because the rest of the prewarm path remains
      // valid.
      if (FILE* fp = std::fopen(abiStamp.c_str(), "w"); fp) {
        std::fprintf(fp, "%u", static_cast<unsigned>(kArchiveAbiVersion));
        std::fclose(fp);
      }
    }
  }

  if (!classifiedFailure) {
    // We can't enumerate archive entries through the current bridge, so
    // report 1 entry-equivalent for "archive present and loaded" per
    // the task spec. Counters expose archive bytes directly so the
    // actual size is observable separately.
    perf::countPrewarmEntriesLoaded(sizeBytes > 0 ? 1u : 0u);
    perf::countArchiveBytes(sizeBytes);
  }

  const auto loadEnd = std::chrono::steady_clock::now();
  perf::countPrewarmLoadCpuTime(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(loadEnd - loadStart).count()));

  releaseSharedLock(lockFd);

  // Prefer the stable `dxmt9-archive` log token so triage tools can
  // filter on it without grepping the prewarm internals.
  dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-archive",
                    "prewarm done: path=\"%s\" mode=%s bytes=%llu%s",
                    archivePath.c_str(), modeName(mode),
                    static_cast<unsigned long long>(sizeBytes),
                    classifiedFailure ? " (renamed-aside)" : "");
}

}  // namespace dxmt9::archive_prewarm
