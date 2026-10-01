#include "device_c_render_tape_capture_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

unsigned failures = 0u;

void check(bool accepted, const char* message) {
  if (!accepted) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

D9CSurfaceDesc volume(std::uint32_t format, std::uint32_t width,
                      std::uint32_t height, std::uint32_t depth) {
  return {.format = format, .resourceType = 4u,
          .width = width, .height = height, .depth = depth};
}

std::vector<std::byte> descriptor(const D9CSurfaceDesc& base,
                                  std::uint32_t mipCount, std::uint32_t pool) {
  const RenderTapeTextureDescriptorV2 header{
      .dimension = static_cast<std::uint32_t>(RenderTapeTextureDimension::Volume),
      .mipLevelCount = mipCount,
      .subresourceCount = mipCount,
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::CompleteSeed)};
  std::vector<std::byte> bytes(sizeof(header) + mipCount * sizeof(base));
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (std::uint32_t mip = 0u; mip < mipCount; ++mip) {
    auto desc = base;
    desc.pool = pool;
    desc.width = renderTapeTextureMipExtent(base.width, mip);
    desc.height = renderTapeTextureMipExtent(base.height, mip);
    desc.depth = renderTapeTextureMipExtent(base.depth, mip);
    std::memcpy(bytes.data() + sizeof(header) + mip * sizeof(desc), &desc,
                sizeof(desc));
  }
  return bytes;
}

void fullPaddedSlices() {
  const auto desc = volume(render_tape_d3d_format::A8R8G8B8, 2u, 3u, 3u);
  RenderTapeVolumeLockLayout layout{};
  check(renderTapeVolumeLockLayout(desc, 12, 48, nullptr, layout) ==
            RenderTapeVolumeLayoutStatus::Accepted && layout.fullSubresource &&
            layout.fullRowBytes == 8u && layout.fullRows == 3u &&
            layout.fullDepth == 3u && layout.fullSliceBytes == 24u &&
            layout.fullBytes == 72u && layout.sourceBytes == 128u,
        "padded row/slice strides retain a packed canonical full extent");
  std::vector<std::byte> physical(144u, std::byte{0xee});
  std::vector<std::byte> expected(72u);
  for (std::size_t slice = 0u; slice < 3u; ++slice) {
    for (std::size_t row = 0u; row < 3u; ++row) {
      for (std::size_t byte = 0u; byte < 8u; ++byte) {
        const auto value = std::byte{static_cast<unsigned char>(
            1u + slice * 24u + row * 8u + byte)};
        physical[slice * 48u + row * 12u + byte] = value;
        expected[slice * 24u + row * 8u + byte] = value;
      }
    }
  }
  std::vector<std::byte> copied;
  check(copyRenderTapeVolumeRows(physical.data(), layout, copied) &&
            copied == expected,
        "full copy includes every slice and strips both padding dimensions");
  std::vector<std::byte> written(144u, std::byte{0xee});
  check(writeRenderTapeVolumeRows(expected, written.data(), layout) &&
            written == physical,
        "full replay write preserves row/slice padding");
  std::vector<std::byte> complete;
  check(applyRenderTapeVolumeMutation(layout, copied, complete) ==
            RenderTapeBlockMutationStatus::Accepted && complete == expected,
        "full volume mutation establishes a previously missing mip seed");
}

