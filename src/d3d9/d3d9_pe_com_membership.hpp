#pragma once

#include "dxmt9/device_c.h"

#include <cstdint>
#include <type_traits>

namespace dxmt9::d3d9::pe {

enum class PeConcreteObjectKind : std::uint8_t {
  Texture2D,
  CubeTexture,
  VolumeTexture,
  Surface,
  VertexBuffer,
  IndexBuffer,
  VertexShader,
  PixelShader,
  VertexDeclaration,
  Query,
};

enum class PeSurfaceQualification : std::uint8_t {
  Any,
  Standalone,
  TextureAlias,
};

// Built on the stack only after a TU-local dynamic_cast has proved concrete
// membership. This value is validation evidence, not an authentication token
// and is never stored in a COM wrapper.
struct PeConcreteMemberIdentity {
  PeConcreteObjectKind kind = PeConcreteObjectKind::Texture2D;
  const void* ownerDevice = nullptr;
  const void* publicIdentity = nullptr;
  D9CWireObjectIdentity wireIdentity{};
  bool surfaceAlias = false;
};

static_assert(std::is_trivially_copyable_v<PeConcreteMemberIdentity>);

constexpr std::uint32_t peConcreteWireKind(
    PeConcreteObjectKind kind) noexcept {
  switch (kind) {
  case PeConcreteObjectKind::Texture2D:
  case PeConcreteObjectKind::CubeTexture:
  case PeConcreteObjectKind::VolumeTexture:
    return D9C_CHUNK_HANDLE_KIND_TEXTURE;
  case PeConcreteObjectKind::Surface:
    return D9C_CHUNK_HANDLE_KIND_SURFACE;
  case PeConcreteObjectKind::VertexBuffer:
  case PeConcreteObjectKind::IndexBuffer:
    return D9C_CHUNK_HANDLE_KIND_BUFFER;
  case PeConcreteObjectKind::VertexShader:
  case PeConcreteObjectKind::PixelShader:
    return D9C_CHUNK_HANDLE_KIND_SHADER;
  case PeConcreteObjectKind::VertexDeclaration:
    return D9C_CHUNK_HANDLE_KIND_VERTEX_DECL;
  case PeConcreteObjectKind::Query:
    return D9C_CHUNK_HANDLE_KIND_QUERY;
  }
  return 0u;
}

constexpr bool validateConcreteMemberIdentity(
    const PeConcreteMemberIdentity& identity,
    PeConcreteObjectKind expectedKind, const void* expectedOwnerDevice,
    const void* expectedPublicIdentity,
    PeSurfaceQualification surfaceQualification =
        PeSurfaceQualification::Any) noexcept {
  if (!expectedPublicIdentity ||
      identity.publicIdentity != expectedPublicIdentity ||
      identity.ownerDevice != expectedOwnerDevice ||
      identity.kind != expectedKind ||
      identity.wireIdentity.kind != peConcreteWireKind(expectedKind) ||
      identity.wireIdentity.generation == 0u ||
      identity.wireIdentity.objectId == 0u) {
    return false;
  }
  if (expectedKind != PeConcreteObjectKind::Surface) {
    return !identity.surfaceAlias &&
           surfaceQualification == PeSurfaceQualification::Any;
  }
  switch (surfaceQualification) {
  case PeSurfaceQualification::Any:
    return true;
  case PeSurfaceQualification::Standalone:
    return !identity.surfaceAlias;
  case PeSurfaceQualification::TextureAlias:
    return identity.surfaceAlias;
  }
  return false;
}

}  // namespace dxmt9::d3d9::pe
