#pragma once

#include <cstddef>
#include <string>

namespace dxmt9::tracing::detail {

// Publishes one artifact in its destination directory without replacing an
// existing path. Temporary files are private, exclusive, and cleaned up by
// this object unless publication succeeds.
class AtomicArtifact {
 public:
  explicit AtomicArtifact(std::string destination) noexcept;
  AtomicArtifact(const AtomicArtifact&) = delete;
  AtomicArtifact& operator=(const AtomicArtifact&) = delete;
  ~AtomicArtifact();

  bool open() noexcept;
  bool write(const char* data, std::size_t size) noexcept;
  bool commit() noexcept;
  void abort() noexcept;

 private:
  std::string destination_;
  std::string temporary_;
  std::string directory_;
  int fd_ = -1;
  bool published_ = false;
};

}  // namespace dxmt9::tracing::detail
