#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_render_pass_internal.hpp"
#include "../../../src/dxmt9/dxmt9_query_policy.hpp"

#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace dxmt9 {
struct CommandQueueArenaLeaseTestAccess {
  static encoders::EncodeContext context(CommandQueue& queue) {
    return queue.makeEncodeContext();
  }
};
}

namespace {
using namespace dxmt9;
using namespace dxmt9::core;
using namespace dxmt9::encoders;

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

// Compare semantic fields rather than padding or process-specific handles.
template <typename T> auto attachment(const T& a) {
  return std::tuple(a.texture, a.load_action, a.store_action,
                    a.level, a.slice, a.depth_plane);
}
auto summary(const RenderPassActionSummary& s) {
  static_assert(sizeof(s) == 19u * sizeof(std::uint64_t));
  return std::bit_cast<std::array<std::uint64_t, 19>>(s);
}
void equal(const PreparedRenderPass& a, const PreparedRenderPass& b) {
  check(a.valid == b.valid && a.primaryWidth == b.primaryWidth &&
        a.primaryHeight == b.primaryHeight &&
        a.discardAfterPresent == b.discardAfterPresent, "prepared metadata");
  check(summary(a.actions) == summary(b.actions) &&
        summary(a.lateStore.summary) == summary(b.lateStore.summary), "action summaries");
  const auto& x = a.info;
  const auto& y = b.info;
  for (std::size_t i = 0; i < 8; ++i) {
    const auto& p = x.colors[i];
    const auto& q = y.colors[i];
    check(attachment(p) == attachment(q) &&
          std::tie(p.clear_color.r, p.clear_color.g, p.clear_color.b, p.clear_color.a,
                   p.resolve_texture, p.resolve_level, p.resolve_slice, p.resolve_depth_plane) ==
          std::tie(q.clear_color.r, q.clear_color.g, q.clear_color.b, q.clear_color.a,
                   q.resolve_texture, q.resolve_level, q.resolve_slice, q.resolve_depth_plane),
          "color descriptor");
  }
  check(attachment(x.depth) == attachment(y.depth) &&
        x.depth.clear_depth == y.depth.clear_depth &&
        x.depth.resolve_texture == y.depth.resolve_texture &&
        x.depth.resolve_filter == y.depth.resolve_filter, "depth descriptor");
  check(attachment(x.stencil) == attachment(y.stencil) &&
        x.stencil.clear_stencil == y.stencil.clear_stencil, "stencil descriptor");
  check(std::tie(x.default_raster_sample_count, x.render_target_array_length,
                 x.tile_width, x.tile_height, x.num_sample_buffer_attachments,
                 x.render_target_height, x.render_target_width, x.visibility_buffer) ==
        std::tie(y.default_raster_sample_count, y.render_target_array_length,
                 y.tile_width, y.tile_height, y.num_sample_buffer_attachments,
                 y.render_target_height, y.render_target_width, y.visibility_buffer),
        "pass descriptor");
  for (std::size_t i = 0; i < 4; ++i) {
    const auto& p = x.sample_buffer_attachments[i];
    const auto& q = y.sample_buffer_attachments[i];
    check(std::tie(p.sample_buffer, p.start_of_encoder_sample_index, p.end_of_encoder_sample_index) ==
          std::tie(q.sample_buffer, q.start_of_encoder_sample_index, q.end_of_encoder_sample_index),
          "sample attachment");
  }
  check(a.lateStore.count == b.lateStore.count, "ledger count");
  for (std::size_t i = 0; i < a.lateStore.count; ++i) {
    const auto& p = a.lateStore.attachments[i];
    const auto& q = b.lateStore.attachments[i];
    check(std::tie(p.handle, p.aliasTexture, p.pixelBytes, p.action, p.aspect, p.colorIndex, p.accounted) ==
          std::tie(q.handle, q.aliasTexture, q.pixelBytes, q.action, q.aspect, q.colorIndex, q.accounted),
          "attachment ledger");
  }
}

void run() {
  check(query::metalOcclusionEnabled(), "query preservation required");
  check(renderPassStoreProofRequired(true, perf::enabled()) == perf::enabled(),
        "exercise expected proof mode");
  auto devices = WMT::CopyAllDevices();
  check(devices && devices.count(), "Metal device");
  auto upper = CreateDXMT9Device(DEVICE_DESC{.device = devices.object(0u)});
  check(bool(upper), "upper device");
  auto ctx = CommandQueueArenaLeaseTestAccess::context(upper->queue());
  std::size_t cases = 0;
  for (const auto [samples, format] : {
           std::pair{MultiSampleType::None, Format::A8R8G8B8},
           std::pair{MultiSampleType::None, Format::NullRt},
           std::pair{MultiSampleType::Two, Format::A8R8G8B8},
           std::pair{MultiSampleType::Two, Format::NullRt}}) {
    for (auto depthFormat : {Format::D16, Format::D24S8, Format::S8_LOCKABLE}) {
      FlatDrawStateRecord hot{};
      SurfaceDesc colorDesc{.width = 16, .height = 16, .format = format, .renderTarget = true};
      SurfaceDesc depthDesc{.width = 16, .height = 16, .format = depthFormat, .depthStencil = true};
      colorDesc.multiSampleType = depthDesc.multiSampleType = samples;
      hot.colorAttachments[0].handle = ctx.pool.createSurface(ctx.device, ctx.limits, colorDesc);
      colorDesc.format = Format::A8R8G8B8;
      hot.colorAttachments[1].handle = ctx.pool.createSurface(ctx.device, ctx.limits, colorDesc);
      hot.depthStencil.handle = ctx.pool.createSurface(ctx.device, ctx.limits, depthDesc);
      hot.colorAttachments[0].sampleCount = hot.colorAttachments[1].sampleCount =
          hot.depthStencil.sampleCount = static_cast<std::uint32_t>(samples);
      check(hot.colorAttachments[0].handle && hot.colorAttachments[1].handle &&
            hot.depthStencil.handle, "surface allocations");
      FlatDrawStateView view{.hot = &hot};
      ClearDesc clear{};
      clear.colorAttachments = hot.colorAttachments;
      clear.depthStencil = hot.depthStencil;
      clear.clearColor = clear.clearDepth = clear.clearStencil = true;
      clear.color = {0.25f, 0.5f, 0.75f, 1.0f};
      clear.depth = 0.375f;
      clear.stencil = 7;
      ChunkSlot suffix;
      suffix.appendClear(clear);
      const RenderPassStoreProofLookaheadSource source{
          .slot = &suffix, .firstCommandIndex = 0, .commandEndIndex = suffix.commandCount()};
      for (bool withClear : {false, true}) {
        const auto loadClear = withClear ? std::optional(clear) : std::nullopt;
        PreparedRenderPass reference{};
        check(prepareRenderPassWithStoreProofLookahead(ctx, view, loadClear, {}, {}, {}, {}, reference),
              "reference preparation");
        for (unsigned flags = 0; flags < 8; ++flags) {
          RenderPassStoreProofActivePass active{
              .hot = &hot, .allowSameAttachmentContinuation = true,
              .lookaheadMayHaveFutureSources = bool(flags & 1),
              .lookaheadInvalid = bool(flags & 2),
              .lookaheadStorageTruncated = bool(flags & 4)};
          PreparedRenderPass candidate{};
          check(prepareRenderPassWithStoreProofLookahead(ctx, view, loadClear,
                    std::span(&source, 1), active, {}, {}, candidate), "candidate preparation");
          equal(reference, candidate);
          check(!candidate.lateStore.hasUnknown(), "query preservation forbids Unknown");
          for (std::size_t i = 0; i < candidate.lateStore.count; ++i) {
            const auto& entry = candidate.lateStore.attachments[i];
            const auto expected = entry.aspect == LateRenderPassStoreAspect::Color &&
                samples != MultiSampleType::None
                    ? WMTStoreActionStoreAndMultisampleResolve : WMTStoreActionStore;
            check(entry.action == expected, "query attachments preserve Store/resolve");
          }
          ++cases;
        }
      }
    }
  }
  std::cout << "query preserved prepared-output cases=" << cases
            << " proof_observer=" << perf::enabled() << '\n';
}
}

int main() {
  try { run(); }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
