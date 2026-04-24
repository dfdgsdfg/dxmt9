#include "dxmt9_shader_archive.hpp"

#include "dxmt9_shader_sources.hpp"

#include <utility>

namespace dxmt9::shaders {

Archive::Archive(WMT::Device device, std::string path) : path_(std::move(path)) {
  ref_ = initShaderArchive(device, path_);
}

Archive::~Archive() {
  if (ref_) {
    persistShaderArchive(ref_, path_);
  }
}

Archive::Archive(Archive&&) noexcept = default;
Archive& Archive::operator=(Archive&&) noexcept = default;

}  // namespace dxmt9::shaders
