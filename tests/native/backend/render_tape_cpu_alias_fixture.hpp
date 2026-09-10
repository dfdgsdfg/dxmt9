#pragma once

#include "render_tape_cpu_destroy_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace dxmt9::d3d9::test {

struct GeneralCpuAliasFixture {
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobs{};
};

namespace general_cpu_alias_fixture_detail {

inline constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 1u,
    .objectId = 71u,
};
inline constexpr D9CWireObjectIdentity kSurface{
    .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
    .generation = 1u,
    .objectId = 72u,
};

inline std::vector<std::byte> patchDrawTarget(
    std::vector<std::byte> bytes, const D9CWireObjectIdentity& target) {
  D9CCommandChunkWireHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  const D9CCommandChunkWireHandleEntry handle{
      .kind = target.kind,
      .generation = target.generation,
      .objectId = target.objectId,
  };
  std::memcpy(bytes.data() + header.handleTableOffset +
                  4u * sizeof(D9CCommandChunkWireHandleEntry),
              &handle, sizeof(handle));
  return bytes;
}

inline std::vector<std::byte> aliasBootstrapChunk() {
  return patchDrawTarget(general_cpu_destroy_fixture_detail::bootstrapChunk(),
                         general_cpu_destroy_fixture_detail::kOutput);
}

inline std::vector<std::byte> aliasDrawChunk() {
  return patchDrawTarget(general_cpu_destroy_fixture_detail::indexedDrawChunk(),
                         kSurface);
}

}  // namespace general_cpu_alias_fixture_detail

