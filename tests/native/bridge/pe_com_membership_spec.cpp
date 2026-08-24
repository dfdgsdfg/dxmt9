#include "d3d9_pe_com_membership.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

using dxmt9::d3d9::pe::PeConcreteMemberIdentity;
using dxmt9::d3d9::pe::PeConcreteObjectKind;
using dxmt9::d3d9::pe::PeSurfaceQualification;

void testExactConcreteMembers() {
  constexpr std::array kinds{
      PeConcreteObjectKind::Texture2D,
      PeConcreteObjectKind::CubeTexture,
      PeConcreteObjectKind::VolumeTexture,
      PeConcreteObjectKind::Surface,
      PeConcreteObjectKind::VertexBuffer,
      PeConcreteObjectKind::IndexBuffer,
      PeConcreteObjectKind::VertexShader,
      PeConcreteObjectKind::PixelShader,
      PeConcreteObjectKind::VertexDeclaration,
      PeConcreteObjectKind::Query,
  };
  const auto* owner = reinterpret_cast<const void*>(0x1000u);
  const auto* object = reinterpret_cast<const void*>(0x2000u);
  for (std::size_t index = 0u; index < kinds.size(); ++index) {
    const PeConcreteMemberIdentity identity{
        .kind = kinds[index],
        .ownerDevice = owner,
        .publicIdentity = object,
        .wireIdentity = {
            .kind = dxmt9::d3d9::pe::peConcreteWireKind(kinds[index]),
            .generation = static_cast<std::uint32_t>(index + 1u),
            .objectId = index + 11u,
        },
    };
    check(dxmt9::d3d9::pe::validateConcreteMemberIdentity(
              identity, kinds[index], owner, object),
          "exact post-RTTI concrete members are accepted");
    check(!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
              identity, kinds[index], reinterpret_cast<const void*>(0x3000u),
              object),
          "foreign owning device is rejected");
    check(!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
              identity, kinds[index], owner,
              reinterpret_cast<const void*>(0x2010u)),
          "noncanonical public identity is rejected");
  }
}

void testKindWireAndAliasQualification() {
  const auto* owner = reinterpret_cast<const void*>(0x1000u);
  const auto* object = reinterpret_cast<const void*>(0x2000u);
  PeConcreteMemberIdentity identity{
      .kind = PeConcreteObjectKind::Surface,
      .ownerDevice = owner,
      .publicIdentity = object,
      .wireIdentity = {
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = 7u,
          .objectId = 9u,
      },
      .surfaceAlias = true,
  };
  check(dxmt9::d3d9::pe::validateConcreteMemberIdentity(
            identity, PeConcreteObjectKind::Surface, owner, object,
            PeSurfaceQualification::TextureAlias),
        "texture-level surface satisfies alias qualification");
  check(!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
            identity, PeConcreteObjectKind::Surface, owner, object,
            PeSurfaceQualification::Standalone),
        "texture-level surface cannot pass a standalone boundary");
  identity.wireIdentity.kind = D9C_CHUNK_HANDLE_KIND_TEXTURE;
  check(!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
            identity, PeConcreteObjectKind::Surface, owner, object),
        "wire kind must match the proven concrete type");
  identity.wireIdentity.kind = D9C_CHUNK_HANDLE_KIND_SURFACE;
  identity.wireIdentity.generation = 0u;
  check(!dxmt9::d3d9::pe::validateConcreteMemberIdentity(
            identity, PeConcreteObjectKind::Surface, owner, object),
        "zero generation is not a valid wire identity");
}
}  // namespace

int main() {
  try {
    testExactConcreteMembers();
    testKindWireAndAliasQualification();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_com_membership_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_com_membership_spec passed\n";
  return EXIT_SUCCESS;
}
