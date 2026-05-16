#include "../dxmt9/dxmt9_presenter.hpp"
#include "core_format_utils.hpp"
#include "core_private.hpp"
#include "core_resources_internal.hpp"
#include "dxmt9/assert.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9::core {

// Split from core.cpp; keep this unit private to the D3D9 frontend. Pure
// pixel-format helpers (pitch math, packed-format encoders, BMP screenshot,
// fill/copy helpers) live in core_format_utils.cpp. Per-class resource
// implementations were further split into core_buffer.cpp, core_texture.cpp,
// and core_surface.cpp; this file now hosts the small Query and SwapChain
// classes plus the cross-cutting Device::* clear/query/sequence/swap-chain
// helpers, together with the file-local env/trace helpers shared with the
// per-class units (declared in core_resources_internal.hpp).

namespace {

std::optional<u32> parseEnvU32Auto(const char *name) {
  return dxmt9::util::getenvU32Auto(name);
}

std::string getenvString(const char *name) {
  return dxmt9::util::getenvString(name);
}

bool frameInList(std::span<const u32> frames, u32 frame) {
  return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

bool frameInRange(u32 frame, u32 start, u32 end, u32 interval) {
  return interval != 0 && frame >= start && frame <= end &&
         ((frame - start) % interval) == 0;
}

std::string captureFramePath(const std::string& dir, u32 frame) {
  char name[32];
  std::snprintf(name, sizeof(name), "frame%06u.bmp", frame);
  return (std::filesystem::path(dir) / name).string();
}

std::string captureSkipPath(const std::string& capturePath) {
  return capturePath + ".skipped.json";
}

std::string jsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

bool ensureParentDirectory(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(parent, ec);
  return !ec;
}

} // namespace

namespace detail {

bool backendOwnsSurfaceContents(const SurfaceDesc &desc) {
  return desc.renderTarget || desc.depthStencil ||
         (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0 ||
         desc.multiSampleType != MultiSampleType::None;
}

bool backendOwnsTextureContents(const TextureDesc &desc) {
  return (desc.usage & (UsageRenderTarget | UsageDepthStencil)) != 0;
}

bool canTrustGpuReadback(const std::shared_ptr<dxmt9::Device> &backend) {
  return backend && backend->supportsGpuReadback();
}

bool renderTraceEnabled() {
  static const bool enabled = [] {
    return dxmt9::util::getenvFlag("DXMT_TRACE_RENDER");
  }();
  return enabled;
}

std::optional<u32> textureDumpHandle() {
  static const auto value = parseEnvU32Auto("DXMT_DUMP_TEXTURE_HANDLE");
  return value;
}

std::string textureDumpDir() {
  static const std::string value = [] {
    const auto env = getenvString("DXMT_DUMP_TEXTURE_DIR");
    return env.empty() ? std::string("/tmp") : env;
  }();
  return value;
}

void emitRenderTrace(const char *fmt, ...) {
  if (!renderTraceEnabled()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-render", fmt, args);
  va_end(args);
}

} // namespace detail

Query::Query(QueryType type) : type_(type) {
  if (type_ == QueryType::TimestampFreq) {
    resolvedValue_ = 1000000000ull;
  } else if (type_ == QueryType::TimestampDisjoint) {
    resolvedValue_ = 0ull;
  }
}

/*
 * TLA+: QuerySeqId binding
 *   QueryIds[q]       -> each core::Query instance.
 *   qIssuedSeqId[q]  -> Query::issuedSequenceId_.
 *   currentSeqId     -> Device::submittedSequenceId_ + 1 before assignment;
 *                       the assigned seqId is submittedSequenceId_ after ++.
 *   completedSeqId   -> Device::completedSequenceId_.
 *   qState[q]        -> derived at GetData: unresolved while
 *                       completedSeqId < qIssuedSeqId[q], resolved at the
 *                       S_OK/data path once completedSeqId >= qIssuedSeqId[q].
 *   pendingFlush     -> caller-side Query::GetData(FLUSH) recorder/bridge
 *                       flush before this core query read observes the fence.
 *
 * Action mapping:
 *   IssueQuery       -> Device::issueQuery() assigns the seqId; Query::end()
 *                       stores it as qIssuedSeqId for D3DISSUE_END.
 *   GetDataFlush    -> Query::getData() unresolved FLUSH branch returns
 *                       S_FALSE after the synchronous flush boundary.
 *   GetDataSOK      -> Query::getData() resolved branch guarded by
 *                       completedSeqId >= qIssuedSeqId[q].
 *   GPUComplete     -> Device::completeUpTo() advances completedSeqId.
 */
void Query::begin(u64 sequenceId) {
  active_ = true;
  issuedSequenceId_ = sequenceId;
  resolvedValue_.reset();
}

void Query::end(u64 sequenceId) {
  active_ = false;
  issuedSequenceId_ = sequenceId;
}

void Query::resolve(u64 value) { resolvedValue_ = value; }

HRESULT Query::getData(void *output, size_t size, u32 flags,
                       u64 completedSequenceId) const {
  if (type_ == QueryType::TimestampFreq) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64 *>(output) = 1000000000ull;
    }
    return S_OK;
  }
  if (type_ == QueryType::TimestampDisjoint) {
    if (output && size >= sizeof(u32)) {
      *static_cast<u32 *>(output) = 0u;
    }
    return S_OK;
  }