void float32RgbaVolumeRowsMipsAndClosure() {
  constexpr auto format = render_tape_d3d_format::A32B32G32R32F;
  const auto desc = volume(format, 3u, 2u, 3u);
  RenderTapeVolumeLockLayout layout{};
  check(renderTapeVolumeLockLayout(desc, 64, 160, nullptr, layout) ==
            RenderTapeVolumeLayoutStatus::Accepted && layout.fullRowBytes == 48u &&
            layout.fullRows == 2u && layout.fullDepth == 3u &&
            layout.fullSliceBytes == 96u && layout.fullBytes == 288u &&
            layout.sourceBytes == 432u,
        "format116 Volume uses exact 16-byte rows with independent row/slice padding");
  std::vector<std::byte> expected(288u);
  for (std::size_t index = 0u; index < expected.size(); ++index)
    expected[index] = static_cast<std::byte>((index * 13u + 7u) & 0xffu);
  std::vector<std::byte> physical(480u, std::byte{0xe3});
  for (std::size_t z = 0u; z < 3u; ++z)
    for (std::size_t y = 0u; y < 2u; ++y)
      std::memcpy(physical.data() + z * 160u + y * 64u,
                  expected.data() + z * 96u + y * 48u, 48u);
  std::vector<std::byte> copied;
  check(copyRenderTapeVolumeRows(physical.data(), layout, copied) && copied == expected,
        "format116 capture retains raw component bytes across every padded slice");
  std::vector<std::byte> replay(480u, std::byte{0xe3});
  check(writeRenderTapeVolumeRows(copied, replay.data(), layout) && replay == physical,
        "format116 replay writes every slice without changing row or slice padding");
  const auto before = replay;
  check(!writeRenderTapeVolumeRows(std::span(copied).first(287u), replay.data(), layout) &&
            replay == before,
        "truncated format116 volume bytes reject atomically");
  const RenderTapeLockBox box{.left = 1, .top = 1, .right = 3,
                              .bottom = 2, .front = 1, .back = 3};
  RenderTapeVolumeLockLayout partial{};
  check(renderTapeVolumeLockLayout(desc, 64, 160, &box, partial) ==
            RenderTapeVolumeLayoutStatus::Accepted && partial.rowBytes == 32u &&
            partial.tightBytes == 64u && partial.subresourceSourceOffset == 240u,
        "format116 partial Volume box binds exact nonzero x/y/z component offsets");
  std::vector<std::byte> patch(64u, std::byte{0x5c});
  auto complete = expected;
  auto changed = expected;
  std::copy_n(patch.begin(), 32u, changed.begin() + 160u);
  std::copy_n(patch.begin() + 32u, 32u, changed.begin() + 256u);
  check(applyRenderTapeVolumeMutation(partial, patch, complete) ==
            RenderTapeBlockMutationStatus::Accepted && complete == changed &&
            std::equal(complete.begin(), complete.begin() + 96u, expected.begin()),
        "format116 nonzero-z mutation preserves slice zero and all unrelated texels");

  const auto base = volume(format, 7u, 5u, 3u);
  const auto sourceDescriptor = descriptor(base, 3u, 2u);
  const auto destinationDescriptor = descriptor(base, 3u, 0u);
  std::array<std::uint64_t, 3u> extents{};
  const auto contract = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_TEXTURE, sourceDescriptor, extents);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.count == 3u && contract.bytes == 1792u &&
            extents == std::array<std::uint64_t, 3u>{1680u, 96u, 16u},
        "format116 Volume mip closure shrinks all three dimensions at 16 bytes per texel");
  const std::vector<std::vector<std::byte>> seeds{
      std::vector<std::byte>(1680u, std::byte{0x11}),
      std::vector<std::byte>(96u, std::byte{0x22}),
      std::vector<std::byte>(16u, std::byte{0x33})};
  std::vector<std::vector<std::byte>> destination(3u);
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, seeds,
          destinationDescriptor, destination) == RenderTapeUpdateTextureStatus::Accepted &&
            destination == seeds,
        "format116 Volume UpdateTexture closes every exact mip seed without conversion");
  auto truncated = seeds;
  truncated.back().pop_back();
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, truncated,
          destinationDescriptor, destination) == RenderTapeUpdateTextureStatus::IncompleteSource &&
            destination == seeds,
        "truncated format116 final mip cannot partially overwrite destination closure");
  check(renderTapeVolumeLockLayout(desc, 64, 127, nullptr, layout) ==
            RenderTapeVolumeLayoutStatus::InvalidPitch,
        "format116 Volume requires a slice stride covering every full physical row");
  const auto overflow = volume(format, 134217728u, 1u, 1u);
  check(renderTapeVolumeContentLayout(overflow, layout) == RenderTapeVolumeLayoutStatus::Overflow,
        "format116 Volume row bytes exceeding signed lock pitch reject before copying");
}

