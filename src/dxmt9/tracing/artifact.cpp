#include "artifact.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace dxmt9::tracing::detail {
namespace {

std::atomic<uint64_t> next_temporary_id{1};

bool close_file(int& fd) noexcept {
  if (fd < 0) return true;
  const int value = fd;
  fd = -1;
  return close(value) == 0;
}

}  // namespace

AtomicArtifact::AtomicArtifact(std::string destination) noexcept
    : destination_(std::move(destination)) {}

AtomicArtifact::~AtomicArtifact() {
  abort();
}

bool AtomicArtifact::open() noexcept {
  try {
    if (fd_ >= 0 || destination_.empty()) return false;
    const std::size_t slash = destination_.find_last_of('/');
    if (slash == std::string::npos) {
      directory_ = ".";
    } else if (slash == 0) {
      directory_ = "/";
    } else {
      directory_ = destination_.substr(0, slash);
    }
    const std::string_view name(destination_.data() +
        (slash == std::string::npos ? 0 : slash + 1),
        destination_.size() - (slash == std::string::npos ? 0 : slash + 1));
    if (name.empty() || name == "." || name == "..") return false;

    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      const uint64_t id =
          next_temporary_id.fetch_add(1, std::memory_order_relaxed);
      temporary_ = directory_ + "/.dxmt9-artifact-" +
          std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
          std::to_string(static_cast<unsigned long long>(id));
      fd_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                   O_NOFOLLOW, S_IRUSR | S_IWUSR);
      if (fd_ >= 0) return true;
      if (errno != EEXIST) {
        temporary_.clear();
        return false;
      }
    }
    temporary_.clear();
    return false;
  } catch (...) {
    abort();
    return false;
  }
}

bool AtomicArtifact::write(const char* data, std::size_t size) noexcept {
  if (fd_ < 0 || (size && !data)) return false;
  std::size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd_, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (result == 0) return false;
    written += static_cast<std::size_t>(result);
  }
  return true;
}

bool AtomicArtifact::commit() noexcept {
  if (fd_ < 0 || temporary_.empty() || published_) return false;
  if (::fsync(fd_) != 0 || !close_file(fd_)) {
    abort();
    return false;
  }
  if (::link(temporary_.c_str(), destination_.c_str()) != 0) {
    abort();
    return false;
  }
  published_ = true;
  if (::unlink(temporary_.c_str()) != 0) return false;
  temporary_.clear();

  int directory_fd = ::open(directory_.c_str(), O_RDONLY | O_CLOEXEC |
                             O_DIRECTORY);
  if (directory_fd < 0) return false;
  const bool synced = ::fsync(directory_fd) == 0;
  const bool closed = close_file(directory_fd);
  return synced && closed;
}

void AtomicArtifact::abort() noexcept {
  (void)close_file(fd_);
  if (!temporary_.empty()) {
    (void)::unlink(temporary_.c_str());
    temporary_.clear();
  }
}

}  // namespace dxmt9::tracing::detail
