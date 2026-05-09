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
  // dxmt9 has no `dev_build` meson option at the time of writing, and
  // R-BACK-3.8 lets us choose the safest default unconditionally. Lazy
  // is the safest because it never blocks device init on archive I/O
  // and still warms the on-disk archive on subsequent runs.
  return Mode::Lazy;
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

void run(WMT::Device device, const std::string& archivePath, Mode mode) {
  // The actual archive load currently rides on the existing
  // shaders::Archive ctor (which calls MTLDevice_newBinaryArchive). This
  // routine only adds the lock / failure-class / housekeeping wrapper —
  // see the inner comment near `loadStart`. The `device` parameter
  // therefore has no Metal call site here yet; the follow-up commit
  // referenced in the TODO will route the load through this routine and
  // start touching `device` directly.
  (void)device;
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

  // STUB-LOAD NOTE: a fully integrated load would call
  // MTLDevice_newBinaryArchive(archivePath, ...) here and inspect the
  // returned NSError to disambiguate corrupt vs schema. The bridge does
  // attempt that load — it lives in shaders::Archive's ctor invoked
  // immediately after this routine returns — but the NSError class is
  // not surfaced through winemetal yet. Until that bridge surface lands
  // we run two cheap pre-load probes that catch the obvious failure
  // classes:
  //
  //   * `corrupt`: the file is too small to be a Metal binary archive.
  //     A real archive contains at least the macho-style header (a few
  //     hundred bytes) plus one entry. Anything under kMinArchiveBytes
  //     was either truncated by a crash or written by a bug.
  //   * `schema`: a companion `${archive}.abi` stamp file records the
  //     dxmt9 archive ABI version of the writer. If present and not
  //     equal to kArchiveAbiVersion, the on-disk archive was written
  //     against a different emitter / variant-key encoding. Per design
  //     §6.1 the file is renamed `.outdated` and we start empty. If the
  //     stamp is absent we proceed without classifying — the archive
  //     can only have been written by an older dxmt9 that did not
  //     stamp, in which case the load itself will reject it on schema
  //     and the bridge fallback returns an empty archive.
  //
  // These probes run before we hand the path to shaders::Archive; on a
  // hit we rename the file aside so the ctor opens an empty archive.
  // Both probes are conservative — false negatives just mean the actual
  // load will catch the same class without bumping the counter.

  constexpr std::uint64_t kMinArchiveBytes = 64;
  bool classifiedFailure = false;

  if (sizeBytes > 0 && sizeBytes < kMinArchiveBytes) {
    perf::countPrewarmFailureCorrupt();
    renameAside(archivePath, ".corrupt");
    classifiedFailure = true;
  } else {
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