void partialOffsetsAndAtomicRejection() {
  const auto desc = volume(render_tape_d3d_format::A8, 4u, 4u, 4u);
  const RenderTapeLockBox box{.left = 1, .top = 1, .right = 3,
                              .bottom = 3, .front = 1, .back = 3};
  RenderTapeVolumeLockLayout layout{};
  check(renderTapeVolumeLockLayout(desc, 6, 32, &box, layout) ==
            RenderTapeVolumeLayoutStatus::Accepted && !layout.fullSubresource &&
            layout.subresourceSourceOffset == 39u && layout.sourceBytes == 40u &&
            layout.subresourceSourceBytes == 79u && layout.tightBytes == 8u,
        "partial x/y/z box has an exact checked source extent");
  const std::vector<std::byte> patch{
      std::byte{101}, std::byte{102}, std::byte{103}, std::byte{104},
      std::byte{105}, std::byte{106}, std::byte{107}, std::byte{108}};
  std::vector<std::byte> physical(128u, std::byte{0xee});
  constexpr std::array<std::size_t, 8u> physicalOffsets{39, 40, 45, 46, 71, 72, 77, 78};
  for (std::size_t index = 0u; index < patch.size(); ++index)
    physical[physicalOffsets[index]] = patch[index];
  std::vector<std::byte> copied;
  check(copyRenderTapeVolumeRows(physical.data(), layout, copied,
                                  RenderTapeLockBitsOrigin::Subresource) &&
            copied == patch &&
            copyRenderTapeVolumeRows(physical.data() + 39u, layout, copied) &&
            copied == patch,
        "mip-base and returned-box pointers copy the same partial slices");
  std::vector<std::byte> written(128u, std::byte{0xee});
  check(writeRenderTapeVolumeRows(patch, written.data(), layout,
                                   RenderTapeLockBitsOrigin::Subresource) &&
            written == physical,
        "partial replay write touches exactly the requested box bytes");
  std::vector<std::byte> complete(64u, std::byte{0x22});
  auto expected = complete;
  constexpr std::array<std::size_t, 8u> canonicalOffsets{21, 22, 25, 26, 37, 38, 41, 42};
  for (std::size_t index = 0u; index < patch.size(); ++index)
    expected[canonicalOffsets[index]] = patch[index];
  check(applyRenderTapeVolumeMutation(layout, patch, complete) ==
            RenderTapeBlockMutationStatus::Accepted && complete == expected,
        "partial mutation preserves every canonical byte outside its x/y/z box");
  std::vector<std::byte> missing;
  check(applyRenderTapeVolumeMutation(layout, patch, missing) ==
            RenderTapeBlockMutationStatus::IncompleteSeed && missing.empty(),
        "partial volume mutation cannot invent a full seed");
  const auto prior = complete;
  auto corrupt = layout;
  ++corrupt.fullSliceBytes;
  check(applyRenderTapeVolumeMutation(corrupt, patch, complete) ==
            RenderTapeBlockMutationStatus::InvalidLayout && complete == prior,
        "forged canonical slice stride rejects atomically");
  check(applyRenderTapeVolumeMutation(layout, std::span(patch).first(7u), complete) ==
            RenderTapeBlockMutationStatus::InvalidBytes && complete == prior,
        "truncated partial bytes reject without modifying the seed");
  corrupt = layout;
  corrupt.fullSubresource = true;
  check(applyRenderTapeVolumeMutation(corrupt, patch, complete) ==
            RenderTapeBlockMutationStatus::InvalidLayout && complete == prior,
        "partial box cannot masquerade as a full seed");
  const auto priorCopy = copied;
  corrupt = layout;
  ++corrupt.sourceBytes;
  check(!copyRenderTapeVolumeRows(physical.data(), corrupt, copied) && copied == priorCopy,
        "corrupt source extent leaves the output copy intact");
  const auto priorPhysical = written;
  check(!writeRenderTapeVolumeRows(patch, written.data(), corrupt) &&
            written == priorPhysical,
        "corrupt replay extent rejects before writing any destination row");
}

