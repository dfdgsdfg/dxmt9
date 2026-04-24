#pragma once

// Shader-cache / binary-archive service. Holds the WMT::BinaryArchive
// reference + on-disk cache path, and persists the archive on
// destruction. Previously the persist step lived inside
// CommandQueue::~CommandQueue — moved here so the execution service
// (CommandQueue) is free of persistence responsibility.

#include "../winemetal/Metal.hpp"

#include <string>

namespace dxmt9::shaders {

class Archive {
 public:
  Archive() = default;
  // Opens the archive at `path` for the given device. On success,
  // reference() is non-null. On failure (null device, missing/malformed
  // archive), remains empty — valid() returns false and persist is a
  // no-op.
  Archive(WMT::Device device, std::string path);
  ~Archive();

  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;
  Archive(Archive&&) noexcept;
  Archive& operator=(Archive&&) noexcept;

  bool valid() const noexcept { return static_cast<bool>(ref_); }
  WMT::Reference<WMT::BinaryArchive>& reference() noexcept { return ref_; }
  const std::string& path() const noexcept { return path_; }

 private:
  WMT::Reference<WMT::BinaryArchive> ref_{};
  std::string path_{};
};

}  // namespace dxmt9::shaders
