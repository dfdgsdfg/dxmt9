#include "device_c_provider.hpp"
#include "device_c_chunk_v2_registry.hpp"

#include <cstring>

namespace dxmt9::d3d9 {

static_assert(v2RecordSchemaComplete());

}  // namespace dxmt9::d3d9

namespace {

int32_t unavailableWireIdentity(D9CWireObjectIdentity* out) {
  if (out) {
    std::memset(out, 0, sizeof(*out));
  }
  return dxmt9::core::D3DERR_INVALIDCALL;
}

}  // namespace

extern "C" int32_t dxmt9c_device_negotiate_command_chunk(
    D9CDevice* device, D9CCommandChunkNegotiation* negotiation) {
  if (!device || !negotiation || negotiation->reserved0 != 0u ||
      negotiation->reserved1 != 0u || negotiation->reserved2 != 0u ||
      negotiation->reserved3 != 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  negotiation->unixSupportedVersions = D9C_COMMAND_CHUNK_CAP_VERSION_1;
  negotiation->selectedVersion = 0u;
  if ((negotiation->peSupportedVersions &
       D9C_COMMAND_CHUNK_CAP_VERSION_1) == 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  negotiation->selectedVersion = D9C_COMMAND_CHUNK_VERSION;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_get_wire_identity(
    D9CTexture*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}

extern "C" int32_t dxmt9c_buffer_get_wire_identity(
    D9CBuffer*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}

extern "C" int32_t dxmt9c_surface_get_wire_identity(
    D9CSurface*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}

extern "C" int32_t dxmt9c_shader_get_wire_identity(
    D9CShader*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}

extern "C" int32_t dxmt9c_vdecl_get_wire_identity(
    D9CVertexDecl*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}

extern "C" int32_t dxmt9c_query_get_wire_identity(
    D9CQuery*, D9CWireObjectIdentity* out) {
  return unavailableWireIdentity(out);
}
