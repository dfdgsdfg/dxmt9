#include "../../../src/dxmt9/dxmt9_pending_clear_projection.hpp"

#include <cstdlib>
#include <iostream>

namespace pc = dxmt9::pending_clear;
namespace core = dxmt9::core;

namespace {
void check(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

core::metalqueue::PublishedCommandRef command(std::uint32_t index) {
  return {.source = {.id = {.index = 0, .generation = 2},
                     .storage = {.firstPage = 0, .pageCount = 1, .generation = 3}},
          .seqId = 4, .slotIndex = 0, .commandIndex = index};
}

void verify(dxmt9::CommandQueue& queue) {
  core::SurfaceDesc colorDesc{.width = 16, .height = 16,
                              .format = core::Format::A8R8G8B8,
                              .renderTarget = true};
  core::SurfaceDesc depthDesc{.width = 16, .height = 16,
                              .format = core::Format::D24S8,
                              .depthStencil = true};
  const auto color = queue.pool().createSurface(queue.device(), queue.limits(), colorDesc);
  const auto depth = queue.pool().createSurface(queue.device(), queue.limits(), depthDesc);
  check(color && depth, "real Pool creates both attachments");
  core::FlatDrawStateRecord draw{};
  draw.colorAttachments[0].handle = color;
  draw.depthStencil.handle = depth;
  core::ClearDesc clear{};
  clear.colorAttachments = draw.colorAttachments;
  clear.depthStencil = draw.depthStencil;
  clear.clearDepth = true;
  auto clearCommand = command(7), drawCommand = command(8);
  auto facts = [&] {
    return pc::projectFacts(queue, queue.pool(), clearCommand, drawCommand,
                            clear, draw, {false, false, false});
  };
  auto expect = [&](pc::Decision reason, const char* label) {
    check(pc::classify(facts()) == reason, label);
  };
  expect(pc::Decision::Admit, "depth-only Clear accepts matching real color+depth extent");
  check(facts().draw[4].aspects == (pc::Depth | pc::Stencil), "D24S8 includes both aspects");
  check(facts().followingColorLoads[0] == pc::Load::Discard,
        "untouched unrequested color keeps serial DontCare");
  clear.clearColor = true;
  expect(pc::Decision::FollowingLoadDiscardsClear, "requested untouched color rejects");
  queue.markColorHandleTouched(color);
  expect(pc::Decision::Admit, "touched requested color preserves Clear");
  clear.clearColor = false;

  for (std::size_t slot = 0; slot < core::kMaxTextures; ++slot) {
    draw.textures[slot] = depth;
    expect(pc::Decision::ReadHazard, "all fragment and vertex slots reject direct read hazard");
    draw.textures[slot] = {};
  }
  auto* surface = queue.pool().findSurface(depth.value);
  check(surface, "retained depth surface resolves");
  surface->aliasTexture = core::TextureHandle{123};
  for (std::size_t slot = 0; slot < core::kMaxTextures; ++slot) {
    draw.textures[slot] = surface->aliasTexture;
    expect(pc::Decision::ReadHazard, "all fragment and vertex slots reject sampled alias");
    draw.textures[slot] = {};
  }
  surface->aliasTexture = {};
  surface->level = 2;
  surface->slice = 3;
  check(facts().draw[4].viewLevel == 2 && facts().draw[4].slice == 3,
        "Pool view level and slice survive projection");
  surface->level = surface->slice = 0;

  ++draw.depthStencil.level;
  expect(pc::Decision::Identity, "bound subresource differs");
  --draw.depthStencil.level;
  draw.depthStencil.sampleCount = 2;
  expect(pc::Decision::SamplesOrResolve, "binding sample count rejects");
  draw.depthStencil.sampleCount = 1;
  surface->desc.multiSampleType = core::MultiSampleType::Two;
  expect(pc::Decision::SamplesOrResolve, "Pool sample count rejects");
  surface->desc.multiSampleType = core::MultiSampleType::None;
  surface->desc.width = 8;
  expect(pc::Decision::Unresolved, "descriptor/physical extent disagreement rejects");
  surface->desc.width = 16;
  surface->resolveTexture = surface->texture;
  expect(pc::Decision::SamplesOrResolve, "resolve target rejects");
  surface->resolveTexture = nullptr;

  auto changed = facts();
  changed = pc::projectFacts(queue, queue.pool(), clearCommand, drawCommand,
                             clear, draw, {true, false, false});
  check(pc::classify(changed) == pc::Decision::Scope, "active encoder scope preserved");
  changed = pc::projectFacts(queue, queue.pool(), clearCommand, drawCommand,
                             clear, draw, {false, true, false});
  check(pc::classify(changed) == pc::Decision::Scope, "query scope preserved");
  changed = pc::projectFacts(queue, queue.pool(), clearCommand, drawCommand,
                             clear, draw, {false, false, true});
  check(pc::classify(changed) == pc::Decision::ReadHazard, "exact hazard preserved");
  ++drawCommand.source.storage.generation;
  expect(pc::Decision::Order, "stale storage generation rejects");
  drawCommand = command(8);
  drawCommand.slotIndex = std::numeric_limits<std::uint32_t>::max();
  expect(pc::Decision::Order, "invalid publication reference rejects");
  drawCommand = command(9);
  expect(pc::Decision::Order, "nonadjacent command rejects");
  drawCommand = command(8);
  queue.currentBackBuffer_ = color;
  queue.backBufferDiscardAfterPresent_ = true;
  expect(pc::Decision::ConversionOrDiscard, "pending backbuffer discard rejects");
  queue.backBufferDiscardAfterPresent_ = false;
  clear.rects.push_back({0, 0, 16, 16});
  expect(pc::Decision::PartialRect, "explicit rect remains outside initial envelope");
  clear.rects.clear();
  check(!pc::projectAttachment(draw.depthStencil, nullptr).resolved,
        "missing surface cannot fabricate resolved facts");
  auto* colorSurface = queue.pool().findSurface(color.value);
  colorSurface->desc.format = core::Format::NullRt;
  expect(pc::Decision::Admit, "backed NULL RT participates exactly like serial descriptor");
  colorSurface->desc.format = core::Format::A8R8G8B8;
  expect(pc::Decision::Admit, "baseline restored");

  core::SurfaceDesc nullDesc = colorDesc;
  nullDesc.format = core::Format::NullRt;
  const auto nullSurface = queue.pool().createSurface(queue.device(), queue.limits(), nullDesc);
  check(nullSurface && !queue.pool().findSurface(nullSurface.value)->texture,
        "direct NULL RT surface has no physical storage");
  draw.colorAttachments[0].handle = nullSurface;
  expect(pc::Decision::Unresolved, "storage-free NULL RT remains outside envelope");
  core::TextureDesc nullTextureDesc{};
  nullTextureDesc.width = nullTextureDesc.height = 16;
  nullTextureDesc.format = core::Format::NullRt;
  nullTextureDesc.usage = core::UsageRenderTarget;
  const auto nullTexture = queue.pool().createTexture(queue.device(), queue.limits(), nullTextureDesc);
  const auto nullAlias = queue.pool().createSurfaceForTexture(nullTexture, 0, nullDesc);
  check(nullTexture && nullAlias && queue.pool().findSurface(nullAlias.value)->texture,
        "actual NULL RT texture alias has physical storage");
  draw.colorAttachments[0].handle = nullAlias;
  expect(pc::Decision::Admit, "actual backed NULL RT alias admits depth Clear");
  draw.colorAttachments[0].handle = color;

  draw.renderStates.entries[0] = {core::RS_SRGB_WRITE_ENABLE, 1};
  draw.renderStates.count = 1;
  expect(pc::Decision::ConversionOrDiscard, "actual flat sRGB state rejects");
  draw.renderStates = {};

  // Real texture aliases and mip views, not fabricated resource metadata.
  core::TextureDesc textureDesc{};
  textureDesc.width = textureDesc.height = 16;
  textureDesc.levels = 2;
  textureDesc.usage = core::UsageRenderTarget;
  const auto texture = queue.pool().createTexture(queue.device(), queue.limits(), textureDesc);
  colorDesc.width = colorDesc.height = 8;
  const auto mip = queue.pool().createSurfaceForTexture(texture, 1, colorDesc);
  check(texture && mip, "actual mip alias created");
  draw.colorAttachments[0].handle = mip;
  expect(pc::Decision::Extent, "actual smaller mip cannot under-clear full depth target");
  clear.clearDepth = false;
  clear.clearColor = true;
  clear.colorAttachments = draw.colorAttachments;
  draw.depthStencil = {};
  queue.markColorHandleTouched(mip);
  expect(pc::Decision::Admit, "real color-only mip admits");
  check(facts().draw[0].alias == texture.value && facts().draw[0].width == 8,
        "alias and physical view extent preserved");
  draw.textures[core::kMaxTextures - 1] = texture;
  expect(pc::Decision::ReadHazard, "last vertex sampler detects real mip alias");
}
} // namespace

int main() {
  auto devices = WMT::CopyAllDevices();
  if (!devices || !devices.count()) return 77;
  core::BackendLimits limits{};
  limits.supportsDepth24Stencil8 = false;
  dxmt9::CommandQueue queue(devices.object(0), limits, false);
  check(queue.valid() && queue.started(), "native queue starts");
  verify(queue);
  std::cout << "pending_clear_projection_spec passed (real Pool, 20 sampler slots; no fusion encoded)\n";
}