void compressedSliceLayouts() {
  for (const auto format : {render_tape_d3d_format::DXT1,
                            render_tape_d3d_format::DXT3,
                            render_tape_d3d_format::DXT5}) {
    const auto desc = volume(format, 7u, 5u, 3u);
    const std::uint32_t blockBytes = format == render_tape_d3d_format::DXT1 ? 8u : 16u;
    RenderTapeVolumeLockLayout packed{};
    check(renderTapeVolumeContentLayout(desc, packed) ==
              RenderTapeVolumeLayoutStatus::Accepted && packed.fullRows == 2u &&
              packed.fullRowBytes == blockBytes * 2u &&
              packed.fullSliceBytes == blockBytes * 4u &&
              packed.fullBytes == blockBytes * 12u,
          "compressed volume canonical slices count block rows");
    const auto paddedPitch = static_cast<std::int32_t>(blockBytes * 3u);
    const auto paddedSlicePitch = static_cast<std::int64_t>(paddedPitch) * 5u;
    const RenderTapeLockBox edge{.left = 4, .top = 4, .right = 7,
                                 .bottom = 5, .front = 1, .back = 3};
    RenderTapeVolumeLockLayout partial{};
    check(renderTapeVolumeLockLayout(desc, paddedPitch, paddedSlicePitch,
                                      &edge, partial) ==
              RenderTapeVolumeLayoutStatus::Accepted && partial.rows == 1u &&
              partial.slices == 2u && partial.rowBytes == blockBytes &&
              partial.top == 1u && partial.destinationByteOffset == blockBytes &&
              partial.tightBytes == blockBytes * 2u,
          "edge-aligned partial DXT box retains explicit padded slice stride");
    std::vector<std::byte> complete(static_cast<std::size_t>(packed.fullBytes),
                                     std::byte{0x22});
    std::vector<std::byte> patch(blockBytes * 2u, std::byte{0x44});
    auto expected = complete;
    std::fill_n(expected.begin() + blockBytes * 7u, blockBytes, std::byte{0x44});
    std::fill_n(expected.begin() + blockBytes * 11u, blockBytes, std::byte{0x44});
    check(applyRenderTapeVolumeMutation(partial, patch, complete) ==
              RenderTapeBlockMutationStatus::Accepted && complete == expected,
          "compressed partial update changes the last block in two exact slices");
    const auto tiny = volume(format, 1u, 1u, 2u);
    check(renderTapeVolumeContentLayout(tiny, packed) ==
              RenderTapeVolumeLayoutStatus::Accepted &&
              packed.fullRowBytes == blockBytes && packed.fullRows == 1u &&
              packed.fullBytes == blockBytes * 2u,
          "tiny compressed volume mips retain one block per slice");
  }
}