  if (completedSequenceId < issuedSequenceId_) {
    if ((flags & QUERY_GETDATA_FLUSH) != 0) {
      // TLA+: QuerySeqId / GetDataFlush
      // The FLUSH caller has committed pending query records before this
      // read; the fence is still unresolved, so the poll reports S_FALSE.
      // TLA+: QuerySeqId / NoDeadlockOnFlushSpin
      // A repeated FLUSH spin observes progress through completedSequenceId.
      return S_FALSE;
    }
    return S_FALSE;
  }

  // TLA+: QuerySeqId / GetDataSOK
  // TLA+: QuerySeqId / QueryResolutionSafety
  // A resolved query is only reported once the GPU has completed the chunk
  // that issued it.
  DXMT_ASSERT(completedSequenceId >= issuedSequenceId_);

  if (type_ == QueryType::Event) {
    return S_OK;
  }

  const u64 value = resolvedValue_.value_or(0ull);
  if (type_ == QueryType::Occlusion) {
    if (!output || size == 0) {
      return S_OK;
    }
    if (size >= sizeof(u32)) {
      *static_cast<u32 *>(output) = static_cast<u32>(
          std::min<u64>(value, std::numeric_limits<u32>::max()));
    }
    return S_OK;
  }
  if (type_ == QueryType::Timestamp) {
    if (output && size >= sizeof(u64)) {
      *static_cast<u64 *>(output) = value;
    }
    return S_OK;
  }
  return D3DERR_NOTAVAILABLE;
}

SwapChain::SwapChain(std::shared_ptr<Device> owner, SwapChainHandle handle,
                     PresentParameters params,
                     std::shared_ptr<Surface> backBuffer,
                     std::shared_ptr<Surface> depthStencil)
    : owner_(std::move(owner)), handle_(handle), params_(params),
      backBuffer_(std::move(backBuffer)),
      depthStencilSurface_(std::move(depthStencil)) {
  ensurePresenter();
}

SwapChain::~SwapChain() = default;

void SwapChain::ensurePresenter() {
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }
  const auto &upper = owner->upperDevice();
  if (!upper) {
    return;
  }
  auto wmtDevice = upper->wmtDevice();
  if (!wmtDevice) {
    return;
  }
  const u64 hwnd = params_.deviceWindow.value;
  if (!hwnd) {
    return;
  }
  presenter_ = std::make_unique<dxmt9::Presenter>(wmtDevice, hwnd, 0ull,
                                                  upper->shaderArchive(),
                                                  upper->shaderArchivePath());
  if (!presenter_->valid()) {
    presenter_.reset();
  }
}

bool SwapChain::displaySyncEnabled() const noexcept {
  return params_.presentationInterval != PresentInterval::Immediate;
}