inline GeneralCpuAliasFixture makeGeneralCpuAliasFixture(
    bool destroyParentAfterFirstDraw = true, bool alternateParentSeed = false) {
  using namespace general_cpu_destroy_fixture_detail;
  using namespace general_cpu_alias_fixture_detail;

  struct TexturedVertex {
    float x;
    float y;
    float z;
    float rhw;
    std::uint32_t diffuse;
    float u;
    float v;
  };
  static_assert(sizeof(TexturedVertex) == 28u);
  const std::array vertices{
      TexturedVertex{-0.5f, -0.5f, 0.0f, 1.0f, 0xffff0000u, 0.0f, 0.0f},
      TexturedVertex{0.5f, -0.5f, 0.0f, 1.0f, 0xff00ff00u, 1.0f, 0.0f},
      TexturedVertex{0.0f, 0.5f, 0.0f, 1.0f, 0xff0000ffu, 0.5f, 1.0f},
  };
  auto vertexBytes = bytesOf(vertices);
  const std::array<std::uint16_t, 3u> indices{0u, 1u, 2u};
  auto indexBytes = bytesOf(indices);
  const std::array<std::uint32_t, 8u> shaderWords{
      0xfffe0300u, 0x0200001fu, 0x80000000u, 0xc00f0000u,
      0x02000001u, 0xc00f0000u, 0xa0e40000u, 0x0000ffffu,
  };
  auto shaderBytes = bytesOf(shaderWords);
  const std::array declaration{
      D9CVertexElement{0u, 0u, 3u, 0u, 9u, 0u},
      D9CVertexElement{0u, 16u, 4u, 0u, 10u, 0u},
      D9CVertexElement{0u, 20u, 1u, 0u, 5u, 0u},
      D9CVertexElement{0xffu, 0u, 17u, 0u, 0u, 0u},
  };
  auto declarationBytes = bytesOf(declaration);

  // The parent is a lockable render-target-capable color texture. The alias
  // surface descriptor has the level-0 layout and the Surface resource kind; no surface
  // mutation is used, so the only receipt is the parent's level-0 upload.
  const D9CSurfaceDesc aliasLevel{
      .format = 22u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 16u,
      .height = 16u,
      .depth = 1u,
  };
  std::vector<std::byte> textureDescriptor(sizeof(RenderTapeTextureDescriptorV2) +
                                            sizeof(aliasLevel));
  const RenderTapeTextureDescriptorV2 texture{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::memcpy(textureDescriptor.data(), &texture, sizeof(texture));
  auto textureLevel = aliasLevel;
  textureLevel.resourceType = 3u;
  std::memcpy(textureDescriptor.data() + sizeof(texture), &textureLevel,
              sizeof(textureLevel));
  std::vector<std::byte> textureBytes(
      static_cast<std::size_t>(aliasLevel.width) * aliasLevel.height * 4u);
  for (std::size_t index = 0u; index < textureBytes.size(); ++index)
    textureBytes[index] = static_cast<std::byte>(0x31u + (index & 0x7fu));
  if (alternateParentSeed) {
    for (auto& byte : textureBytes) byte ^= std::byte{0x5au};
  }
  const auto textureDigest = RenderTapeCaptureSession::sha256(textureBytes);
  const RenderTapeSurfaceDescriptorV2 aliasDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::TextureSubresource),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::Unavailable),
      .subresource = 0u,
      .parentTexture = kTexture,
      .surface = aliasLevel,
  };
  const D9CSurfaceDesc outputSurface{
      .format = 22u,
      .resourceType = 1u,
      .usage = 1u,
      .pool = 0u,
      .width = 16u,
      .height = 16u,
      .depth = 1u,
  };
  const RenderTapeSurfaceDescriptorV2 outputDescriptor{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(
          RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = outputSurface,
  };
  const D9CBufferDesc vertexBuffer{
      .size = static_cast<std::uint32_t>(vertexBytes.size()), .usage = 0x200u};
  const D9CBufferDesc indexBuffer{
      .size = static_cast<std::uint32_t>(indexBytes.size()),
      .usage = 0x200u,
      .format = 101u,
  };
  const RenderTapeShaderDescriptor shader{
      .stage = D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX,
      .bytecodeBytes = static_cast<std::uint32_t>(shaderBytes.size()),
  };
  const RenderTapeVertexDeclDescriptor vertexDeclaration{
      .elementCount = 4u,
      .elementBytes = static_cast<std::uint32_t>(declarationBytes.size()),
  };
  const auto vertexDigest = RenderTapeCaptureSession::sha256(vertexBytes);
  const auto indexDigest = RenderTapeCaptureSession::sha256(indexBytes);
  const auto shaderDigest = RenderTapeCaptureSession::sha256(shaderBytes);
  const auto declarationDigest =
      RenderTapeCaptureSession::sha256(declarationBytes);
  const RenderTapeOracleAttachment oracle{
      .identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder builder;
  const auto bootstrap = aliasBootstrapChunk();
  const auto draw = aliasDrawChunk();
  builder.appendBootstrapState(bootstrap);
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, textureBytes.size(), 1u);
  builder.appendObjectDefine(
      kSurface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&aliasDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      kVertexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&vertexBuffer, 1u)), 0u, {}, vertexBytes.size(), 1u);
  builder.appendObjectDefine(
      kIndexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&indexBuffer, 1u)), 0u, {}, indexBytes.size(), 1u);
  builder.appendObjectDefine(
      kVertexShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      std::as_bytes(std::span(&shader, 1u)), shaderBytes.size(), shaderDigest);
  builder.appendObjectDefine(
      kVertexDeclaration,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::VertexDeclaration),
      std::as_bytes(std::span(&vertexDeclaration, 1u)), declarationBytes.size(),
      declarationDigest);
  builder.appendResourceMutation(
      kTexture, RenderTapeMutationKind::Upload, 0u, 0u, textureBytes.size(),
      textureDigest);
  builder.appendResourceMutation(
      kVertexBuffer, RenderTapeMutationKind::Upload, 0u, 0u, vertexBytes.size(),
      vertexDigest);
  builder.appendResourceMutation(
      kIndexBuffer, RenderTapeMutationKind::Upload, 0u, 0u, indexBytes.size(),
      indexDigest);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u}, draw);
  if (destroyParentAfterFirstDraw)
    builder.appendObjectDestroy(kTexture);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u}, draw);
  if (!destroyParentAfterFirstDraw)
    builder.appendObjectDestroy(kTexture);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      general_cpu_destroy_fixture_detail::presentChunk());
  builder.appendPresentComplete(
      15u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));

  return GeneralCpuAliasFixture{
      .tape = builder.seal(),
      .blobs = {std::move(textureBytes), std::move(vertexBytes),
                std::move(indexBytes), std::move(shaderBytes),
                std::move(declarationBytes)},
  };
}

}  // namespace dxmt9::d3d9::test