void exhaustiveSmallBoxOverlays() {
  const auto desc = volume(render_tape_d3d_format::L8, 3u, 2u, 2u);
  std::vector<std::byte> physical(28u, std::byte{0xee});
  for (unsigned z = 0u; z < 2u; ++z)
    for (unsigned y = 0u; y < 2u; ++y)
      for (unsigned x = 0u; x < 3u; ++x)
        physical[z * 14u + y * 5u + x] =
            std::byte{static_cast<unsigned char>(1u + z * 6u + y * 3u + x)};
  for (int front = 0; front < 2; ++front) {
    for (int back = front + 1; back <= 2; ++back) {
      for (int top = 0; top < 2; ++top) {
        for (int bottom = top + 1; bottom <= 2; ++bottom) {
          for (int left = 0; left < 3; ++left) {
            for (int right = left + 1; right <= 3; ++right) {
              const RenderTapeLockBox box{left, top, right, bottom, front, back};
              RenderTapeVolumeLockLayout layout{};
              std::vector<std::byte> patch;
              std::vector<std::byte> complete(12u, std::byte{0xff});
              auto expected = complete;
              for (int z = 0; z < 2; ++z) {
                for (int y = 0; y < 2; ++y) {
                  for (int x = 0; x < 3; ++x) {
                    const auto index = static_cast<std::size_t>(z * 6 + y * 3 + x);
                    if (front <= z && z < back && top <= y && y < bottom &&
                        left <= x && x < right)
                      expected[index] = std::byte{static_cast<unsigned char>(index + 1u)};
                  }
                }
              }
              check(renderTapeVolumeLockLayout(desc, 5, 14, &box, layout) ==
                        RenderTapeVolumeLayoutStatus::Accepted &&
                        copyRenderTapeVolumeRows(physical.data(), layout, patch,
                                                   RenderTapeLockBitsOrigin::Subresource) &&
                        applyRenderTapeVolumeMutation(layout, patch, complete) ==
                            RenderTapeBlockMutationStatus::Accepted &&
                        complete == expected,
                    "every small legal box refines the coordinate-membership byte oracle");
            }
          }
        }
      }
    }
  }
}

void invalidLayoutsAndOverflow() {
  auto desc = volume(render_tape_d3d_format::A8R8G8B8, 4u, 4u, 4u);
  RenderTapeVolumeLockLayout layout{};
  for (const auto box : {
           RenderTapeLockBox{.left = -1, .right = 4, .bottom = 4, .back = 4},
           RenderTapeLockBox{.right = 4, .bottom = 4, .front = 3, .back = 2},
           RenderTapeLockBox{.right = 4, .bottom = 4, .back = 5},
           RenderTapeLockBox{.right = 5, .bottom = 4, .back = 4},
           RenderTapeLockBox{.right = 4, .bottom = 0, .back = 4}}) {
    check(renderTapeVolumeLockLayout(desc, 16, 64, &box, layout) ==
              RenderTapeVolumeLayoutStatus::InvalidExtent && layout.fullBytes == 0u,
          "invalid volume boxes reject with no usable output layout");
  }
  for (const auto pitches : {std::array<std::int64_t, 2u>{0, 64},
                             std::array<std::int64_t, 2u>{-16, 64},
                             std::array<std::int64_t, 2u>{15, 64},
                             std::array<std::int64_t, 2u>{16, -64},
                             std::array<std::int64_t, 2u>{16, 63}}) {
    check(renderTapeVolumeLockLayout(desc, static_cast<std::int32_t>(pitches[0]),
                                      pitches[1], nullptr, layout) ==
              RenderTapeVolumeLayoutStatus::InvalidPitch,
          "invalid row or full-slice pitch rejects");
  }
  desc = volume(render_tape_d3d_format::DXT1, 8u, 8u, 2u);
  const RenderTapeLockBox unaligned{.left = 1, .right = 8, .bottom = 8, .back = 2};
  check(renderTapeVolumeLockLayout(desc, 16, 32, &unaligned, layout) ==
            RenderTapeVolumeLayoutStatus::InvalidAlignment,
        "DXT volume origins require block alignment");
  desc = volume(render_tape_d3d_format::A8, 1u, 1u, 4u);
  check(renderTapeVolumeLockLayout(desc, 1, std::numeric_limits<std::int64_t>::max(),
                                    nullptr, layout) ==
            RenderTapeVolumeLayoutStatus::Overflow,
        "slice-address multiplication overflow rejects before copying");
  desc = volume(render_tape_d3d_format::A16B16G16R16, 268435455u,
                2147483647u, 2147483647u);
  check(renderTapeVolumeContentLayout(desc, layout) == RenderTapeVolumeLayoutStatus::Overflow,
        "canonical three-dimensional byte multiplication overflow rejects");
  desc = volume(render_tape_d3d_format::A8R8G8B8, 536870912u, 1u, 1u);
  check(renderTapeVolumeContentLayout(desc, layout) == RenderTapeVolumeLayoutStatus::Overflow,
        "canonical row pitch exceeding signed lock range rejects");
  desc = volume(0u, 1u, 1u, 1u);
  check(renderTapeVolumeContentLayout(desc, layout) ==
            RenderTapeVolumeLayoutStatus::UnsupportedFormat,
        "unknown volume format remains unsupported");
  desc = volume(render_tape_d3d_format::A8, 0u, 1u, 1u);
  check(renderTapeVolumeContentLayout(desc, layout) == RenderTapeVolumeLayoutStatus::InvalidExtent,
        "empty canonical volume extent rejects");
}