void SwapChain::resize(const PresentParameters &params) {
  params_ = params;
  auto owner = owner_.lock();
  if (!owner) {
    return;
  }

  const u32 width = std::max(1u, params_.backBufferWidth);
  const u32 height = std::max(1u, params_.backBufferHeight);
  backBuffer_ = owner->createSurface({width, height, params_.backBufferFormat,
                                      Pool::Default, UsageRenderTarget, true,
                                      false, params_.multiSampleType});
  if (params_.enableAutoDepthStencil) {
    depthStencilSurface_ = owner->createSurface(
        {width, height, params_.autoDepthStencilFormat, Pool::Default,
         UsageDepthStencil, false, true, params_.multiSampleType});
  } else {
    depthStencilSurface_.reset();
  }
}

HResult SwapChain::present(std::shared_ptr<dxmt9::Device> device,
                           const SwapDesc &desc) {
  if (device) {
    SwapDesc adjusted = desc;
    if (backBuffer_) {
      adjusted.sourceSurface = backBuffer_->handle();
    }
    device->present(adjusted);
  }
  return D3D_OK;
}

std::shared_ptr<Query> Device::createQuery(QueryType type) {
  auto query = std::make_shared<Query>(type);
  queries_.push_back(query);
  return query;
}

std::shared_ptr<SwapChain>
Device::createAdditionalSwapChain(const PresentParameters &params) {
  if (validatePresentParameters(params, extendedDevice_) != D3D_OK) {
    return {};
  }
  const auto normalized = normalizePresentParameters(adapter_, params);
  auto backBuffer = createSurface({std::max(1u, normalized.backBufferWidth),
                                   std::max(1u, normalized.backBufferHeight),
                                   normalized.backBufferFormat, Pool::Default,
                                   UsageRenderTarget, true, false,
                                   normalized.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (normalized.enableAutoDepthStencil) {
    depth = createSurface({std::max(1u, normalized.backBufferWidth),
                           std::max(1u, normalized.backBufferHeight),
                           normalized.autoDepthStencilFormat, Pool::Default,
                           UsageDepthStencil, false, true,
                           normalized.multiSampleType});
  }
  auto swapChain = std::make_shared<SwapChain>(
      shared_from_this(), Handle{nextHandle_++}, normalized, backBuffer, depth);
  swapChains_.push_back(swapChain);
  return swapChain;
}

void Device::initializeDefaultSwapChain() {
  if (!swapChains_.empty()) {
    return;
  }
  const u32 width = std::max(1u, presentParameters_.backBufferWidth);
  const u32 height = std::max(1u, presentParameters_.backBufferHeight);
  auto backBuffer = createSurface(
      {width, height, presentParameters_.backBufferFormat, Pool::Default,
       UsageRenderTarget, true, false, presentParameters_.multiSampleType});
  std::shared_ptr<Surface> depth;
  if (presentParameters_.enableAutoDepthStencil) {
    depth =
        createSurface({width, height, presentParameters_.autoDepthStencilFormat,
                       Pool::Default, UsageDepthStencil, false, true,
                       presentParameters_.multiSampleType});
  }
  swapChains_.push_back(
      std::make_shared<SwapChain>(shared_from_this(), Handle{nextHandle_++},
                                  presentParameters_, backBuffer, depth));
  state_.renderTargets[0] =
      backBuffer ? RenderTargetAttachment{backBuffer->handle(), 0,
                                          backBuffer->multiSampleCount()}
                 : RenderTargetAttachment{};
  state_.depthStencil = depth
                            ? RenderTargetAttachment{depth->handle(), 0,
                                                     depth->multiSampleCount()}
                            : RenderTargetAttachment{};
  invalidateDrawStateCache();
}

std::shared_ptr<SwapChain> Device::swapChain(size_t index) const {
  if (index >= swapChains_.size()) {
    return {};
  }
  return swapChains_[index];
}

ClearDesc Device::snapshotClearDesc(const ClearDesc &desc) const {
  ClearDesc snapshot = desc;
  if (snapshot.clearColor) {
    bool hasExplicitColor = false;
    for (const auto &attachment : snapshot.colorAttachments) {
      if (attachment.handle) {
        hasExplicitColor = true;
        break;
      }
    }
    if (!hasExplicitColor) {
      snapshot.colorAttachments = state_.renderTargets;
    }
  }
  if ((snapshot.clearDepth || snapshot.clearStencil) &&
      !snapshot.depthStencil.handle) {
    snapshot.depthStencil = state_.depthStencil;
  }
  return snapshot;
}

HResult Device::clear(const ClearDesc &desc) {
  auto snapshot = snapshotClearDesc(desc);
  if (snapshot.clearColor) {
    for (const auto &attachment : snapshot.colorAttachments) {
      if (!attachment.handle) {
        continue;
      }
      for (auto &surface : surfaces_) {
        if (auto sp = surface.lock();
            sp && sp->handle() == attachment.handle && sp->valid()) {
          if (detail::canTrustGpuReadback(backend_) &&
              detail::backendOwnsSurfaceContents(sp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            sp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto &rect : snapshot.rects) {
              sp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
      for (auto &texture : textures_) {
        if (auto tp = texture.lock();
            tp && tp->handle() == attachment.handle && tp->valid()) {
          if (detail::canTrustGpuReadback(backend_) &&
              detail::backendOwnsTextureContents(tp->desc())) {
            continue;
          }
          if (snapshot.rects.empty()) {
            tp->fillColor(nullptr, snapshot.color);
          } else {
            for (const auto &rect : snapshot.rects) {
              tp->fillColor(&rect, snapshot.color);
            }
          }
        }
      }
    }
  }

  if (snapshot.clearDepth || snapshot.clearStencil) {
    const auto applyDepthClear = [&](const std::shared_ptr<Surface> &surface) {
      if (!surface || !surface->valid()) {
        return;
      }
      const auto &surfaceDesc = surface->desc();
      if (detail::canTrustGpuReadback(backend_) &&
          detail::backendOwnsSurfaceContents(surfaceDesc)) {
        return;
      }
      if (!surfaceDesc.depthStencil) {
        return;
      }
      if (snapshot.rects.empty()) {
        auto region = surface->lockRect(nullptr, 0);
        if (region.data) {
          std::vector<u8> scratch(
              static_cast<size_t>(region.pitch) * surfaceDesc.height, 0);
          fillDepthStencil(scratch, region.pitch, surfaceDesc.width,
                           surfaceDesc.height, surfaceDesc.format, nullptr,
                           snapshot.clearDepth, snapshot.depth,
                           snapshot.clearStencil, snapshot.stencil);
          std::memcpy(region.data, scratch.data(), scratch.size());
        }
        surface->unlockRect();
      } else {
        for (const auto &rect : snapshot.rects) {
          auto region = surface->lockRect(&rect, 0);
          if (!region.data) {
            continue;
          }
          auto *bytes = static_cast<u8 *>(region.data);
          const u32 rectWidth =
              static_cast<u32>(std::max(0, rect.right - rect.left));
          const u32 rectHeight =
              static_cast<u32>(std::max(0, rect.bottom - rect.top));
          std::vector<u8> scratch(
              static_cast<size_t>(region.pitch) * rectHeight, 0);
          // Fill a temporary region, then copy it into the locked surface area.
          fillDepthStencil(scratch, region.pitch, rectWidth, rectHeight,
                           surfaceDesc.format, nullptr, snapshot.clearDepth,
                           snapshot.depth, snapshot.clearStencil,
                           snapshot.stencil);
          for (u32 y = 0; y < rectHeight; ++y) {
            std::memcpy(bytes + static_cast<size_t>(y) * region.pitch,
                        scratch.data() + static_cast<size_t>(y) * region.pitch,
                        static_cast<size_t>(rectWidth) *
                            bytesPerPixel(surfaceDesc.format));
          }
          surface->unlockRect();
        }
      }
    };

    if (snapshot.depthStencil.handle) {
      for (auto &surface : surfaces_) {
        if (auto sp = surface.lock();
            sp && sp->handle() == snapshot.depthStencil.handle) {
          applyDepthClear(sp);
        }
      }
    }
  }
  submitClearInternal(snapshot);
  return D3D_OK;
}

HResult Device::issueQuery(const std::shared_ptr<Query> &query, bool begin) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  // TLA+: QuerySeqId / IssueQuery
  // The command stream assigns the next seqId, and D3DISSUE_END records that
  // seqId as qIssuedSeqId for the query fence.
  ++submittedSequenceId_;
  // TLA+: QuerySeqId / SeqIdMonotone
  // Queries advance the submission sequence but never allow the completed
  // sequence to move ahead of it.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
  if (begin) {
    query->begin(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      activeOcclusionQuery_ = query;
      activeOcclusionCount_ = 0;
    }
  } else {
    query->end(submittedSequenceId_);
    if (query->type() == QueryType::Occlusion) {
      query->resolve(activeOcclusionCount_);
      activeOcclusionQuery_.reset();
    } else if (query->type() == QueryType::Timestamp) {
      query->resolve(submittedSequenceId_);
    }
  }
  return D3D_OK;
}

HResult Device::getQueryData(const std::shared_ptr<Query> &query, void *output,
                             size_t size, u32 flags) {
  if (!query) {
    return D3DERR_INVALIDCALL;
  }
  if ((flags & QUERY_GETDATA_FLUSH) != 0 && backend_) {
    upperDevice_->flush();
    completeUpTo(submittedSequenceId_);
  }
  // TLA+: QuerySeqId / SeqIdMonotone
  // The completed sequence never exceeds submitted query/draw work.
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
  return query->getData(output, size, flags, completedSequenceId_);
}

void Device::completeUpTo(u64 sequenceId) {
  // TLA+: QuerySeqId / SeqIdMonotone
  // GPUComplete advances completedSeqId monotonically and keeps it bounded by
  // the submitted sequence cursor.
  DXMT_ASSERT(sequenceId >= completedSequenceId_);
  completedSequenceId_ = std::max(completedSequenceId_, sequenceId);
  DXMT_ASSERT(completedSequenceId_ <= submittedSequenceId_);
}

void Device::invalidateDefaultPoolResources() {
  auto invalidateWeak = [](auto &list) {
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](const auto &weak) {
                                if (auto ptr = weak.lock()) {
                                  if (ptr->desc().pool == Pool::Default) {
                                    ptr->invalidate();
                                  }
                                  return false;
                                }
                                return true;
                              }),
               list.end());
  };
  invalidateWeak(buffers_);
  invalidateWeak(textures_);
  invalidateWeak(surfaces_);
}

void Device::submitClearInternal(const ClearDesc &desc) {
  if (detail::renderTraceEnabled()) {
    detail::emitRenderTrace(
        "clear seq=%llu color=%d depth=%d stencil=%d color0=0x%llx "
        "depthStencil=0x%llx rects=%zu rgba=(%.3f,%.3f,%.3f,%.3f) "
        "depthValue=%.3f stencilValue=%u",
        static_cast<unsigned long long>(submittedSequenceId_ + 1),
        desc.clearColor ? 1 : 0, desc.clearDepth ? 1 : 0,
        desc.clearStencil ? 1 : 0,
        static_cast<unsigned long long>(desc.colorAttachments[0].handle.value),
        static_cast<unsigned long long>(desc.depthStencil.handle.value),
        desc.rects.size(), desc.color.r, desc.color.g, desc.color.b,
        desc.color.a, desc.depth, desc.stencil);
  }
  upperDevice_->submitClear(desc);
  ++submittedSequenceId_;
  // SeqIdSafety: a submission can advance the current sequence, but never
  // below the completed sequence.
  DXMT_ASSERT(submittedSequenceId_ >= completedSequenceId_);
}

u32 Device::experimentCaptureRequestedCount() const {
  u32 count = experimentCapture_.frame != 0 ? 1u : 0u;
  count += static_cast<u32>(experimentCapture_.frames.size());
  if (experimentCapture_.rangeInterval != 0 &&
      experimentCapture_.rangeEnd >= experimentCapture_.rangeStart) {
    count += ((experimentCapture_.rangeEnd - experimentCapture_.rangeStart) /
              experimentCapture_.rangeInterval) +
             1u;
  }
  return count;
}

void Device::recordExperimentCaptureSkip(const std::string& capturePath,
                                         const char* reason) {
  ++experimentCapture_.droppedCount;
  const std::string sidecarPath = captureSkipPath(capturePath);
  if (!ensureParentDirectory(sidecarPath)) {
    return;
  }
  std::ofstream stream(sidecarPath, std::ios::out | std::ios::trunc);
  if (!stream) {
    return;
  }
  stream << "{\n"
         << "  \"schema\": \"dxmt9.render_capture.skip.v1\",\n"
         << "  \"frame_id\": " << presentCount_ << ",\n"
         << "  \"present_id\": " << presentCount_ << ",\n"
         << "  \"source\": \"internal_dump\",\n"
         << "  \"reason\": \"" << jsonEscape(reason) << "\",\n"
         << "  \"counters\": {\n"
         << "    \"present_count\": " << presentCount_ << ",\n"
         << "    \"requested_count\": " << experimentCaptureRequestedCount()
         << ",\n"
         << "    \"captured_count\": " << experimentCapture_.capturedCount
         << ",\n"
         << "    \"dropped_count\": " << experimentCapture_.droppedCount
         << "\n"
         << "  }\n"
         << "}\n";
}

void Device::maybeCaptureExperimentFrame() {
  const bool singleRequested =
      experimentCapture_.frame != 0 && !experimentCapture_.path.empty() &&
      !experimentCapture_.captured && presentCount_ >= experimentCapture_.frame;
  const bool listRequested =
      !experimentCapture_.dir.empty() &&
      frameInList(experimentCapture_.frames, presentCount_);
  const bool rangeRequested =
      !experimentCapture_.dir.empty() &&
      frameInRange(presentCount_, experimentCapture_.rangeStart,
                   experimentCapture_.rangeEnd,
                   experimentCapture_.rangeInterval);
  if (!singleRequested && !listRequested && !rangeRequested) {
    return;
  }
  const std::string capturePath =
      singleRequested ? experimentCapture_.path
                      : captureFramePath(experimentCapture_.dir, presentCount_);
  const bool trace = detail::renderTraceEnabled();
  if (trace) {
    detail::emitRenderTrace("capture frame=%u path=%s begin", presentCount_,
                            capturePath.c_str());
  }
  if (!ensureParentDirectory(capturePath)) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: mkdir failed path=%s",
                              presentCount_, capturePath.c_str());
    }
    recordExperimentCaptureSkip(capturePath, "artifact-write-failed");
    return;
  }
  auto chain = swapChain(0);
  if (!chain) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: no swap chain",
                              presentCount_);
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  auto backBuffer = chain->backBuffer();
  if (!backBuffer || !backBuffer->valid()) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: invalid backbuffer",
                              presentCount_);
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  const auto &desc = backBuffer->desc();
  const u32 bpp = bytesPerPixel(desc.format);
  if (bpp == 0) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: unsupported format=%u",
                              presentCount_,
                              static_cast<unsigned>(desc.format));
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  auto scratch =
      createSurface({desc.width, desc.height, desc.format, Pool::Scratch, 0,
                     false, false, MultiSampleType::None});
  if (!scratch) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: scratch alloc failed",
                              presentCount_);
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  const auto readbackHr = getRenderTargetData(backBuffer, scratch);
  if (readbackHr != D3D_OK) {
    if (trace) {
      detail::emitRenderTrace(
          "capture frame=%u aborted: getRenderTargetData hr=0x%08x",
          presentCount_, static_cast<unsigned>(readbackHr));
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  auto region = scratch->lockRect(nullptr, 0);
  if (!region.data) {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: lockRect failed",
                              presentCount_);
    }
    recordExperimentCaptureSkip(capturePath, "backbuffer-unavailable");
    return;
  }
  const size_t byteCount = static_cast<size_t>(region.pitch) * desc.height;
  const bool wrote = writeBmpScreenshot(
      capturePath, desc.format, desc.width, desc.height,
      region.pitch,
      std::span<const u8>(static_cast<const u8 *>(region.data), byteCount));
  scratch->unlockRect();
  if (wrote) {
    if (singleRequested) {
      experimentCapture_.captured = true;
    }
    ++experimentCapture_.capturedCount;
    if (trace) {
      detail::emitRenderTrace("capture frame=%u wrote=%s", presentCount_,
                              capturePath.c_str());
    }
  } else {
    if (trace) {
      detail::emitRenderTrace("capture frame=%u aborted: writeBmp failed",
                              presentCount_);
    }
    recordExperimentCaptureSkip(capturePath, "artifact-write-failed");
  }
}

} // namespace dxmt9::core
