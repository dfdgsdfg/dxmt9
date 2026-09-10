#pragma once

// A small, authenticated general-frame tape for the AUTOGEN/mipmap path.
// The fixture intentionally carries only the visible level-0 seed.  Mipmap
// pixels are produced by the replayed GENERATE_MIPMAPS command and are not
// represented as a CPU-side receipt.

#include "render_tape_cpu_destroy_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::d3d9::test {

struct GeneralCpuMipmapFixture {
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobs{};
  D9CWireObjectIdentity texture{};
  std::size_t expectedDrawCount = 1u;
  std::size_t expectedMipmapCommandCount = 1u;
};

namespace general_cpu_mipmap_fixture_detail {

inline constexpr D9CWireObjectIdentity kTexture{
    .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
    .generation = 4u,
    .objectId = 59u,
};

inline std::vector<std::byte> generateMipmapsChunk() {
  const D9CCommandChunkWireGenerateMipmaps fixed{
      .textureHandleIndex = 0u};
  const D9CCommandChunkWireHandleEntry handle{
      .kind = kTexture.kind,
      .generation = kTexture.generation,
      .objectId = kTexture.objectId,
  };
  return general_cpu_destroy_fixture_detail::singleRecordChunk(
      D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
      std::as_bytes(std::span(&fixed, 1u)),
      std::span<const D9CCommandChunkWireHandleEntry>(&handle, 1u));
}

}  // namespace general_cpu_mipmap_fixture_detail

inline GeneralCpuMipmapFixture makeGeneralCpuMipmapFixture() {
  using namespace general_cpu_destroy_fixture_detail;
  using namespace general_cpu_mipmap_fixture_detail;

  const std::array<std::byte, 8u * 8u * 4u> seed = [] {
    std::array<std::byte, 8u * 8u * 4u> value{};
    for (std::size_t index = 0u; index < value.size(); ++index)
      value[index] = static_cast<std::byte>(0x20u + (index & 0x7fu));
    return value;
  }();
  std::vector<std::byte> textureBytes(seed.begin(), seed.end());
  const auto textureDigest = RenderTapeCaptureSession::sha256(textureBytes);

  // D3DUSAGE_AUTOGENMIPMAP is deliberately retained in the descriptor.  The
  // tape exposes one public level; the backend may own hidden physical levels,
  // but those are not part of the authenticated CPU seed.
  const D9CSurfaceDesc textureLevel{
      .format = 22u,
      .resourceType = 3u,
      .usage = 0x400u,
      .pool = 0u,
      .width = 8u,
      .height = 8u,
      .depth = 1u,
  };
  std::vector<std::byte> textureDescriptor(
      sizeof(RenderTapeTextureDescriptorV2) + sizeof(textureLevel));
  const RenderTapeTextureDescriptorV2 textureMetadata{
      .schemaVersion = kRenderTapeTextureDescriptorVersion2,
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u,
      .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed),
  };
  std::memcpy(textureDescriptor.data(), &textureMetadata,
              sizeof(textureMetadata));
  std::memcpy(textureDescriptor.data() + sizeof(textureMetadata),
              &textureLevel, sizeof(textureLevel));

  // Reuse the canonical draw resources and state overlay used by the general
  // destroy fixture.  The draw itself need not sample the AUTOGEN texture;
  // the explicit mip command carries the texture's exact handle closure.
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
      .descriptorKind = static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Surface),
  };

  RenderTapeBuilder builder;
  builder.appendBootstrapState(bootstrapChunk());
  builder.appendObjectDefine(
      kOutput, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&outputDescriptor, 1u)), 0u, {});
  builder.appendObjectDefine(
      kVertexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&vertexBuffer, 1u)), 0u, {}, vertexBytes.size(),
      1u);
  builder.appendObjectDefine(
      kIndexBuffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&indexBuffer, 1u)), 0u, {}, indexBytes.size(),
      1u);
  builder.appendObjectDefine(
      kVertexShader, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Shader),
      std::as_bytes(std::span(&shader, 1u)), shaderBytes.size(), shaderDigest);
  builder.appendObjectDefine(
      kVertexDeclaration,
      static_cast<std::uint32_t>(RenderTapeDescriptorKind::VertexDeclaration),
      std::as_bytes(std::span(&vertexDeclaration, 1u)), declarationBytes.size(),
      declarationDigest);
  builder.appendObjectDefine(
      kTexture, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Texture),
      textureDescriptor, 0u, {}, textureBytes.size(), 1u);

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
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 5u},
      indexedDrawChunk());
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      generateMipmapsChunk());
  builder.appendObjectDestroy(kTexture);
  builder.appendObjectDestroy(kVertexBuffer);
  builder.appendObjectDestroy(kIndexBuffer);
  builder.appendCommandChunk(
      CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u},
      general_cpu_destroy_fixture_detail::presentChunk());
  builder.appendPresentComplete(
      16u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));

  return GeneralCpuMipmapFixture{
      .tape = builder.seal(),
      .blobs = {std::move(textureBytes), std::move(vertexBytes),
                std::move(indexBytes), std::move(shaderBytes),
                std::move(declarationBytes)},
      .texture = kTexture,
  };
}

}  // namespace dxmt9::d3d9::test