void seedContractsAndUpdateClosure() {
  const auto base = volume(render_tape_d3d_format::A8R8G8B8, 8u, 4u, 4u);
  const auto sourceDescriptor = descriptor(base, 4u, 2u);
  const auto destinationDescriptor = descriptor(base, 4u, 0u);
  std::array<std::uint64_t, 4u> extents{};
  auto contract = renderTapeDeriveExpectedContentContract(
      D9C_CHUNK_HANDLE_KIND_TEXTURE, sourceDescriptor, extents);
  check(contract.status == RenderTapeExpectedContentStatus::Accepted &&
            contract.count == 4u && contract.bytes == 588u &&
            extents == std::array<std::uint64_t, 4u>{512, 64, 8, 4},
        "volume seed closure shrinks width, height and depth for every mip");
  check(renderTapeValidateExpectedContentExtents(D9C_CHUNK_HANDLE_KIND_TEXTURE,
          sourceDescriptor, extents) == RenderTapeExpectedContentStatus::Accepted,
        "exact volume mip extents satisfy initial-content validation");
  --extents[2];
  check(renderTapeValidateExpectedContentExtents(D9C_CHUNK_HANDLE_KIND_TEXTURE,
          sourceDescriptor, extents) == RenderTapeExpectedContentStatus::InvalidExtent,
        "one truncated volume mip rejects the seed contract");
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE, .generation = 2u, .objectId = 7u};
  const auto missing = renderTapeDescribeMissingSeed(identity, sourceDescriptor, 1u);
  check(missing.expectedTightBytesValid && missing.expectedTightBytes == 64u &&
            missing.missingSurface.depth == 2u,
        "missing-seed attribution reports exact volume mip bytes");
  auto depthOne = volume(render_tape_d3d_format::L8, 3u, 2u, 1u);
  check(renderTapeDeriveExpectedTextureSubresourceContent(depthOne).bytes == 6u,
        "depth-one Volume retains a complete texture seed");
  check(renderTapeDeriveExpectedSurfaceContent(base).status ==
            RenderTapeExpectedContentStatus::InvalidExtent,
        "standalone surface byte contracts remain two-dimensional");
  std::vector<std::vector<std::byte>> source{
      std::vector<std::byte>(512u, std::byte{0x11}),
      std::vector<std::byte>(64u, std::byte{0x22}),
      std::vector<std::byte>(8u, std::byte{0x33}),
      std::vector<std::byte>(4u, std::byte{0x44})};
  std::vector<std::vector<std::byte>> destination(4u);
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, source,
          destinationDescriptor, destination) == RenderTapeUpdateTextureStatus::Accepted &&
            destination == source,
        "matching Volume UpdateTexture closes every mip and slice");
  const auto prior = destination;
  source.back().pop_back();
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, source,
          destinationDescriptor, destination) == RenderTapeUpdateTextureStatus::IncompleteSource &&
            destination == prior,
        "incomplete final mip leaves every destination mip unchanged");
  source = prior;
  auto mismatchedBase = base;
  mismatchedBase.depth = 2u;
  const auto mismatchedDepth = descriptor(mismatchedBase, 4u, 0u);
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, source,
          mismatchedDepth, destination) == RenderTapeUpdateTextureStatus::DescriptorMismatch &&
            destination == prior,
        "depth mismatch rejects atomically even with matching rows");
  mismatchedBase = base;
  mismatchedBase.format = render_tape_d3d_format::X8R8G8B8;
  const auto mismatchedFormat = descriptor(mismatchedBase, 4u, 0u);
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, source,
          mismatchedFormat, destination) == RenderTapeUpdateTextureStatus::DescriptorMismatch &&
            destination == prior,
        "same-size different-format Volume copy remains rejected");
  const auto systemDestination = descriptor(base, 4u, 2u);
  check(applyRenderTapeUpdateTextureClosure(sourceDescriptor, source,
          systemDestination, destination) == RenderTapeUpdateTextureStatus::DescriptorMismatch &&
            destination == prior,
        "Volume closure retains the SYSTEMMEM-to-destination pool contract");
  const auto compressedBase = volume(render_tape_d3d_format::DXT5, 8u, 8u, 4u);
  const auto compressedSource = descriptor(compressedBase, 4u, 2u);
  const auto compressedDestination = descriptor(compressedBase, 4u, 0u);
  const std::vector<std::vector<std::byte>> compressedBytes{
      std::vector<std::byte>(256u, std::byte{1}),
      std::vector<std::byte>(32u, std::byte{2}),
      std::vector<std::byte>(16u, std::byte{3}),
      std::vector<std::byte>(16u, std::byte{4})};
  destination.assign(4u, {});
  check(applyRenderTapeUpdateTextureClosure(compressedSource, compressedBytes,
          compressedDestination, destination) == RenderTapeUpdateTextureStatus::Accepted &&
            destination == compressedBytes,
        "compressed Volume UpdateTexture closes packed block-row mip slices");
}

