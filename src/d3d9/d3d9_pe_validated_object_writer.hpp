#pragma once

#include "d3d9_pe_device_child.hpp"

// This header is intentionally included only by validator implementation TUs.
// The public child header forward-declares the writer solely as a friend; no
// caller can mint or populate validation evidence through this type.
struct D3D9PeValidatedObjectWriter {
  template<typename Public, typename Raw, typename Wire>
  static void clear(
      D3D9PeValidatedObjectStorage<Public, Raw, Wire>* out) noexcept {
    if (!out) return;
    *out->object() = {};
  }

  template<typename Public, typename Raw, typename Wire>
  static void assign(
      D3D9PeValidatedObjectStorage<Public, Raw, Wire>* out, Public* publicIdentity,
      const void* ownerDevice, Raw* raw, const Wire& wire,
      std::uint64_t localMetadata,
      dxmt9::d3d9::pe::PeConcreteObjectKind kind) noexcept {
    if (!out) return;
    auto* object = out->object();
    object->publicIdentity_ = publicIdentity;
    object->ownerDevice_ = ownerDevice;
    object->raw_ = raw;
    object->wire_ = wire;
    object->localMetadata_ = localMetadata;
    object->kind_ = kind;
  }
};