void sharedOwnerLockState() {
  RenderTapeVolumeLockState state{};
  bool wasWritable = true;
  check(!state.take(wasWritable) && !wasWritable && !state.active && !state.writable,
        "an unmatched unlock reports no writable mutation");
  check(state.begin(true) && state.active && !state.writable,
        "read-only successful lock establishes owner state without writable arm guard");
  check(!state.begin(false) && state.active && !state.writable,
        "double begin cannot promote an active read-only lock to writable");
  check(state.take(wasWritable) && !wasWritable && !state.active && !state.writable,
        "read-only unlock consumes owner state without a mutation");
  check(state.begin(false) && state.active && state.writable,
        "writable successful lock activates the arm guard");
  check(!state.begin(true) && state.active && state.writable,
        "double begin cannot demote an active writable lock");
  // A failed runtime unlock does not call take; a subsequent wrapper observes
  // the same owning texture/mip state and consumes it after successful unlock.
  const auto retained = state;
  check(retained.active && retained.writable && state.active && state.writable,
        "failed runtime unlock leaves owner lock state available to a retry");
  check(state.take(wasWritable) && wasWritable && !state.active && !state.writable,
        "successful cross-wrapper unlock consumes one writable mutation");
  check(!state.take(wasWritable) && !wasWritable,
        "duplicate successful-unlock notification cannot repeat a mutation");
  std::array<RenderTapeVolumeLockState, 2u> mips{};
  check(mips[0].begin(false) && mips[1].begin(true) &&
            mips[1].take(wasWritable) && !wasWritable &&
            mips[0].active && mips[0].writable && !mips[1].active,
        "per-mip read-only unlock preserves a different mip's writable lock");
}

} // namespace

int main() {
  fullPaddedSlices();
  float32RgbaVolumeRowsMipsAndClosure();
  partialOffsetsAndAtomicRejection();
  compressedSliceLayouts();
  exhaustiveSmallBoxOverlays();
  invalidLayoutsAndOverflow();
  seedContractsAndUpdateClosure();
  sharedOwnerLockState();
  return failures == 0u ? 0 : 1;
}
