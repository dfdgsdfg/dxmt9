#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_encoder_internal.hpp"
#include "dxmt9_draw_encoder_diagnostics.hpp"
#include "dxmt9_argbuf_hybrid.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_debug_alloc_guard.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_signposts.hpp"
#include "util/log/log.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt9::encoders {

using core::IndexType;

using dxmt9::core::metalqueue::emitQueueTraceLine;

namespace {

std::uint64_t drawGeometryTraceInterval() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT9_TRACE_DRAW_GEOMETRY");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end != env && parsed > 1ull) {
      return parsed;
    }
    return 1ull;
  }();
  return value;
}

std::uint64_t drawGeometryTraceLimit() {
  static const std::uint64_t value = [] {
    const char* env = std::getenv("DXMT9_TRACE_DRAW_GEOMETRY_LIMIT");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0ull;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    return end != env ? parsed : 0ull;
  }();
  return value;
}

std::optional<std::uint64_t> nextDrawGeometryTraceSample() {
  const auto interval = drawGeometryTraceInterval();
  if (interval == 0ull) {
    return std::nullopt;
  }
  static std::atomic<std::uint64_t> drawCounter{0};
  static std::atomic<std::uint64_t> emittedCounter{0};
  const auto drawNo = drawCounter.fetch_add(1, std::memory_order_relaxed) + 1ull;
  if (interval > 1ull && (drawNo % interval) != 0ull) {
    return std::nullopt;
  }
  const auto limit = drawGeometryTraceLimit();
  if (limit != 0ull &&
      emittedCounter.fetch_add(1, std::memory_order_relaxed) >= limit) {
    return std::nullopt;
  }
  return drawNo;
}

const char* indexTypeName(IndexType type) {
  return type == IndexType::UInt32 ? "u32" : "u16";
}

const char* drawGeometrySourceName(bool direct, bool up, bool expanded) {
  if (expanded) {
    return "expanded";
  }
  if (up) {
    return "up";
  }
  return direct ? "direct" : "unknown";
}

const char* metalDrawMethodName(bool indexed, bool expanded) {
  if (indexed && !expanded) {
    return "drawIndexedPrimitives";
  }
  return "drawPrimitives";
}

void appendVertexDeclSummary(std::ostringstream& out,
                             const core::VertexDeclSnapshot& vertexDecl) {
  out << " elems=" << vertexDecl.elements.size()
      << " decl=[";
  for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
    if (i) {
      out << ';';
    }
    const auto& e = vertexDecl.elements[i];
    out << "{s=" << e.stream
        << ",off=" << e.offset
        << ",type=" << e.type
        << ",method=" << e.method
        << ",usage=" << e.usage
        << ",idx=" << e.usageIndex
        << "}";
  }
  out << "]";
}

}  // namespace

void recordDrawGeometryDiagnostics(core::FlatDrawStateView drawState,
                                   const ParamView& pv,
                                   u64 seqId,
                                   u64 vertexCount,
                                   u64 vertexBufferOffset,
                                   u32 vertexStreamOffset,
                                   u32 vertexStreamStride,
                                   bool indexed,
                                   bool direct,
                                   bool up,
                                   bool expanded,
                                   bool fixedFunctionPath) {
  const auto& hot = *drawState.hot;
  const auto& vertexDecl = drawState.shaderContext().vertexDecl;
  perf::countDrawGeometryDiagnostics(fixedFunctionPath,
                                     indexed,
                                     pv.indexType == IndexType::UInt32,
                                     direct,
                                     up,
                                     expanded,
                                     pv.baseVertexIndex != 0,
                                     pv.startIndex != 0u,
                                     hot.streamOffsets[0] != 0u,
                                     hot.streamStrides[0],
                                     hot.key.vertexDeclHash);

  const auto sample = nextDrawGeometryTraceSample();
  if (!sample) {
    return;
  }

  std::ostringstream out;
  out << "[dxmt9-geometry] sample=" << static_cast<unsigned long long>(*sample)
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " api="
      << (indexed ? (up ? "DrawIndexedPrimitiveUP" : "DrawIndexedPrimitive")
                  : (up ? "DrawPrimitiveUP" : "DrawPrimitive"))
      << " metal=" << metalDrawMethodName(indexed, expanded)
      << " source=" << drawGeometrySourceName(direct, up, expanded)
      << " shaderPath=" << (fixedFunctionPath ? "ffp" : "vs")
      << " indexed=" << (indexed ? 1 : 0)
      << " baseVertex=" << pv.baseVertexIndex
      << " startVertex=" << pv.startVertex
      << " startIndex=" << pv.startIndex
      << " indexType=" << indexTypeName(pv.indexType)
      << " minVertex=na numVertices=na"
      << " primType=" << static_cast<unsigned>(pv.primitiveType)
      << " primCount=" << pv.primitiveCount
      << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
      << " stream0Handle=0x" << std::hex
      << static_cast<unsigned long long>(hot.streamBuffers[0].value)
      << " stream0Offset=" << std::dec << hot.streamOffsets[0]
      << " stream0Stride=" << hot.streamStrides[0]
      << " vertexBufferOffset=" << static_cast<unsigned long long>(vertexBufferOffset)
      << " uniformStreamOffset=" << vertexStreamOffset
      << " uniformStreamStride=" << vertexStreamStride
      << " declHash=0x" << std::hex << hot.key.vertexDeclHash
      << " fvf=0x" << vertexDecl.fvf << std::dec
      << " vsHash=0x" << std::hex
      << static_cast<unsigned long long>(drawState.shaderContext().vertexShader.hash)
      << " psHash=0x"
      << static_cast<unsigned long long>(drawState.shaderContext().pixelShader.hash)
      << std::dec
      << " userVertexBytes=" << pv.userVertexData.size()
      << " userIndexBytes=" << pv.userIndexData.size();
  appendVertexDeclSummary(out, vertexDecl);
  emitQueueTraceLine(out.str());
}


using core::ClearDesc;
using core::Handle;
using core::IndexType;
using core::SamplerSnapshot;
using core::SAMP_ADDRESS_U;
using core::SAMP_ADDRESS_V;
using core::SAMP_ADDRESS_W;
using core::SAMP_BORDER_COLOR;
using core::SAMP_MAG_FILTER;
using core::SAMP_MAX_ANISOTROPY;
using core::SAMP_MAX_MIP_LEVEL;
using core::SAMP_MIN_FILTER;
using core::SAMP_MIP_FILTER;
using core::kMaxSamplers;
using core::kMaxTextureStages;

using core::CompareFunc;
using core::TextureOp;

using core::RS_ALPHABLEND_ENABLE;
using core::RS_ALPHA_FUNC;
using core::RS_ALPHA_REF;
using core::RS_ALPHA_TEST_ENABLE;
using core::RS_BLEND_OP;
using core::RS_BLEND_OP_ALPHA;
using core::RS_BLEND_FACTOR;
using core::RS_COLOR_WRITE_ENABLE;
using core::RS_CULL_MODE;
using core::RS_DEST_BLEND;
using core::RS_DEST_BLEND_ALPHA;
using core::RS_POINT_SCALE_ENABLE;
using core::RS_POINT_SPRITE_ENABLE;
using core::RS_POINTSIZE;
using core::RS_POINTSIZE_MAX;
using core::RS_POINTSIZE_MIN;
using core::RS_SEPARATE_ALPHA_BLEND_ENABLE;
using core::RS_SRC_BLEND;
using core::RS_SRC_BLEND_ALPHA;
using core::RS_TEXTURE_FACTOR;
using core::RS_Z_ENABLE;
using core::RS_Z_FUNC;
using core::RS_Z_WRITE_ENABLE;

using core::TSS_ALPHA_ARG1;
using core::TSS_ALPHA_ARG2;
using core::TSS_ALPHA_OP;
using core::TSS_COLOR_ARG1;
using core::TSS_COLOR_ARG2;
using core::TSS_COLOR_OP;
using core::TSS_TEXCOORD_INDEX;
using core::TSS_TEXTURE_TRANSFORM_FLAGS;

using dxmt9::ffp::kD3DDeclTypeD3DColor;
using dxmt9::ffp::kD3DDeclTypeFloat1;
using dxmt9::ffp::kD3DDeclTypeFloat2;
using dxmt9::ffp::kD3DDeclTypeFloat3;
using dxmt9::ffp::kD3DDeclTypeFloat4;
using dxmt9::ffp::kD3DDeclUsageColor;
using dxmt9::ffp::kD3DDeclUsagePosition;
using dxmt9::ffp::kD3DDeclUsagePositionT;
using dxmt9::ffp::kD3DDeclUsageTexcoord;

using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::convert::toPixelFormat;
using dxmt9::convert::toCullMode;
using dxmt9::convert::toIndexType;
using dxmt9::convert::toPrimitiveType;
using dxmt9::ffp::computeVertexDeclStreamStride;
using dxmt9::ffp::computeVertexDeclStride;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

using dxmt9::core::metalqueue::emitQueueTraceLine;
using dxmt9::core::metalqueue::emitTextureTraceLine;
using dxmt9::core::metalqueue::queueTraceEnabled;

using dxmt9::state::DrawVolatile;
using dxmt9::state::FfpPsConsts;
using dxmt9::state::FfpVsConsts;
using dxmt9::state::PsConsts;
using dxmt9::state::SamplerLodBias;
using dxmt9::state::VsConsts;
using dxmt9::state::anySamplerLodBiasNonzero;
using dxmt9::state::buildDrawVolatile;
using dxmt9::state::buildFfpPsConsts;
using dxmt9::state::buildSamplerLodBias;
using dxmt9::state::buildFfpVsConsts;
using dxmt9::state::buildPsConsts;
using dxmt9::state::buildVsConsts;
using dxmt9::state::makeDepthStencilKey;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;

namespace {

std::optional<u64> parseEnvU64Auto(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const u64 value = static_cast<u64>(std::strtoull(env, &end, 0));
  if (end == env || (end && *end != '\0')) {
    return std::nullopt;
  }
  return value;
}

std::string getenvStringLocal(const char* name) {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' ? std::string(env) : std::string{};
}

bool getenvFlagLocal(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return false;
  }
  return std::strcmp(env, "0") != 0 &&
         std::strcmp(env, "false") != 0 &&
         std::strcmp(env, "FALSE") != 0 &&
         std::strcmp(env, "off") != 0 &&
         std::strcmp(env, "OFF") != 0;
}

std::vector<u64> parseEnvU64ListAuto(const char* name) {
  std::vector<u64> values;
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return values;
  }
  const char* cursor = env;
  while (*cursor != '\0') {
    while (*cursor == ',' || *cursor == ';' || *cursor == ':' ||
           std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    char* end = nullptr;
    const u64 value = static_cast<u64>(std::strtoull(cursor, &end, 0));
    if (end == cursor) {
      break;
    }
    values.push_back(value);
    cursor = end;
  }
  return values;
}

bool traceEncodeProgressEnabled() {
  static const bool enabled = getenvFlagLocal("DXMT9_TRACE_ENCODE_PROGRESS");
  return enabled;
}

std::optional<u64> traceEncodeProgressSeqFilter() {
  static const std::optional<u64> filter =
      parseEnvU64Auto("DXMT9_TRACE_ENCODE_PROGRESS_SEQ");
  return filter;
}

}  // namespace

bool traceEncodeProgressForSeq(u64 seqId) {
  if (!traceEncodeProgressEnabled()) {
    return false;
  }
  const auto filter = traceEncodeProgressSeqFilter();
  return !filter.has_value() || *filter == seqId;
}

const char* metalCommandKindName(core::MetalCommandKind kind) {
  switch (kind) {
  case core::MetalCommandKind::DrawRun:
    return "DrawRun";
  case core::MetalCommandKind::Clear:
    return "Clear";
  case core::MetalCommandKind::SurfaceCopy:
    return "SurfaceCopy";
  case core::MetalCommandKind::StretchRect:
    return "StretchRect";
  case core::MetalCommandKind::Readback:
    return "Readback";
  case core::MetalCommandKind::ColorFill:
    return "ColorFill";
  case core::MetalCommandKind::DepthResolve:
    return "DepthResolve";
  case core::MetalCommandKind::Present:
    return "Present";
  }
  return "Unknown";
}

void emitEncodeProgressDrawStage(u64 seqId,
                                 std::uint32_t commandIndex,
                                 u64 commandDrawIndex,
                                 u64 commandDrawCount,
                                 const char* stage) {
  if (!traceEncodeProgressForSeq(seqId)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encode-progress]"
      << " stage=draw." << stage
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " command=" << commandIndex
      << " draw=" << static_cast<unsigned long long>(commandDrawIndex)
      << "/" << static_cast<unsigned long long>(commandDrawCount);
  emitQueueTraceLine(out.str());
}

namespace {

struct VisibilityScoutConfig {
  bool enabled = false;
  std::string path;
  u64 maxResultsPerEncoder = 65536;
  debug::RenderEncoderSelector row{};
  debug::RenderEncoderSelectorList rows{};
};

const VisibilityScoutConfig& visibilityScoutConfig() {
  static const VisibilityScoutConfig config = [] {
    VisibilityScoutConfig result{};
    result.path = getenvStringLocal("DXMT9_VISIBILITY_SCOUT_PATH");
    result.maxResultsPerEncoder =
        parseEnvU64Auto("DXMT9_VISIBILITY_SCOUT_MAX_RESULTS_PER_ENCODER")
            .value_or(result.maxResultsPerEncoder);
    result.maxResultsPerEncoder = std::max<u64>(1, result.maxResultsPerEncoder);
    result.row =
        debug::makeRenderEncoderSelector(
            getenvStringLocal("DXMT9_VISIBILITY_SCOUT_ROW"));
    result.rows =
        debug::makeRenderEncoderSelectorList(
            getenvStringLocal("DXMT9_VISIBILITY_SCOUT_ROWS"));
    result.enabled =
        !result.path.empty() &&
        (getenvFlagLocal("DXMT9_VISIBILITY_SCOUT") ||
         result.row.enabled ||
         result.rows.enabled);
    return result;
  }();
  return config;
}

bool visibilityScoutWantsPass(u64 seqId, u64 encoderIndex) {
  const auto& config = visibilityScoutConfig();
  if (!config.enabled) {
    return false;
  }
  if (config.row.enabled &&
      !debug::renderEncoderSelectorMatches(config.row, seqId, encoderIndex)) {
    return false;
  }
  if (config.rows.enabled &&
      !debug::renderEncoderSelectorListMatches(config.rows, seqId, encoderIndex)) {
    return false;
  }
  return true;
}

}  // namespace

std::optional<VisibilityScoutPass> makeVisibilityScoutPass(
    WMT::Device device,
    u64 seqId,
    u64 encoderIndex) {
  if (!device || !visibilityScoutWantsPass(seqId, encoderIndex)) {
    return std::nullopt;
  }
  const auto& config = visibilityScoutConfig();
  const auto capacity = static_cast<std::uint32_t>(
      std::min<u64>(config.maxResultsPerEncoder,
                    std::numeric_limits<std::uint32_t>::max()));
  WMTBufferInfo info{};
  info.length = static_cast<u64>(capacity) * sizeof(std::uint64_t);
  info.options = WMTResourceStorageModeShared;
  auto buffer = device.newBuffer(info);
  if (!buffer || !info.memory.ptr) {
    return std::nullopt;
  }
  std::memset(info.memory.ptr, 0, static_cast<std::size_t>(info.length));

  VisibilityScoutPass pass{};
  pass.buffer = std::move(buffer);
  pass.results = static_cast<std::uint64_t*>(info.memory.ptr);
  pass.path = config.path;
  pass.seqId = seqId;
  pass.encoderIndex = encoderIndex;
  pass.capacity = capacity;
  pass.records.reserve(capacity);
  return pass;
}

VisibilityScoutDrawRecord makeVisibilityScoutDrawRecord(
    const VisibilityScoutPass& pass,
    core::FlatDrawStateView drawState,
    const core::ViewportScissor& viewport,
    WMTPrimitiveType primitiveType,
    const ParamView& pv,
    u64 drawOrdinal,
    std::uint32_t commandIndex,
    u64 submittedPrimitiveCount,
    u64 submittedElementCount,
    bool indexed,
    bool expandedIndexed,
    std::uint32_t splitChunk,
    WMTCullMode cullMode,
    WMTTriangleFillMode fillMode) {
  const auto& hot = *drawState.hot;
  VisibilityScoutDrawRecord record{};
  record.seqId = pass.seqId;
  record.encoderIndex = pass.encoderIndex;
  record.commandIndex = commandIndex;
  record.drawOrdinal = drawOrdinal;
  record.metalDrawIndex = pass.metalDrawIndex;
  record.primitiveType = static_cast<std::uint32_t>(primitiveType);
  record.sourcePrimitiveCount = pv.primitiveCount;
  record.submittedPrimitiveCount = submittedPrimitiveCount;
  record.submittedElementCount = submittedElementCount;
  record.indexed = indexed ? 1u : 0u;
  record.expandedIndexed = expandedIndexed ? 1u : 0u;
  record.splitChunk = splitChunk;
  record.rt0 = hot.colorAttachments[0].handle.value;
  record.depth = hot.depthStencil.handle.value;
  record.textureMask = hot.textureMask;
  record.colorWrite =
      core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
  record.zEnable = core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u);
  record.zWrite = core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u);
  record.zFunc =
      core::flatStateOr(hot.renderStates, RS_Z_FUNC,
                        static_cast<u32>(core::CompareFunc::LessEqual));
  record.alphaBlend =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u);
  record.alphaTest =
      core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u);
  record.scissor = viewport.scissorEnabled ? 1u : 0u;
  record.cull = static_cast<std::uint32_t>(cullMode);
  record.fill = static_cast<std::uint32_t>(fillMode);
  return record;
}

std::optional<std::uint32_t> beginVisibilityScoutDraw(
    VisibilityScoutPass* pass,
    WMT::RenderCommandEncoder& encoder,
    VisibilityScoutDrawRecord record) {
  if (!pass) {
    return std::nullopt;
  }
  if (pass->records.size() >= pass->capacity) {
    pass->overflow = true;
    return std::nullopt;
  }
  record.resultIndex = static_cast<std::uint32_t>(pass->records.size());
  pass->records.push_back(record);
  ++pass->metalDrawIndex;
  encoder.setVisibilityResultMode(WMTVisibilityResultModeCounting,
                                  static_cast<u64>(record.resultIndex) *
                                      sizeof(std::uint64_t));
  return record.resultIndex;
}

void endVisibilityScoutDraw(VisibilityScoutPass* pass,
                            WMT::RenderCommandEncoder& encoder,
                            std::optional<std::uint32_t> resultIndex) {
  if (!pass || !resultIndex.has_value()) {
    return;
  }
  encoder.setVisibilityResultMode(WMTVisibilityResultModeDisabled, 0);
}

void appendVisibilityScoutCsv(const std::string& path,
                              WMT::Reference<WMT::Buffer> buffer,
                              const std::uint64_t* results,
                              std::vector<VisibilityScoutDrawRecord> records,
                              bool overflow) {
  (void)buffer;
  if (path.empty() || results == nullptr || records.empty()) {
    return;
  }
  std::filesystem::path outputPath(path);
  if (const auto parent = outputPath.parent_path(); !parent.empty()) {
    std::error_code createError;
    std::filesystem::create_directories(parent, createError);
  }
  static std::mutex outputMutex;
  std::lock_guard lock(outputMutex);
  std::error_code sizeError;
  const bool needsHeader =
      !std::filesystem::exists(outputPath, sizeError) ||
      std::filesystem::file_size(outputPath, sizeError) == 0;
  std::ofstream out(path, std::ios::app);
  if (!out) {
    return;
  }
  if (needsHeader) {
    out << "seq,encoder,command,draw_ordinal,result_index,metal_draw_index,"
           "primitive_type,source_primitive_count,submitted_primitive_count,"
           "submitted_element_count,indexed,expanded_indexed,split_chunk,"
           "visible_samples,rt0,depth,texture_mask,color_write,z_enable,"
           "z_write,z_func,alpha_blend,alpha_test,scissor,cull,fill,overflow\n";
  }
  for (const auto& record : records) {
    const auto visibleSamples = results[record.resultIndex];
    out << record.seqId << ','
        << record.encoderIndex << ','
        << record.commandIndex << ','
        << record.drawOrdinal << ','
        << record.resultIndex << ','
        << record.metalDrawIndex << ','
        << record.primitiveType << ','
        << record.sourcePrimitiveCount << ','
        << record.submittedPrimitiveCount << ','
        << record.submittedElementCount << ','
        << record.indexed << ','
        << record.expandedIndexed << ','
        << record.splitChunk << ','
        << visibleSamples << ','
        << record.rt0 << ','
        << record.depth << ','
        << record.textureMask << ','
        << record.colorWrite << ','
        << record.zEnable << ','
        << record.zWrite << ','
        << record.zFunc << ','
        << record.alphaBlend << ','
        << record.alphaTest << ','
        << record.scissor << ','
        << record.cull << ','
        << record.fill << ','
        << (overflow ? 1u : 0u) << '\n';
  }
}

void enqueueVisibilityScoutCompletion(
    VisibilityScoutPass& pass,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!pass.results || pass.records.empty()) {
    return;
  }
  completionCallbacks.push_back(
      [path = std::move(pass.path),
       buffer = std::move(pass.buffer),
       results = pass.results,
       records = std::move(pass.records),
       overflow = pass.overflow]() mutable {
        appendVisibilityScoutCsv(path, std::move(buffer), results,
                                 std::move(records), overflow);
      });
  pass.results = nullptr;
}

namespace {

struct DepthAttachmentDumpConfig {
  bool enabled = false;
  u64 handle = 0;
  std::optional<u64> seq;
  std::optional<u64> enc;
  std::string path;
};

struct ColorAttachmentDumpConfig {
  bool enabled = false;
  bool afterDraw = false;
  std::optional<u64> handle;
  std::optional<u64> index;
  std::optional<u64> seq;
  std::optional<u64> enc;
  std::optional<u64> draw;
  std::vector<u64> draws;
  std::optional<u64> commandIndex;
  std::optional<u64> commandIndexMin;
  std::optional<u64> commandIndexMax;
  std::optional<u64> texture0;
  std::vector<u64> texture0s;
  std::string path;
  std::string dir;
  std::string roiSummaryPath;
  u64 brightThreshold = 220;
  u64 whiteThreshold = 240;
  u64 warmRedThreshold = 180;
  u64 warmGreenThreshold = 110;
  u64 warmBlueMargin = 32;
  struct Roi {
    u32 left = 0;
    u32 top = 0;
    u32 right = 0;
    u32 bottom = 0;
    std::string name;
  };
  std::vector<Roi> rois;
};

std::vector<ColorAttachmentDumpConfig::Roi> parseColorAttachmentRois(
    const char* name) {
  std::vector<ColorAttachmentDumpConfig::Roi> rois;
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return rois;
  }

  const char* cursor = env;
  while (*cursor != '\0') {
    while (*cursor == ';' || std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }

    char* end = nullptr;
    const auto left = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto top = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto right = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto bottom = std::strtoull(cursor, &end, 10);
    if (end == cursor || right <= left || bottom <= top ||
        right > std::numeric_limits<u32>::max() ||
        bottom > std::numeric_limits<u32>::max()) {
      break;
    }
    cursor = end;

    std::string roiName;
    if (*cursor == ':') {
      ++cursor;
      const char* nameStart = cursor;
      while (*cursor != '\0' && *cursor != ';') {
        ++cursor;
      }
      roiName.assign(nameStart, cursor);
    }
    if (roiName.empty()) {
      std::ostringstream generated;
      generated << left << "," << top << "," << right << "," << bottom;
      roiName = generated.str();
    }

    ColorAttachmentDumpConfig::Roi roi{};
    roi.left = static_cast<u32>(left);
    roi.top = static_cast<u32>(top);
    roi.right = static_cast<u32>(right);
    roi.bottom = static_cast<u32>(bottom);
    roi.name = std::move(roiName);
    rois.push_back(std::move(roi));

    while (*cursor != '\0' && *cursor != ';') {
      ++cursor;
    }
  }
  return rois;
}

const DepthAttachmentDumpConfig& depthAttachmentDumpConfig() {
  static const DepthAttachmentDumpConfig config = [] {
    DepthAttachmentDumpConfig result{};
    result.handle =
        parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE").value_or(0);
    result.seq = parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_ENC");
    result.path = getenvStringLocal("DXMT9_DUMP_DEPTH_ATTACHMENT_PATH");
    result.enabled = result.handle != 0 && !result.path.empty();
    return result;
  }();
  return config;
}

const ColorAttachmentDumpConfig& colorAttachmentDumpConfig() {
  static const ColorAttachmentDumpConfig config = [] {
    ColorAttachmentDumpConfig result{};
    result.afterDraw =
        getenvFlagLocal("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW");
    result.handle = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_HANDLE");
    result.index = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX");
    result.seq = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_ENC");
    result.draw = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_DRAW");
    result.draws = parseEnvU64ListAuto("DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS");
    result.commandIndex =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX");
    result.commandIndexMin =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN");
    result.commandIndexMax =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX");
    result.texture0 = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0");
    result.texture0s =
        parseEnvU64ListAuto("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S");
    result.path = getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_PATH");
    result.dir = getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_DIR");
    result.roiSummaryPath =
        getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH");
    result.rois = parseColorAttachmentRois("DXMT9_DUMP_COLOR_ATTACHMENT_ROIS");
    result.brightThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD")
            .value_or(220ull);
    result.whiteThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD")
            .value_or(240ull);
    result.warmRedThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD")
            .value_or(180ull);
    result.warmGreenThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD")
            .value_or(110ull);
    result.warmBlueMargin =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN")
            .value_or(32ull);
    result.enabled = (!result.path.empty() ||
                      !result.dir.empty() ||
                      !result.roiSummaryPath.empty()) &&
                     (result.handle.has_value() ||
                      result.index.has_value() ||
                      result.seq.has_value() ||
                      result.enc.has_value() ||
                      result.draw.has_value() ||
                      !result.draws.empty() ||
                      result.commandIndex.has_value() ||
                      result.commandIndexMin.has_value() ||
                      result.commandIndexMax.has_value() ||
                      result.texture0.has_value() ||
                      !result.texture0s.empty());
    return result;
  }();
  return config;
}

struct DrawTextureDumpConfig {
  bool enabled = false;
  bool texture0Any = false;
  std::vector<u64> handles;
  std::optional<u64> texture0Width;
  std::optional<u64> texture0Height;
  std::optional<u64> texture0Format;
  std::optional<u64> seq;
  std::optional<u64> seqMin;
  std::optional<u64> enc;
  std::string dir;
};

const DrawTextureDumpConfig& drawTextureDumpConfig() {
  static const DrawTextureDumpConfig config = [] {
    DrawTextureDumpConfig result{};
    result.handles = parseEnvU64ListAuto("DXMT9_DUMP_DRAW_TEXTURE_HANDLES");
    if (const auto handle = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_HANDLE")) {
      result.handles.push_back(*handle);
    }
    result.texture0Any = getenvFlagLocal("DXMT9_DUMP_DRAW_TEXTURE0_ANY");
    result.texture0Width = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_WIDTH");
    result.texture0Height = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_HEIGHT");
    result.texture0Format = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_FORMAT");
    result.seq = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_SEQ");
    result.seqMin = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_SEQ_MIN");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_ENC");
    result.dir = getenvStringLocal("DXMT9_DUMP_DRAW_TEXTURE_DIR");
    result.enabled = (!result.handles.empty() ||
                      result.texture0Any ||
                      result.texture0Width.has_value() ||
                      result.texture0Height.has_value() ||
                      result.texture0Format.has_value()) &&
                     !result.dir.empty();
    return result;
  }();
  return config;
}

}  // namespace

std::size_t drawTextureDumpReserveCapacity() {
  return drawTextureDumpConfig().handles.size();
}

bool parallelRenderPassSidecarObservationEnabled() {
  return depthAttachmentDumpConfig().enabled ||
      colorAttachmentDumpConfig().enabled ||
      drawTextureDumpConfig().enabled ||
      visibilityScoutConfig().enabled;
}

void selectActiveDepthAttachmentDump(
    const resources::Pool& pool,
    const core::BackendLimits& limits,
    core::FlatDrawStateView drawState,
    bool renderEncoderActive,
    u64 seqId,
    u64 encoderIndex,
    ActiveDepthAttachmentDump& activeDump) {
  auto* depthSurface = pool.findSurface(drawState.hot->depthStencil.handle.value);
  if (!renderEncoderActive || !depthSurface || !depthSurface->texture ||
      !depthSurface->desc.depthStencil) {
    return;
  }
  activeDump.handle = drawState.hot->depthStencil.handle;
  activeDump.texture = depthSurface->texture;
  activeDump.format = depthSurface->desc.format;
  activeDump.metalPixelFormat = toPixelFormat(depthSurface->desc.format, limits);
  activeDump.width = std::max(1u, depthSurface->desc.width);
  activeDump.height = std::max(1u, depthSurface->desc.height);
  activeDump.seq = seqId;
  activeDump.enc = encoderIndex;
  activeDump.hasDepth = formatHasDepthAspect(depthSurface->desc.format);
  activeDump.hasStencil = formatHasStencilAspect(depthSurface->desc.format);
}

void selectActiveColorAttachmentDump(
    const resources::Pool& pool,
    const core::BackendLimits& limits,
    core::FlatDrawStateView drawState,
    bool renderEncoderActive,
    u64 seqId,
    u64 encoderIndex,
    ActiveColorAttachmentDump& activeDump) {
  if (!renderEncoderActive || !colorAttachmentDumpConfig().enabled) {
    return;
  }
  const auto& colorDumpConfig = colorAttachmentDumpConfig();
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    const auto colorHandle = drawState.hot->colorAttachments[i].handle;
    if (!colorHandle) {
      continue;
    }
    if (colorDumpConfig.index.has_value() &&
        *colorDumpConfig.index != i) {
      continue;
    }
    if (colorDumpConfig.handle.has_value() &&
        *colorDumpConfig.handle != colorHandle.value) {
      continue;
    }
    auto* colorSurface = pool.findSurface(colorHandle.value);
    if (!colorSurface || !colorSurface->texture ||
        !colorSurface->desc.renderTarget) {
      continue;
    }
    activeDump.handle = colorHandle;
    activeDump.texture = colorSurface->texture;
    activeDump.format = colorSurface->desc.format;
    activeDump.metalPixelFormat =
        toPixelFormat(colorSurface->desc.format, limits);
    activeDump.width = std::max(1u, colorSurface->desc.width);
    activeDump.height = std::max(1u, colorSurface->desc.height);
    activeDump.index = static_cast<u32>(i);
    activeDump.seq = seqId;
    activeDump.enc = encoderIndex;
    break;
  }
}

bool drawTextureDumpWantsTexture(u32 textureIndex,
                                 core::Handle handle,
                                 const resources::TextureRecord& record) {
  const auto& config = drawTextureDumpConfig();
  if (!config.enabled || !handle) {
    return false;
  }
  if (!config.handles.empty() &&
      std::find(config.handles.begin(), config.handles.end(),
                handle.value) == config.handles.end()) {
    return false;
  }
  if (!config.texture0Any &&
      !config.texture0Width.has_value() &&
      !config.texture0Height.has_value() &&
      !config.texture0Format.has_value()) {
    return true;
  }
  if (textureIndex != 0u) {
    return false;
  }
  if (config.texture0Width.has_value() &&
      record.desc.width != *config.texture0Width) {
    return false;
  }
  if (config.texture0Height.has_value() &&
      record.desc.height != *config.texture0Height) {
    return false;
  }
  if (config.texture0Format.has_value() &&
      static_cast<u64>(record.desc.format) != *config.texture0Format) {
    return false;
  }
  return true;
}

ColorAttachmentReadbackRegion colorAttachmentDumpReadbackRegion(
    const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.path.empty() || !config.dir.empty() || config.rois.empty()) {
    return ColorAttachmentReadbackRegion{0, 0, active.width, active.height};
  }

  u32 left = active.width;
  u32 top = active.height;
  u32 right = 0;
  u32 bottom = 0;
  for (const auto& roi : config.rois) {
    const u32 clippedLeft = std::min(roi.left, active.width);
    const u32 clippedTop = std::min(roi.top, active.height);
    const u32 clippedRight = std::min(roi.right, active.width);
    const u32 clippedBottom = std::min(roi.bottom, active.height);
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
      continue;
    }
    left = std::min(left, clippedLeft);
    top = std::min(top, clippedTop);
    right = std::max(right, clippedRight);
    bottom = std::max(bottom, clippedBottom);
  }
  if (right <= left || bottom <= top) {
    return ColorAttachmentReadbackRegion{0, 0, 0, 0};
  }
  return ColorAttachmentReadbackRegion{left, top, right - left, bottom - top};
}

bool depthAttachmentDumpMatches(const ActiveDepthAttachmentDump& active) {
  const auto& config = depthAttachmentDumpConfig();
  if (!config.enabled || !active.handle || !active.texture) {
    return false;
  }
  if (config.handle != active.handle.value) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  static std::atomic_bool started{false};
  bool expected = false;
  return started.compare_exchange_strong(expected, true);
}

bool colorAttachmentDumpMatches(const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.enabled || !active.handle || !active.texture) {
    return false;
  }
  if (config.afterDraw != active.afterDraw) {
    return false;
  }
  if (config.handle.has_value() && *config.handle != active.handle.value) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  if (config.draw.has_value() && *config.draw != active.draw) {
    return false;
  }
  if (!config.draws.empty() &&
      std::find(config.draws.begin(), config.draws.end(), active.draw) ==
          config.draws.end()) {
    return false;
  }
  if (config.commandIndex.has_value() &&
      *config.commandIndex != active.commandIndex) {
    return false;
  }
  if (config.commandIndexMin.has_value() &&
      active.commandIndex < *config.commandIndexMin) {
    return false;
  }
  if (config.commandIndexMax.has_value() &&
      active.commandIndex > *config.commandIndexMax) {
    return false;
  }
  if (config.texture0.has_value() && *config.texture0 != active.texture0) {
    return false;
  }
  if (!config.texture0s.empty() &&
      std::find(config.texture0s.begin(), config.texture0s.end(),
                active.texture0) == config.texture0s.end()) {
    return false;
  }
  if (config.dir.empty()) {
    if (!config.roiSummaryPath.empty()) {
      std::ostringstream key;
      key << (active.afterDraw ? "after" : "pass")
          << ":s" << active.seq
          << ":e" << active.enc
          << ":i" << active.index
          << ":d" << active.draw
          << ":c" << active.commandIndex
          << ":ci" << active.commandDrawIndex
          << ":t" << active.texture0;
      static std::mutex mutex;
      static std::unordered_set<std::string> dumped;
      std::lock_guard lock(mutex);
      return dumped.insert(key.str()).second;
    }
    static std::atomic_bool started{false};
    bool expected = false;
    return started.compare_exchange_strong(expected, true);
  }
  std::ostringstream key;
  key << (active.afterDraw ? "after" : "pass")
      << ":s" << active.seq
      << ":e" << active.enc
      << ":i" << active.index
      << ":d" << active.draw
      << ":c" << active.commandIndex
      << ":ci" << active.commandDrawIndex
      << ":t" << active.texture0;
  static std::mutex mutex;
  static std::unordered_set<std::string> dumped;
  std::lock_guard lock(mutex);
  return dumped.insert(key.str()).second;
}

bool colorAttachmentDumpAfterDrawWantsSplit(
    const ActiveColorAttachmentDump& active,
    core::FlatDrawStateView drawState,
    u64 encoderDrawIndex,
    u64 commandIndex) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.enabled || !config.afterDraw || !active.handle || !active.texture ||
      !drawState.hot) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  if (config.draw.has_value() && *config.draw != encoderDrawIndex) {
    return false;
  }
  if (!config.draws.empty() &&
      std::find(config.draws.begin(), config.draws.end(), encoderDrawIndex) ==
          config.draws.end()) {
    return false;
  }
  if (config.commandIndex.has_value() &&
      *config.commandIndex != commandIndex) {
    return false;
  }
  if (config.commandIndexMin.has_value() &&
      commandIndex < *config.commandIndexMin) {
    return false;
  }
  if (config.commandIndexMax.has_value() &&
      commandIndex > *config.commandIndexMax) {
    return false;
  }
  const u64 texture0 = drawState.hot->textures[0]
      ? drawState.hot->textures[0].value
      : 0ull;
  if (config.texture0.has_value() && *config.texture0 != texture0) {
    return false;
  }
  if (!config.texture0s.empty() &&
      std::find(config.texture0s.begin(), config.texture0s.end(), texture0) ==
          config.texture0s.end()) {
    return false;
  }
  return config.draw.has_value() ||
         !config.draws.empty() ||
         config.commandIndex.has_value() ||
         config.commandIndexMin.has_value() ||
         config.commandIndexMax.has_value() ||
         config.texture0.has_value() ||
         !config.texture0s.empty();
}

std::string colorAttachmentDumpPath(const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (config.dir.empty()) {
    return config.path;
  }
  std::ostringstream name;
  name << "color-s" << active.seq
       << "-e" << active.enc;
  if (active.afterDraw) {
    name << "-after-draw-d" << active.draw;
    name << "-ci" << active.commandIndex;
    name << "-cmd-d" << active.commandDrawIndex
         << "-of" << active.commandDrawCount;
  } else {
    name << "-pass-end";
  }
  if (active.texture0 != 0) {
    name << "-tex0x" << std::hex << active.texture0 << std::dec;
  }
  name << ".bin";
  return (std::filesystem::path(config.dir) / name.str()).string();
}

bool drawTextureDumpPassMatches(u64 seq, u64 enc) {
  const auto& config = drawTextureDumpConfig();
  if (!config.enabled) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != seq) {
    return false;
  }
  if (config.seqMin.has_value() && seq < *config.seqMin) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != enc) {
    return false;
  }
  return true;
}

u64 drawTextureDumpKey(const ActiveDrawTextureDump& active) {
  u64 key = active.handle.value;
  key ^= static_cast<u64>(active.srgb ? 0x9e37u : 0u) << 48u;
  key ^= static_cast<u64>(active.vertexStage ? 0x51u : 0x23u) << 40u;
  return key;
}

bool drawTextureDumpAlreadyQueued(std::span<const ActiveDrawTextureDump> active,
                                  const ActiveDrawTextureDump& candidate) {
  const u64 key = drawTextureDumpKey(candidate);
  return std::any_of(active.begin(), active.end(), [&](const auto& entry) {
    return drawTextureDumpKey(entry) == key;
  });
}

bool drawTextureDumpMarkStarted(const ActiveDrawTextureDump& active) {
  static std::mutex mutex;
  static std::unordered_set<u64> started;
  const u64 key = drawTextureDumpKey(active);
  std::lock_guard lock(mutex);
  return started.insert(key).second;
}

std::string drawTextureDumpShaderStageName(const ActiveDrawTextureDump& active) {
  return active.vertexStage ? "vertex" : "fragment";
}

std::string drawTextureDumpJsonPath(const ActiveDrawTextureDump& active) {
  std::ostringstream filename;
  filename << "texture-h0x" << std::hex << active.handle.value << std::dec
           << "-seq" << active.seq
           << "-enc" << active.enc
           << "-" << drawTextureDumpShaderStageName(active) << active.stage
           << "-" << (active.srgb ? "srgb" : "linear")
           << ".json";
  return (std::filesystem::path(drawTextureDumpConfig().dir) /
          filename.str()).string();
}

std::string drawTextureDumpSubresourceBasename(
    const ActiveDrawTextureDump& active,
    u32 level,
    u32 slice) {
  std::ostringstream filename;
  filename << "texture-h0x" << std::hex << active.handle.value << std::dec
           << "-seq" << active.seq
           << "-enc" << active.enc
           << "-" << drawTextureDumpShaderStageName(active) << active.stage
           << "-" << (active.srgb ? "srgb" : "linear")
           << "-slice" << slice
           << "-level" << level
           << ".bin";
  return filename.str();
}

void writeDepthAttachmentDump(const ActiveDepthAttachmentDump& active,
                              std::string path,
                              u32 rowBytes,
                              u64 byteCount,
                              const void* bytes) {
  if (!bytes || path.empty() || byteCount == 0) {
    return;
  }
  try {
    const std::filesystem::path binaryPath(path);
    if (const auto parent = binaryPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    {
      std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(byteCount));
    }
    {
      std::ofstream meta(path + ".json", std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"metalPixelFormat\": " << static_cast<u32>(active.metalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"rowBytes\": " << rowBytes << ",\n"
           << "  \"byteCount\": " << byteCount << ",\n"
           << "  \"hasDepth\": " << (active.hasDepth ? 1 : 0) << ",\n"
           << "  \"hasStencil\": " << (active.hasStencil ? 1 : 0) << "\n"
           << "}\n";
    }
    std::ostringstream out;
    out << "[dxmt9-depth] dump handle=0x" << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " size=" << active.width << "x" << active.height
        << " rowBytes=" << rowBytes
        << " bytes=" << byteCount
        << " path=" << path;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-depth] dump write-failed handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " path=" << path
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void writeColorAttachmentDump(const ActiveColorAttachmentDump& active,
                              std::string path,
                              u32 rowBytes,
                              u64 byteCount,
                              const void* bytes) {
  if (!bytes || path.empty() || byteCount == 0) {
    return;
  }
  try {
    const std::filesystem::path binaryPath(path);
    if (const auto parent = binaryPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    {
      std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(byteCount));
    }
    {
      std::ofstream meta(path + ".json", std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"index\": " << active.index << ",\n"
           << "  \"afterDraw\": " << (active.afterDraw ? 1 : 0) << ",\n"
           << "  \"draw\": " << active.draw << ",\n"
           << "  \"commandIndex\": " << active.commandIndex << ",\n"
           << "  \"commandDrawIndex\": " << active.commandDrawIndex << ",\n"
           << "  \"commandDrawCount\": " << active.commandDrawCount << ",\n"
           << "  \"texture0\": \"0x" << std::hex << active.texture0 << std::dec << "\",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"metalPixelFormat\": " << static_cast<u32>(active.metalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"rowBytes\": " << rowBytes << ",\n"
           << "  \"byteCount\": " << byteCount << "\n"
           << "}\n";
    }
    std::ostringstream out;
    out << "[dxmt9-color] dump handle=0x" << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " index=" << active.index
        << " afterDraw=" << (active.afterDraw ? 1 : 0)
        << " draw=" << active.draw
        << " commandIndex=" << active.commandIndex
        << " commandDrawIndex=" << active.commandDrawIndex
        << " commandDrawCount=" << active.commandDrawCount
        << " texture0=0x" << std::hex << active.texture0 << std::dec
        << " format=" << static_cast<unsigned>(active.format)
        << " size=" << active.width << "x" << active.height
        << " rowBytes=" << rowBytes
        << " bytes=" << byteCount
        << " path=" << path;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-color] dump write-failed handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " index=" << active.index
        << " path=" << path
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void appendColorAttachmentRoiSummary(
    const ActiveColorAttachmentDump& active,
    std::string path,
    ColorAttachmentReadbackRegion region,
    u32 rowBytes,
    const void* bytes) {
  const auto& config = colorAttachmentDumpConfig();
  if (path.empty() || !bytes || region.width == 0 || region.height == 0) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp < 4) {
    return;
  }

  const auto rois = config.rois.empty()
      ? std::vector<ColorAttachmentDumpConfig::Roi>{
            ColorAttachmentDumpConfig::Roi{
                0, 0, active.width, active.height, "full"}}
      : config.rois;
  const auto* data = static_cast<const u8*>(bytes);

  try {
    const std::filesystem::path csvPath(path);
    if (const auto parent = csvPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const bool writeHeader =
        !std::filesystem::exists(csvPath) ||
        std::filesystem::file_size(csvPath) == 0;
    std::ofstream out(csvPath, std::ios::app);
    if (writeHeader) {
      out << "handle,seq,enc,index,after_draw,draw,command_index,"
             "command_draw_index,command_draw_count,texture0,format,"
             "format_name,width,height,readback_x,readback_y,readback_width,"
             "readback_height,roi,roi_rect,pixels,avg_r,avg_g,avg_b,max_r,"
             "max_g,max_b,bright_pixels,white_pixels,warm_pixels,hot_x,hot_y,"
             "hot_r,hot_g,hot_b,warm_hot_x,warm_hot_y,warm_hot_r,warm_hot_g,"
             "warm_hot_b\n";
    }

    for (const auto& roi : rois) {
      const u32 left = std::min(roi.left, active.width);
      const u32 top = std::min(roi.top, active.height);
      const u32 right = std::min(roi.right, active.width);
      const u32 bottom = std::min(roi.bottom, active.height);
      if (right <= left || bottom <= top) {
        continue;
      }
      const u32 sampleLeft = std::max(left, region.x);
      const u32 sampleTop = std::max(top, region.y);
      const u32 sampleRight = std::min(right, region.x + region.width);
      const u32 sampleBottom = std::min(bottom, region.y + region.height);
      if (sampleRight <= sampleLeft || sampleBottom <= sampleTop) {
        continue;
      }

      u64 sumR = 0;
      u64 sumG = 0;
      u64 sumB = 0;
      u32 maxR = 0;
      u32 maxG = 0;
      u32 maxB = 0;
      u64 bright = 0;
      u64 white = 0;
      u64 warm = 0;
      u32 hotX = sampleLeft;
      u32 hotY = sampleTop;
      u32 hotR = 0;
      u32 hotG = 0;
      u32 hotB = 0;
      u32 hotMax = 0;
      u32 warmHotX = sampleLeft;
      u32 warmHotY = sampleTop;
      u32 warmHotR = 0;
      u32 warmHotG = 0;
      u32 warmHotB = 0;
      u32 warmHotScore = 0;
      const u64 pixels =
          static_cast<u64>(sampleRight - sampleLeft) *
          static_cast<u64>(sampleBottom - sampleTop);

      for (u32 y = sampleTop; y < sampleBottom; ++y) {
        const u64 rowOffset =
            static_cast<u64>(y - region.y) * rowBytes +
            static_cast<u64>(sampleLeft - region.x) * bpp;
        for (u32 x = sampleLeft; x < sampleRight; ++x) {
          const u64 offset = rowOffset + static_cast<u64>(x - sampleLeft) * bpp;
          const u32 b = data[offset + 0];
          const u32 g = data[offset + 1];
          const u32 r = data[offset + 2];
          sumR += r;
          sumG += g;
          sumB += b;
          maxR = std::max(maxR, r);
          maxG = std::max(maxG, g);
          maxB = std::max(maxB, b);
          const u32 channelMax = std::max({r, g, b});
          if (channelMax > config.brightThreshold) {
            ++bright;
          }
          if (r > config.whiteThreshold &&
              g > config.whiteThreshold &&
              b > config.whiteThreshold) {
            ++white;
          }
          const bool warmPixel =
              r >= config.warmRedThreshold &&
              g >= config.warmGreenThreshold &&
              static_cast<u64>(b) <=
                  static_cast<u64>(r) + config.warmBlueMargin;
          if (warmPixel) {
            ++warm;
            const u32 warmScore = r + g + b;
            if (warmScore > warmHotScore) {
              warmHotScore = warmScore;
              warmHotX = x;
              warmHotY = y;
              warmHotR = r;
              warmHotG = g;
              warmHotB = b;
            }
          }
          if (channelMax > hotMax) {
            hotMax = channelMax;
            hotX = x;
            hotY = y;
            hotR = r;
            hotG = g;
            hotB = b;
          }
        }
      }

      const double denom = pixels ? static_cast<double>(pixels) : 1.0;
      out << "0x" << std::hex << active.handle.value << std::dec
          << ',' << active.seq
          << ',' << active.enc
          << ',' << active.index
          << ',' << (active.afterDraw ? 1 : 0)
          << ',' << active.draw
          << ',' << active.commandIndex
          << ',' << active.commandDrawIndex
          << ',' << active.commandDrawCount
          << ",0x" << std::hex << active.texture0 << std::dec
          << ',' << static_cast<u32>(active.format)
          << ',' << core::formatName(active.format)
          << ',' << active.width
          << ',' << active.height
          << ',' << region.x
          << ',' << region.y
          << ',' << region.width
          << ',' << region.height
          << ',' << roi.name
          << ',' << left << ' ' << top << ' ' << right << ' ' << bottom
          << ',' << pixels
          << ',' << std::fixed << std::setprecision(3) << (sumR / denom)
          << ',' << (sumG / denom)
          << ',' << (sumB / denom)
          << ',' << maxR
          << ',' << maxG
          << ',' << maxB
          << ',' << bright
          << ',' << white
          << ',' << warm
          << ',' << hotX
          << ',' << hotY
          << ',' << hotR
          << ',' << hotG
          << ',' << hotB
          << ',' << warmHotX
          << ',' << warmHotY
          << ',' << warmHotR
          << ',' << warmHotG
          << ',' << warmHotB
          << '\n';
    }

    std::ostringstream trace;
    trace << "[dxmt9-color] roi-summary seq=" << active.seq
          << " enc=" << active.enc
          << " afterDraw=" << (active.afterDraw ? 1 : 0)
          << " draw=" << active.draw
          << " commandIndex=" << active.commandIndex
          << " texture0=0x" << std::hex << active.texture0 << std::dec
          << " rois=" << rois.size()
          << " path=" << path;
    emitTextureTraceLine(trace.str());
  } catch (const std::exception& e) {
    std::ostringstream trace;
    trace << "[dxmt9-color] roi-summary write-failed seq=" << active.seq
          << " enc=" << active.enc
          << " path=" << path
          << " error=" << e.what();
    emitTextureTraceLine(trace.str());
  }
}

void writeDrawTextureDump(const TextureSidecarReadbackBatch& batch) {
  const auto& active = batch.active;
  const std::string jsonPath = drawTextureDumpJsonPath(active);
  try {
    const std::filesystem::path metadataPath(jsonPath);
    const auto parent = metadataPath.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    for (const auto& subresource : batch.subresources) {
      if (!subresource.bytes || subresource.byteCount == 0 ||
          subresource.basename.empty()) {
        continue;
      }
      const auto binPath = parent / subresource.basename;
      std::ofstream out(binPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(subresource.bytes),
                static_cast<std::streamsize>(subresource.byteCount));
    }

    {
      std::ofstream meta(jsonPath, std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"textureIndex\": " << active.textureIndex << ",\n"
           << "  \"stage\": " << active.stage << ",\n"
           << "  \"shaderStage\": \"" << drawTextureDumpShaderStageName(active) << "\",\n"
           << "  \"srgb\": " << (active.srgb ? 1 : 0) << ",\n"
           << "  \"shaderReadView\": " << (active.shaderReadView ? 1 : 0) << ",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"type\": " << static_cast<u32>(active.type) << ",\n"
           << "  \"storageMetalPixelFormat\": "
           << static_cast<u32>(active.storageMetalPixelFormat) << ",\n"
           << "  \"shaderMetalPixelFormat\": "
           << static_cast<u32>(active.shaderMetalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"depth\": " << active.depth << ",\n"
           << "  \"levels\": " << active.levels << ",\n"
           << "  \"subresources\": [\n";
      for (std::size_t i = 0; i < batch.subresources.size(); ++i) {
        const auto& subresource = batch.subresources[i];
        meta << "    {\n"
             << "      \"level\": " << subresource.level << ",\n"
             << "      \"slice\": " << subresource.slice << ",\n"
             << "      \"width\": " << subresource.width << ",\n"
             << "      \"height\": " << subresource.height << ",\n"
             << "      \"depth\": " << subresource.depth << ",\n"
             << "      \"rowBytes\": " << subresource.rowBytes << ",\n"
             << "      \"bytesPerImage\": " << subresource.bytesPerImage << ",\n"
             << "      \"byteCount\": " << subresource.byteCount << ",\n"
             << "      \"path\": \"" << subresource.basename << "\"\n"
             << "    }" << (i + 1u == batch.subresources.size() ? "\n" : ",\n");
      }
      meta << "  ]\n"
           << "}\n";
    }

    std::ostringstream out;
    out << "[dxmt9-texture] draw-sidecar-dump handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " stage=" << drawTextureDumpShaderStageName(active) << active.stage
        << " srgb=" << (active.srgb ? 1 : 0)
        << " format=" << static_cast<unsigned>(active.format)
        << " size=" << active.width << "x" << active.height
        << " levels=" << active.levels
        << " subresources=" << batch.subresources.size()
        << " path=" << jsonPath;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-texture] draw-sidecar-dump write-failed handle=0x"
        << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " path=" << jsonPath
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void maybeEncodeDepthAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveDepthAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!depthAttachmentDumpMatches(active)) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp == 0 || active.width == 0 || active.height == 0) {
    return;
  }
  const u32 rowBytes = active.width * bpp;
  const u64 byteCount = static_cast<u64>(rowBytes) * active.height;
  WMTBufferInfo bufInfo{};
  bufInfo.length = byteCount;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuffer = device.newBuffer(bufInfo);
  void* readbackMemory = bufInfo.memory.get_accessible_or_null();
  if (!readbackBuffer || !readbackMemory) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[DepthAttachmentDump seq=%llu enc=%llu handle=0x%llx]",
      static_cast<unsigned long long>(active.seq),
      static_cast<unsigned long long>(active.enc),
      static_cast<unsigned long long>(active.handle.value)));
  WMTOrigin origin{0, 0, 0};
  WMTSize size{active.width, active.height, 1};
  blit.copyFromTextureToBuffer(WMT::Texture{active.texture.handle}, 0, 0,
                               origin, size, WMT::Buffer{readbackBuffer.handle},
                               0, rowBytes, 0);
  blit.endEncoding();
  const auto path = depthAttachmentDumpConfig().path;
  completionCallbacks.push_back(
      [active, path, rowBytes, byteCount, readbackBuffer, readbackMemory] {
        (void)readbackBuffer;
        writeDepthAttachmentDump(active, path, rowBytes, byteCount, readbackMemory);
      });
}

void maybeEncodeColorAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveColorAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!colorAttachmentDumpMatches(active)) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp == 0 || active.width == 0 || active.height == 0) {
    return;
  }
  const auto region = colorAttachmentDumpReadbackRegion(active);
  if (region.width == 0 || region.height == 0) {
    return;
  }
  const u32 rowBytes = region.width * bpp;
  const u64 byteCount = static_cast<u64>(rowBytes) * region.height;
  WMTBufferInfo bufInfo{};
  bufInfo.length = byteCount;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuffer = device.newBuffer(bufInfo);
  void* readbackMemory = bufInfo.memory.get_accessible_or_null();
  if (!readbackBuffer || !readbackMemory) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[ColorAttachmentDump seq=%llu enc=%llu handle=0x%llx]",
      static_cast<unsigned long long>(active.seq),
      static_cast<unsigned long long>(active.enc),
      static_cast<unsigned long long>(active.handle.value)));
  WMTOrigin origin{region.x, region.y, 0};
  WMTSize size{region.width, region.height, 1};
  blit.copyFromTextureToBuffer(WMT::Texture{active.texture.handle}, 0, 0,
                               origin, size, WMT::Buffer{readbackBuffer.handle},
                               0, rowBytes, 0);
  blit.endEncoding();
  const auto path = colorAttachmentDumpPath(active);
  const auto summaryPath = colorAttachmentDumpConfig().roiSummaryPath;
  completionCallbacks.push_back(
      [active, path, summaryPath, region, rowBytes, byteCount,
       readbackBuffer, readbackMemory] {
        (void)readbackBuffer;
        if (!path.empty()) {
          writeColorAttachmentDump(active, path, rowBytes, byteCount, readbackMemory);
        }
        appendColorAttachmentRoiSummary(active, summaryPath, region, rowBytes,
                                        readbackMemory);
      });
}

void maybeCollectDrawTextureDump(
    std::vector<ActiveDrawTextureDump>& activeDumps,
    const resources::Pool& pool,
    core::FlatDrawStateView drawState,
    u64 seq,
    u64 enc) {
  if (!drawState.hot || !drawTextureDumpPassMatches(seq, enc)) {
    return;
  }
  const auto& hot = *drawState.hot;
  for (u32 textureIndex = 0; textureIndex < core::kMaxTextures; ++textureIndex) {
    const auto handle = hot.textures[textureIndex];
    const auto* record = pool.findTexture(handle.value);
    if (!record || !record->texture) {
      continue;
    }
    if (!drawTextureDumpWantsTexture(textureIndex, handle, *record)) {
      continue;
    }
    const bool vertexStage = textureIndex >= core::kVertexTextureSampler0;
    const u32 stage = vertexStage ? textureIndex - core::kVertexTextureSampler0
                                  : textureIndex;
    const bool srgbTexture =
        core::flatStateOr(hot.samplerStates[textureIndex],
                          core::SAMP_SRGB_TEXTURE, 0u) != 0;
    ActiveDrawTextureDump candidate{};
    candidate.handle = handle;
    candidate.texture = resources::textureForShaderRead(*record, srgbTexture);
    candidate.format = record->desc.format;
    candidate.type = record->desc.type;
    candidate.storageMetalPixelFormat =
        WMT::Texture{record->texture.handle}.pixelFormat();
    candidate.shaderMetalPixelFormat =
        candidate.texture ? candidate.texture.pixelFormat()
                          : WMTPixelFormatInvalid;
    candidate.width = std::max(1u, record->desc.width);
    candidate.height = std::max(1u, record->desc.height);
    candidate.depth = std::max(1u, record->desc.depth);
    candidate.levels = std::max(1u, record->desc.levels);
    candidate.textureIndex = textureIndex;
    candidate.stage = stage;
    candidate.vertexStage = vertexStage;
    candidate.srgb = srgbTexture;
    candidate.shaderReadView =
        (srgbTexture && record->srgbShaderReadTexture) ||
        (!srgbTexture && record->shaderReadTexture);
    candidate.seq = seq;
    candidate.enc = enc;
    if (!candidate.texture ||
        drawTextureDumpAlreadyQueued(activeDumps, candidate) ||
        !drawTextureDumpMarkStarted(candidate)) {
      continue;
    }
    activeDumps.push_back(candidate);
  }
}

void maybeEncodeDrawTextureDumps(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    std::span<const ActiveDrawTextureDump> activeDumps,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!drawTextureDumpConfig().enabled || activeDumps.empty()) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[DrawTextureDump count=%llu]",
      static_cast<unsigned long long>(activeDumps.size())));
  for (const auto& active : activeDumps) {
    auto batch = std::make_shared<TextureSidecarReadbackBatch>();
    batch->active = active;
    const u32 sliceCount =
        active.type == core::TextureType::Cube ? 6u : 1u;
    for (u32 slice = 0; slice < sliceCount; ++slice) {
      for (u32 level = 0; level < active.levels; ++level) {
        const u32 width = std::max(1u, active.width >> level);
        const u32 height = std::max(1u, active.height >> level);
        const u32 depth =
            active.type == core::TextureType::Volume
                ? std::max(1u, active.depth >> level)
                : 1u;
        const u32 rowBytes = core::formatRowPitch(active.format, width);
        const u64 bytesPerImage =
            static_cast<u64>(core::formatByteSize(active.format, width, height));
        const u64 byteCount = bytesPerImage * depth;
        if (rowBytes == 0 || bytesPerImage == 0 || byteCount == 0) {
          continue;
        }
        WMTBufferInfo bufInfo{};
        bufInfo.length = byteCount;
        bufInfo.options = WMTResourceStorageModeShared;
        auto readbackBuffer = device.newBuffer(bufInfo);
        const void* readbackMemory = bufInfo.memory.get_accessible_or_null();
        if (!readbackBuffer || !readbackMemory) {
          continue;
        }
        TextureSubresourceReadback subresource{};
        subresource.level = level;
        subresource.slice = slice;
        subresource.width = width;
        subresource.height = height;
        subresource.depth = depth;
        subresource.rowBytes = rowBytes;
        subresource.bytesPerImage = bytesPerImage;
        subresource.byteCount = byteCount;
        subresource.basename =
            drawTextureDumpSubresourceBasename(active, level, slice);
        subresource.buffer = readbackBuffer;
        subresource.bytes = readbackMemory;
        WMTOrigin origin{0, 0, 0};
        WMTSize size{width, height, depth};
        blit.copyFromTextureToBuffer(
            active.texture,
            active.type == core::TextureType::Volume ? 0u : slice,
            level, origin, size, WMT::Buffer{readbackBuffer.handle}, 0,
            rowBytes, active.type == core::TextureType::Volume ? bytesPerImage : 0);
        batch->subresources.push_back(std::move(subresource));
      }
    }
    if (!batch->subresources.empty()) {
      completionCallbacks.push_back([batch] {
        writeDrawTextureDump(*batch);
      });
    }
  }
  blit.endEncoding();
}

bool colorAttachmentAliasesTracedTexture(resources::Pool& pool,
                                         const core::FlatDrawStateRecord& hot,
                                         std::size_t* attachmentIndex) {
  const u64 wanted = debug::traceTextureHandle();
  if (wanted == 0) {
    return false;
  }
  for (std::size_t i = 0; i < hot.colorAttachments.size(); ++i) {
    const auto handle = hot.colorAttachments[i].handle;
    if (!handle) {
      continue;
    }
    const auto* surface = pool.findSurface(handle.value);
    if (surface && surface->aliasTexture.value == wanted) {
      if (attachmentIndex) {
        *attachmentIndex = i;
      }
      return true;
    }
  }
  return false;
}

void traceRenderTargetWriteForTexture(resources::Pool& pool,
                                      const core::FlatDrawStateRecord& hot,
                                      u64 seqId,
                                      u64 drawOrdinal) {
  std::size_t attachmentIndex = 0;
  if (!colorAttachmentAliasesTracedTexture(pool, hot, &attachmentIndex)) {
    return;
  }
  const auto handle = hot.colorAttachments[attachmentIndex].handle;
  std::ostringstream out;
  out << "[dxmt9-texture] render-target-write seq="
      << static_cast<unsigned long long>(seqId)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " colorIndex=" << attachmentIndex
      << " surface=0x" << std::hex << handle.value << std::dec
      << " texture=0x" << std::hex << debug::traceTextureHandle() << std::dec
      << " colorWrite=" << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
      << " srgbWrite=" << core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u)
      << " alphaBlend=" << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
      << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
      << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
      << " tex0=0x" << std::hex << hot.textures[0].value << std::dec;
  emitTextureTraceLine(out.str());
}


bool suppressRecordedMetalCalls(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressMetalCalls;
}

bool suppressBaseStateLookup(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressBaseStateLookup;
}

void recordedSetRenderPipelineState(EncodeContext& ctx,
                                    WMT::RenderCommandEncoder& encoder,
                                    WMT::RenderPipelineState pipeline) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRenderPipelineState) {
    recorder->setRenderPipelineState(recorder->userdata, pipeline);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRenderPipelineState(pipeline);
  }
}

void recordedSetDepthStencilState(EncodeContext& ctx,
                                  WMT::RenderCommandEncoder& encoder,
                                  WMT::DepthStencilState depthStencil,
                                  std::uint8_t stencilRef) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setDepthStencilState) {
    recorder->setDepthStencilState(recorder->userdata, depthStencil, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setDepthStencilState(depthStencil, stencilRef);
  }
}

void recordedSetBlendColorAndStencilRef(EncodeContext& ctx,
                                        WMT::RenderCommandEncoder& encoder,
                                        float red,
                                        float green,
                                        float blue,
                                        float alpha,
                                        std::uint8_t stencilRef) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setBlendColorAndStencilRef) {
    recorder->setBlendColorAndStencilRef(
        recorder->userdata, red, green, blue, alpha, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setBlendColorAndStencilRef(red, green, blue, alpha, stencilRef);
  }
}

void recordedSetViewport(EncodeContext& ctx,
                         WMT::RenderCommandEncoder& encoder,
                         WMTViewport viewport) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setViewport) {
    recorder->setViewport(recorder->userdata, viewport);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setViewport(viewport);
  }
}

void recordedSetScissorRect(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTScissorRect rect) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setScissorRect) {
    recorder->setScissorRect(recorder->userdata, rect);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setScissorRect(rect);
  }
}

void recordedSetRasterizerState(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMTTriangleFillMode fillMode,
                                WMTCullMode cullMode,
                                WMTDepthClipMode depthClipMode,
                                WMTWinding winding,
                                float depthBias,
                                float slopeScale,
                                float depthBiasClamp) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRasterizerState) {
    recorder->setRasterizerState(recorder->userdata, fillMode, cullMode,
                                 depthClipMode, winding, depthBias,
                                 slopeScale, depthBiasClamp);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRasterizerState(fillMode, cullMode, depthClipMode, winding,
                               depthBias, slopeScale, depthBiasClamp);
  }
}

void recordedSetFragmentTexture(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMT::Texture texture,
                                std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentTexture) {
    recorder->setFragmentTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentTexture(texture, index);
  }
}

void recordedSetFragmentSamplerState(EncodeContext& ctx,
                                     WMT::RenderCommandEncoder& encoder,
                                     WMT::SamplerState sampler,
                                     std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentSamplerState) {
    recorder->setFragmentSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentSamplerState(sampler, index);
  }
}

void recordedSetVertexTexture(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              WMT::Texture texture,
                              std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexTexture) {
    recorder->setVertexTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexTexture(texture, index);
  }
}

void recordedSetVertexSamplerState(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMT::SamplerState sampler,
                                   std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexSamplerState) {
    recorder->setVertexSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexSamplerState(sampler, index);
  }
}

void recordedSetVertexBuffer(EncodeContext& ctx,
                             WMT::RenderCommandEncoder& encoder,
                             WMT::Buffer buffer,
                             u64 offset,
                             std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBuffer) {
    recorder->setVertexBuffer(recorder->userdata, buffer, offset, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBuffer(buffer, offset, index);
  }
}

void recordedSetVertexBytes(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            const void* bytes,
                            u64 length,
                            std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBytes) {
    recorder->setVertexBytes(recorder->userdata, bytes, length, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBytes(bytes, length, index);
  }
}

void recordedSetFragmentBytes(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              const void* bytes,
                              u64 length,
                              std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentBytes) {
    recorder->setFragmentBytes(recorder->userdata, bytes, length, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentBytes(bytes, length, index);
  }
}

void recordedDrawPrimitives(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTPrimitiveType primitiveType,
                            u64 vertexStart,
                            u64 vertexCount,
                            u32 instanceCount,
                            u32 baseInstance) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawPrimitives) {
    recorder->drawPrimitives(recorder->userdata, primitiveType,
                             vertexStart, vertexCount, instanceCount,
                             baseInstance);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawPrimitives(primitiveType, vertexStart, vertexCount,
                           instanceCount, baseInstance);
  }
}

void recordedDrawIndexedPrimitives(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMTPrimitiveType primitiveType,
                                   WMTIndexType indexType,
                                   u64 indexCount,
                                   WMT::Buffer indexBuffer,
                                   u64 indexBufferOffset,
                                   u32 instanceCount,
                                   i32 baseVertex,
                                   u32 baseInstance) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawIndexedPrimitives) {
    recorder->drawIndexedPrimitives(recorder->userdata, primitiveType,
                                    indexType, indexCount, indexBuffer,
                                    indexBufferOffset, instanceCount,
                                    baseVertex, baseInstance);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawIndexedPrimitives(primitiveType, indexType, indexCount,
                                  indexBuffer, indexBufferOffset,
                                  instanceCount, baseVertex, baseInstance);
  }
}

void countTextureBind() {
  perf::countBaseStateBind(1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countSamplerBind() {
  perf::countBaseStateBind(0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countTextureBindSkipped() {
  perf::countBaseStateBindSkip(1, 0);
}

void countSamplerBindSkipped() {
  perf::countBaseStateBindSkip(0, 1);
}

void countVertexBufferBind() {
  perf::countBaseStateBind(0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
}

void countIndexBufferBind() {
  perf::countBaseStateBind(0, 0, 0, 1, 0, 0, 0, 0, 0, 0);
}

void countUniformBufferBinds(std::uint32_t count) {
  perf::countBaseStateBind(0, 0, 0, 0, count, 0, 0, 0, 0, 0);
}

void countPipelineBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 1, 0, 0, 0, 0);
}

void countDepthStateBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 1, 0, 0, 0);
}

void countViewportBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 1, 0, 0);
}

void countScissorBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 1, 0);
}

void countRasterizerBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
}

void countVertexBufferBindSkipped() {
  perf::countBaseStateBindSkipExtended(1, 0, 0, 0, 0, 0, 0);
}

void countPipelineBindSkipped() {
  perf::countBaseStateBindSkipExtended(0, 0, 1, 0, 0, 0, 0);
}

void countDepthStateBindSkipped() {
  perf::countBaseStateBindSkipExtended(0, 0, 0, 1, 0, 0, 0);
}


bool x8ShaderAlphaFillEnabledForDiagnostics();

bool effectDrawTraceEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

namespace {

std::optional<u64> effectDrawTraceSeqFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ");
  return value;
}

std::optional<u64> effectDrawTraceSeqMinFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN");
  return value;
}

std::optional<u64> effectDrawTraceSeqMaxFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ_MAX");
  return value;
}

bool effectDrawTraceSeqMatches(u64 seqId) {
  if (const auto seqFilter = effectDrawTraceSeqFilter();
      seqFilter.has_value()) {
    return *seqFilter == seqId;
  }
  if (const auto minFilter = effectDrawTraceSeqMinFilter();
      minFilter.has_value() && seqId < *minFilter) {
    return false;
  }
  if (const auto maxFilter = effectDrawTraceSeqMaxFilter();
      maxFilter.has_value() && seqId > *maxFilter) {
    return false;
  }
  return true;
}

std::optional<u64> effectDrawTraceEncoderFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_ENC");
  return value;
}

std::optional<u64> effectDrawTraceTexture0Filter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0");
  return value;
}

std::optional<u64> effectDrawTraceTexture0WidthFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_WIDTH");
  return value;
}

std::optional<u64> effectDrawTraceTexture0HeightFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_HEIGHT");
  return value;
}

std::optional<u64> effectDrawTraceTexture0FormatFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_FORMAT");
  return value;
}

std::optional<u64> effectDrawTracePrimitiveTypeFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_TYPE");
  return value;
}

bool effectDrawTracePointSpriteOnly() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_POINT_SPRITE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool effectDrawTraceIncludeNonAlpha() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool effectDrawTraceIncludeUntextured() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

}  // namespace

bool effectDrawTraceGeometryEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

namespace {

u64 effectDrawTraceGeometryMaxRefs() {
  static const u64 value = [] {
    const auto envValue =
        parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS");
    return envValue.value_or(384ull);
  }();
  return value;
}

}  // namespace


void traceEffectDraw(const ActiveEncoderBreakdown* encoderBreakdown,
                     const core::FlatDrawStateRecord& hot,
                     const resources::Pool& pool,
                     u64 seqId,
                     u64 drawOrdinal,
                     std::uint32_t commandIndex,
                     u64 commandDrawIndex,
                     u64 commandDrawCount,
                     core::PrimitiveType primitiveType,
                     u32 primitiveCount,
                     u64 vertexCount,
                     bool indexedDraw,
                     bool fixedFunctionPath,
                     bool preTransformed,
                     u64 vertexShaderHash,
                     u64 pixelShaderHash) {
  if (!effectDrawTraceEnabled()) {
    return;
  }
  const auto encoderIndex = encoderBreakdown ? encoderBreakdown->stats.encoderIndex : 0ull;
  if (!effectDrawTraceSeqMatches(seqId)) {
    return;
  }
  if (const auto encFilter = effectDrawTraceEncoderFilter();
      encFilter.has_value() && *encFilter != encoderIndex) {
    return;
  }
  const bool alphaBlendEnabled =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  if (!alphaBlendEnabled && !effectDrawTraceIncludeNonAlpha()) {
    return;
  }
  if (hot.textureMask == 0u && !effectDrawTraceIncludeUntextured()) {
    return;
  }
  const auto srcBlend = core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u);
  const auto dstBlend = core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u);
  const auto blendOp = core::flatStateOr(hot.renderStates, RS_BLEND_OP, 0u);
  const bool depthEnabled =
      core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const auto depthFunc = core::flatStateOr(
      hot.renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual));
  const float depthBias = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_DEPTH_BIAS, 0u));
  const float slopeScale = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  const bool pointSpriteState =
      core::flatStateOr(hot.renderStates, RS_POINT_SPRITE_ENABLE, 0u) != 0u;
  const bool pointSpriteCandidate =
      pointSpriteState && primitiveType == core::PrimitiveType::PointList;
  if (effectDrawTracePointSpriteOnly() && !pointSpriteCandidate) {
    return;
  }
  if (const auto primitiveTypeFilter = effectDrawTracePrimitiveTypeFilter();
      primitiveTypeFilter.has_value() &&
      static_cast<u64>(primitiveType) != *primitiveTypeFilter) {
    return;
  }
  const bool pointScaleState =
      core::flatStateOr(hot.renderStates, RS_POINT_SCALE_ENABLE, 0u) != 0u;
  const float pointSize = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE, std::bit_cast<u32>(1.0f)));
  const float pointSizeMin = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MIN, std::bit_cast<u32>(1.0f)));
  const float pointSizeMax = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MAX, std::bit_cast<u32>(64.0f)));
  const auto colorWrite =
      core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
  const auto encoderDrawIndex =
      encoderBreakdown ? encoderBreakdown->stats.drawCalls + 1ull : 0ull;
  std::array<const resources::TextureRecord*, core::kMaxTextures> textureRecords{};
  for (std::size_t i = 0; i < core::kMaxTextures; ++i) {
    textureRecords[i] =
        hot.textures[i] ? pool.findTexture(hot.textures[i].value) : nullptr;
  }
  if (const auto handleFilter = effectDrawTraceTexture0Filter();
      handleFilter.has_value() &&
      (!hot.textures[0] || hot.textures[0].value != *handleFilter)) {
    return;
  }
  if (const auto widthFilter = effectDrawTraceTexture0WidthFilter();
      widthFilter.has_value() &&
      (!textureRecords[0] || textureRecords[0]->desc.width != *widthFilter)) {
    return;
  }
  if (const auto heightFilter = effectDrawTraceTexture0HeightFilter();
      heightFilter.has_value() &&
      (!textureRecords[0] || textureRecords[0]->desc.height != *heightFilter)) {
    return;
  }
  if (const auto formatFilter = effectDrawTraceTexture0FormatFilter();
      formatFilter.has_value() &&
      (!textureRecords[0] ||
       static_cast<u64>(textureRecords[0]->desc.format) != *formatFilter)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-effect-draw"
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " encoder=" << static_cast<unsigned long long>(encoderIndex)
      << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " command_index=" << commandIndex
      << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
      << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
      << " primitive_type=" << static_cast<unsigned>(primitiveType)
      << " primitive_count=" << primitiveCount
      << " vertex_count=" << static_cast<unsigned long long>(vertexCount)
      << " small=" << (primitiveCount <= 63u ? 1u : 0u)
      << " indexed=" << (indexedDraw ? 1u : 0u)
      << " ffp=" << (fixedFunctionPath ? 1u : 0u)
      << " pretransformed=" << (preTransformed ? 1u : 0u)
      << " point_sprite_state=" << (pointSpriteState ? 1u : 0u)
      << " point_sprite_candidate=" << (pointSpriteCandidate ? 1u : 0u)
      << " point_scale=" << (pointScaleState ? 1u : 0u)
      << " point_size=" << pointSize
      << " point_size_min=" << pointSizeMin
      << " point_size_max=" << pointSizeMax
      << " vs_hash=0x" << std::hex << vertexShaderHash
      << " ps_hash=0x" << pixelShaderHash
      << " texture_mask=0x" << std::hex << hot.textureMask
      << " texture0=0x" << hot.textures[0].value
      << " texture1=0x" << hot.textures[1].value
      << " texture2=0x" << hot.textures[2].value
      << " texture3=0x" << hot.textures[3].value
      << " texture4=0x" << hot.textures[4].value
      << " texture5=0x" << hot.textures[5].value
      << " texture6=0x" << hot.textures[6].value
      << " texture7=0x" << hot.textures[7].value
      << std::dec
      << " texture0_width=" << (textureRecords[0] ? textureRecords[0]->desc.width : 0u)
      << " texture0_height=" << (textureRecords[0] ? textureRecords[0]->desc.height : 0u)
      << " texture0_format="
      << (textureRecords[0] ? static_cast<u32>(textureRecords[0]->desc.format) : 0u)
      << " texture1_width=" << (textureRecords[1] ? textureRecords[1]->desc.width : 0u)
      << " texture1_height=" << (textureRecords[1] ? textureRecords[1]->desc.height : 0u)
      << " texture1_format="
      << (textureRecords[1] ? static_cast<u32>(textureRecords[1]->desc.format) : 0u)
      << " texture2_width=" << (textureRecords[2] ? textureRecords[2]->desc.width : 0u)
      << " texture2_height=" << (textureRecords[2] ? textureRecords[2]->desc.height : 0u)
      << " texture2_format="
      << (textureRecords[2] ? static_cast<u32>(textureRecords[2]->desc.format) : 0u)
      << " texture3_width=" << (textureRecords[3] ? textureRecords[3]->desc.width : 0u)
      << " texture3_height=" << (textureRecords[3] ? textureRecords[3]->desc.height : 0u)
      << " texture3_format="
      << (textureRecords[3] ? static_cast<u32>(textureRecords[3]->desc.format) : 0u)
      << " texture4_width=" << (textureRecords[4] ? textureRecords[4]->desc.width : 0u)
      << " texture4_height=" << (textureRecords[4] ? textureRecords[4]->desc.height : 0u)
      << " texture4_format="
      << (textureRecords[4] ? static_cast<u32>(textureRecords[4]->desc.format) : 0u)
      << " texture5_width=" << (textureRecords[5] ? textureRecords[5]->desc.width : 0u)
      << " texture5_height=" << (textureRecords[5] ? textureRecords[5]->desc.height : 0u)
      << " texture5_format="
      << (textureRecords[5] ? static_cast<u32>(textureRecords[5]->desc.format) : 0u)
      << " texture6_width=" << (textureRecords[6] ? textureRecords[6]->desc.width : 0u)
      << " texture6_height=" << (textureRecords[6] ? textureRecords[6]->desc.height : 0u)
      << " texture6_format="
      << (textureRecords[6] ? static_cast<u32>(textureRecords[6]->desc.format) : 0u)
      << " texture7_width=" << (textureRecords[7] ? textureRecords[7]->desc.width : 0u)
      << " texture7_height=" << (textureRecords[7] ? textureRecords[7]->desc.height : 0u)
      << " texture7_format="
      << (textureRecords[7] ? static_cast<u32>(textureRecords[7]->desc.format) : 0u)
      << " src_blend=" << srcBlend
      << " dst_blend=" << dstBlend
      << " blend_op=" << blendOp
      << " alpha_test="
      << (core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u ? 1u : 0u)
      << " depth_enabled=" << (depthEnabled ? 1u : 0u)
      << " depth_write=" << (depthWrite ? 1u : 0u)
      << " depth_func=" << depthFunc
      << " depth_bias=" << depthBias
      << " slope_scale_depth_bias=" << slopeScale
      << " color_write=0x" << std::hex << colorWrite << std::dec
      << " scissor=" << (hot.viewport.scissorEnabled ? 1u : 0u)
      << ']';
  emitQueueTraceLine(out.str());
}


void recordArgbufCbufUploadForBreakdown(void* userdata,
                                        u32 argbufIndex,
                                        const void* data,
                                        u64 bytes,
                                        u64 hostStructBytes) {
  if (!userdata) {
    return;
  }
  static_cast<ActiveEncoderBreakdown*>(userdata)->recordArgbufCbufUploadContent(
      argbufIndex, data, bytes, hostStructBytes);
}


bool renderEncoderGpuTimeEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_GPU_TIME");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool encoderBreakdownCbufContentEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN_CBUF_CONTENT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool textureSamplerDirectSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_TEXTURE_SAMPLER_DIRECT_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool encodeDrawPhaseSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool streamBindPhaseSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_STREAM_BIND_PHASE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool bindingPacketPlanSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool drawIssueSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_DRAW_ISSUE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufCbufProbeSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufReopenSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_REOPEN_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufCbufDirtyIdentityPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_CBUF_DIRTY_IDENTITY");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufPayloadDeltaPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_PAYLOAD_DELTA");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufPayloadDeltaSourcePerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

PerfCounterFn argbufCbufCachedRepointCpuRecorder(u32 argbufIndex) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointPsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointFfpVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfCounterFn argbufCbufContentProbeCpuRecorder(u32 argbufIndex) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbeVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbePsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbeFfpPsCpuTime;
    default:
      return nullptr;
  }
}

void countArgbufCbufCachedRepointStage(u32 argbufIndex, u64 bytes) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointVsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointVsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointPsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointPsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointFfpVsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointFfpVsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointFfpPsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointFfpPsBytes(bytes);
      break;
    default:
      break;
  }
}


bool x8ShaderAlphaFillEnabledForDiagnostics() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_X8_SHADER_ALPHA_FILL");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

namespace {

std::optional<u32> readEffectTraceIndexValue(
    std::span<const u8> indexBytes,
    IndexType indexType,
    std::size_t elementIndex) {
  const std::size_t elementSize = indexType == IndexType::UInt16 ? 2u : 4u;
  const std::size_t offset = elementIndex * elementSize;
  if (offset + elementSize > indexBytes.size()) {
    return std::nullopt;
  }
  if (indexType == IndexType::UInt16) {
    u16 value = 0;
    std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
    return value;
  }
  u32 value = 0;
  std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
  return value;
}

u32 effectTraceFloatComponentCount(u32 declType) {
  switch (declType) {
    case kD3DDeclTypeFloat1:
      return 1u;
    case kD3DDeclTypeFloat2:
      return 2u;
    case kD3DDeclTypeFloat3:
      return 3u;
    case kD3DDeclTypeFloat4:
      return 4u;
    default:
      return 0u;
  }
}

struct EffectTraceStream0Layout {
  bool hasPosition = false;
  bool preTransformed = false;
  u32 positionOffset = 0;
  u32 positionComponents = 0;
  bool hasDiffuse = false;
  u32 diffuseOffset = 0;
  bool hasTexcoord0 = false;
  u32 texcoord0Offset = 0;
  u32 texcoord0Components = 0;
};

EffectTraceStream0Layout resolveEffectTraceStream0Layout(
    core::FlatDrawStateView drawState) {
  EffectTraceStream0Layout out{};
  if (!drawState.hasShaderContext()) {
    return out;
  }

  const auto& vertexDecl = drawState.shaderContext().vertexDecl;
  for (const auto& element : vertexDecl.elements) {
    if (element.stream != 0u) {
      continue;
    }
    const u32 floatComponents = effectTraceFloatComponentCount(element.type);
    if ((element.usage == kD3DDeclUsagePosition ||
         element.usage == kD3DDeclUsagePositionT) &&
        element.usageIndex == 0u && floatComponents >= 2u) {
      if (!out.hasPosition || element.usage == kD3DDeclUsagePositionT) {
        out.hasPosition = true;
        out.preTransformed = element.usage == kD3DDeclUsagePositionT;
        out.positionOffset = element.offset;
        out.positionComponents = floatComponents;
      }
    } else if (!out.hasDiffuse &&
               element.usage == kD3DDeclUsageColor &&
               element.usageIndex == 0u &&
               element.type == kD3DDeclTypeD3DColor) {
      out.hasDiffuse = true;
      out.diffuseOffset = element.offset;
    } else if (!out.hasTexcoord0 &&
               element.usage == kD3DDeclUsageTexcoord &&
               element.usageIndex == 0u &&
               floatComponents != 0u) {
      out.hasTexcoord0 = true;
      out.texcoord0Offset = element.offset;
      out.texcoord0Components = floatComponents;
    }
  }
  return out;
}

}  // namespace

void traceEffectIndexedGeometry(const ActiveEncoderBreakdown* encoderBreakdown,
                                core::FlatDrawStateView drawState,
                                const resources::Pool& pool,
                                std::span<const u8> indexBytes,
                                std::span<const u8> vertexBytes,
                                IndexType indexType,
                                u32 startIndex,
                                u64 indexCount,
                                i32 baseVertexIndex,
                                u64 stream0Offset,
                                u64 stream0Stride,
                                u64 stream0Handle,
                                u64 indexBufferHandle,
                                u64 seqId,
                                u64 drawOrdinal,
                                std::uint32_t commandIndex,
                                u64 commandDrawIndex,
                                u64 commandDrawCount,
                                core::PrimitiveType primitiveType,
                                u32 primitiveCount,
                                bool fixedFunctionPath,
                                bool preTransformed,
                                u64 vertexShaderHash,
                                u64 pixelShaderHash) {
  if (!effectDrawTraceEnabled() || !effectDrawTraceGeometryEnabled() ||
      !drawState.hot || indexBytes.empty() || vertexBytes.empty() ||
      stream0Stride == 0u) {
    return;
  }

  const auto& hot = *drawState.hot;
  const auto encoderIndex = encoderBreakdown ? encoderBreakdown->stats.encoderIndex : 0ull;
  if (!effectDrawTraceSeqMatches(seqId)) {
    return;
  }
  if (const auto encFilter = effectDrawTraceEncoderFilter();
      encFilter.has_value() && *encFilter != encoderIndex) {
    return;
  }
  const bool alphaBlendEnabled =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  if (!alphaBlendEnabled && !effectDrawTraceIncludeNonAlpha()) {
    return;
  }
  if (hot.textureMask == 0u && !effectDrawTraceIncludeUntextured()) {
    return;
  }
  const bool pointSpriteState =
      core::flatStateOr(hot.renderStates, RS_POINT_SPRITE_ENABLE, 0u) != 0u;
  const bool pointSpriteCandidate =
      pointSpriteState && primitiveType == core::PrimitiveType::PointList;
  if (effectDrawTracePointSpriteOnly() && !pointSpriteCandidate) {
    return;
  }
  if (const auto primitiveTypeFilter = effectDrawTracePrimitiveTypeFilter();
      primitiveTypeFilter.has_value() &&
      static_cast<u64>(primitiveType) != *primitiveTypeFilter) {
    return;
  }

  const resources::TextureRecord* texture0 =
      hot.textures[0] ? pool.findTexture(hot.textures[0].value) : nullptr;
  if (const auto handleFilter = effectDrawTraceTexture0Filter();
      handleFilter.has_value() &&
      (!hot.textures[0] || hot.textures[0].value != *handleFilter)) {
    return;
  }
  if (const auto widthFilter = effectDrawTraceTexture0WidthFilter();
      widthFilter.has_value() && (!texture0 || texture0->desc.width != *widthFilter)) {
    return;
  }
  if (const auto heightFilter = effectDrawTraceTexture0HeightFilter();
      heightFilter.has_value() && (!texture0 || texture0->desc.height != *heightFilter)) {
    return;
  }
  if (const auto formatFilter = effectDrawTraceTexture0FormatFilter();
      formatFilter.has_value() &&
      (!texture0 || static_cast<u64>(texture0->desc.format) != *formatFilter)) {
    return;
  }

  const auto layout = resolveEffectTraceStream0Layout(drawState);
  const auto encoderDrawIndex =
      encoderBreakdown ? encoderBreakdown->stats.drawCalls + 1ull : 0ull;
  if (!layout.hasPosition) {
    std::ostringstream out;
    out << "[dxmt9-effect-geometry"
        << " seq=" << static_cast<unsigned long long>(seqId)
        << " encoder=" << static_cast<unsigned long long>(encoderIndex)
        << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
        << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
        << " command_index=" << commandIndex
        << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
        << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
        << " layout=missing-position"
        << " primitive_type=" << static_cast<unsigned>(primitiveType)
        << " primitive_count=" << primitiveCount
        << " index_count=" << static_cast<unsigned long long>(indexCount)
        << " texture0=0x" << std::hex << hot.textures[0].value
        << " vs_hash=0x" << vertexShaderHash
        << " ps_hash=0x" << pixelShaderHash
        << std::dec << ']';
    emitQueueTraceLine(out.str());
    return;
  }

  auto readF32 = [&](u64 absoluteOffset) -> std::optional<float> {
    if (absoluteOffset > static_cast<u64>(vertexBytes.size()) ||
        sizeof(float) > static_cast<u64>(vertexBytes.size()) - absoluteOffset) {
      return std::nullopt;
    }
    float value = 0.0f;
    std::memcpy(&value,
                vertexBytes.data() + static_cast<std::size_t>(absoluteOffset),
                sizeof(value));
    return value;
  };
  auto readU32 = [&](u64 absoluteOffset) -> std::optional<u32> {
    if (absoluteOffset > static_cast<u64>(vertexBytes.size()) ||
        sizeof(u32) > static_cast<u64>(vertexBytes.size()) - absoluteOffset) {
      return std::nullopt;
    }
    u32 value = 0;
    std::memcpy(&value,
                vertexBytes.data() + static_cast<std::size_t>(absoluteOffset),
                sizeof(value));
    return value;
  };

  const u64 maxRefs = effectDrawTraceGeometryMaxRefs();
  const u64 refsToSample = std::min(indexCount, maxRefs);
  u64 readableIndices = 0;
  u64 validVertices = 0;
  u64 invalidRefs = 0;
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float minZ = std::numeric_limits<float>::infinity();
  float minW = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  float maxZ = -std::numeric_limits<float>::infinity();
  float maxW = -std::numeric_limits<float>::infinity();
  float minU = std::numeric_limits<float>::infinity();
  float minV = std::numeric_limits<float>::infinity();
  float maxU = -std::numeric_limits<float>::infinity();
  float maxV = -std::numeric_limits<float>::infinity();
  u32 minAlpha = std::numeric_limits<u32>::max();
  u32 maxAlpha = 0u;
  bool hasUv = false;
  bool hasAlpha = false;
  const bool canProject =
      drawState.hasUniformPayload() &&
      !fixedFunctionPath &&
      !(preTransformed || layout.preTransformed);
  std::optional<VsConsts> projectVsConsts;
  std::optional<PsConsts> projectPsConsts;
  std::optional<FfpVsConsts> projectFfpVsConsts;
  if (canProject) {
    projectVsConsts = buildVsConsts(drawState);
    projectPsConsts = buildPsConsts(drawState);
    projectFfpVsConsts = buildFfpVsConsts(drawState);
  }
  u64 projectedRefs = 0;
  u64 projectedVisibleRefs = 0;
  u64 projectedInsideXyRefs = 0;
  u64 projectedZInsideRefs = 0;
  u64 projectedBehindRefs = 0;
  float minClipX = std::numeric_limits<float>::infinity();
  float minClipY = std::numeric_limits<float>::infinity();
  float minClipZ = std::numeric_limits<float>::infinity();
  float minClipW = std::numeric_limits<float>::infinity();
  float maxClipX = -std::numeric_limits<float>::infinity();
  float maxClipY = -std::numeric_limits<float>::infinity();
  float maxClipZ = -std::numeric_limits<float>::infinity();
  float maxClipW = -std::numeric_limits<float>::infinity();
  float minNdcX = std::numeric_limits<float>::infinity();
  float minNdcY = std::numeric_limits<float>::infinity();
  float minNdcZ = std::numeric_limits<float>::infinity();
  float maxNdcX = -std::numeric_limits<float>::infinity();
  float maxNdcY = -std::numeric_limits<float>::infinity();
  float maxNdcZ = -std::numeric_limits<float>::infinity();
  float minScreenX = std::numeric_limits<float>::infinity();
  float minScreenY = std::numeric_limits<float>::infinity();
  float maxScreenX = -std::numeric_limits<float>::infinity();
  float maxScreenY = -std::numeric_limits<float>::infinity();
  std::ostringstream refs;
  refs << std::setprecision(6);
  u32 refsLogged = 0u;

  for (u64 i = 0; i < refsToSample; ++i) {
    const auto indexValue = readEffectTraceIndexValue(
        indexBytes, indexType, static_cast<std::size_t>(startIndex) +
                                   static_cast<std::size_t>(i));
    if (!indexValue.has_value()) {
      ++invalidRefs;
      continue;
    }
    ++readableIndices;
    const auto effectiveVertex =
        static_cast<std::int64_t>(baseVertexIndex) +
        static_cast<std::int64_t>(*indexValue);
    if (effectiveVertex < 0) {
      ++invalidRefs;
      continue;
    }
    const auto vertexIndex64 = static_cast<u64>(effectiveVertex);
    if (vertexIndex64 >
        (std::numeric_limits<u64>::max() - stream0Offset) / stream0Stride) {
      ++invalidRefs;
      continue;
    }
    const u64 vertexBase = stream0Offset + vertexIndex64 * stream0Stride;
    const auto x = readF32(vertexBase + layout.positionOffset);
    const auto y = readF32(vertexBase + layout.positionOffset + sizeof(float));
    if (!x.has_value() || !y.has_value() ||
        !std::isfinite(*x) || !std::isfinite(*y)) {
      ++invalidRefs;
      continue;
    }
    const auto zValue = layout.positionComponents >= 3u
        ? readF32(vertexBase + layout.positionOffset + sizeof(float) * 2u)
        : std::optional<float>(0.0f);
    const auto wValue = layout.positionComponents >= 4u
        ? readF32(vertexBase + layout.positionOffset + sizeof(float) * 3u)
        : std::optional<float>(1.0f);
    const float z = zValue.value_or(0.0f);
    const float w = wValue.value_or(1.0f);
    if (!std::isfinite(z) || !std::isfinite(w)) {
      ++invalidRefs;
      continue;
    }
    ++validVertices;
    minX = std::min(minX, *x);
    minY = std::min(minY, *y);
    minZ = std::min(minZ, z);
    minW = std::min(minW, w);
    maxX = std::max(maxX, *x);
    maxY = std::max(maxY, *y);
    maxZ = std::max(maxZ, z);
    maxW = std::max(maxW, w);

    if (projectVsConsts && projectFfpVsConsts) {
      const std::array<float, 4> vertex{*x, *y, z, w};
      auto dotConst = [&](std::size_t row) {
        const auto& c = projectVsConsts->vsFloatConst[row];
        return c[0] * vertex[0] + c[1] * vertex[1] +
               c[2] * vertex[2] + c[3] * vertex[3];
      };
      float clipX = dotConst(0);
      float clipY = dotConst(1);
      const float clipZ = dotConst(2);
      const float clipW = dotConst(3);
      clipX += projectFfpVsConsts->halfPixelFixup[0] * clipW;
      clipY += projectFfpVsConsts->halfPixelFixup[1] * clipW;
      if (std::isfinite(clipX) && std::isfinite(clipY) &&
          std::isfinite(clipZ) && std::isfinite(clipW) && clipW != 0.0f) {
        ++projectedRefs;
        minClipX = std::min(minClipX, clipX);
        minClipY = std::min(minClipY, clipY);
        minClipZ = std::min(minClipZ, clipZ);
        minClipW = std::min(minClipW, clipW);
        maxClipX = std::max(maxClipX, clipX);
        maxClipY = std::max(maxClipY, clipY);
        maxClipZ = std::max(maxClipZ, clipZ);
        maxClipW = std::max(maxClipW, clipW);
        if (clipW <= 0.0f) {
          ++projectedBehindRefs;
        }
        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float ndcZ = clipZ / clipW;
        minNdcX = std::min(minNdcX, ndcX);
        minNdcY = std::min(minNdcY, ndcY);
        minNdcZ = std::min(minNdcZ, ndcZ);
        maxNdcX = std::max(maxNdcX, ndcX);
        maxNdcY = std::max(maxNdcY, ndcY);
        maxNdcZ = std::max(maxNdcZ, ndcZ);
        const bool insideXy =
            ndcX >= -1.0f && ndcX <= 1.0f &&
            ndcY >= -1.0f && ndcY <= 1.0f;
        const bool zInside = ndcZ >= 0.0f && ndcZ <= 1.0f;
        if (insideXy) {
          ++projectedInsideXyRefs;
        }
        if (zInside) {
          ++projectedZInsideRefs;
        }
        if (clipW > 0.0f && insideXy && zInside) {
          ++projectedVisibleRefs;
        }
        const float screenX =
            projectFfpVsConsts->viewportOrigin[0] +
            (ndcX * 0.5f + 0.5f) * projectFfpVsConsts->viewportSize[0];
        const float screenY =
            projectFfpVsConsts->viewportOrigin[1] +
            (0.5f - ndcY * 0.5f) * projectFfpVsConsts->viewportSize[1];
        if (std::isfinite(screenX) && std::isfinite(screenY)) {
          minScreenX = std::min(minScreenX, screenX);
          minScreenY = std::min(minScreenY, screenY);
          maxScreenX = std::max(maxScreenX, screenX);
          maxScreenY = std::max(maxScreenY, screenY);
        }
      }
    }

    std::optional<float> u;
    std::optional<float> v;
    if (layout.hasTexcoord0) {
      u = readF32(vertexBase + layout.texcoord0Offset);
      if (layout.texcoord0Components >= 2u) {
        v = readF32(vertexBase + layout.texcoord0Offset + sizeof(float));
      } else {
        v = 0.0f;
      }
      if (u.has_value() && v.has_value() &&
          std::isfinite(*u) && std::isfinite(*v)) {
        hasUv = true;
        minU = std::min(minU, *u);
        minV = std::min(minV, *v);
        maxU = std::max(maxU, *u);
        maxV = std::max(maxV, *v);
      }
    }

    std::optional<u32> color;
    if (layout.hasDiffuse) {
      color = readU32(vertexBase + layout.diffuseOffset);
      if (color.has_value()) {
        const u32 alpha = (*color >> 24u) & 0xffu;
        hasAlpha = true;
        minAlpha = std::min(minAlpha, alpha);
        maxAlpha = std::max(maxAlpha, alpha);
      }
    }

    if (refsLogged < 6u) {
      refs << " r" << refsLogged << "#" << *indexValue
           << "=(" << *x << "," << *y << "," << z << "," << w << ")";
      if (u.has_value() && v.has_value()) {
        refs << " uv=(" << *u << "," << *v << ")";
      }
      if (color.has_value()) {
        refs << " c=0x" << std::hex << *color << std::dec;
      }
      ++refsLogged;
    }
  }

  std::ostringstream out;
  out << std::setprecision(6)
      << "[dxmt9-effect-geometry"
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " encoder=" << static_cast<unsigned long long>(encoderIndex)
      << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " command_index=" << commandIndex
      << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
      << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
      << " primitive_type=" << static_cast<unsigned>(primitiveType)
      << " primitive_count=" << primitiveCount
      << " index_count=" << static_cast<unsigned long long>(indexCount)
      << " refs_sampled=" << static_cast<unsigned long long>(refsToSample)
      << " readable_indices=" << static_cast<unsigned long long>(readableIndices)
      << " valid_vertices=" << static_cast<unsigned long long>(validVertices)
      << " invalid_refs=" << static_cast<unsigned long long>(invalidRefs)
      << " complete=" << (refsToSample == indexCount ? 1u : 0u)
      << " indexed=1"
      << " ffp=" << (fixedFunctionPath ? 1u : 0u)
      << " pretransformed=" << (preTransformed || layout.preTransformed ? 1u : 0u)
      << " src_blend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
      << " dst_blend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
      << " blend_op=" << core::flatStateOr(hot.renderStates, RS_BLEND_OP, 0u)
      << " depth_enabled="
      << (core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u) != 0u ? 1u : 0u)
      << " depth_write="
      << (core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u ? 1u : 0u)
      << " texture0=0x" << std::hex << hot.textures[0].value
      << " vs_hash=0x" << vertexShaderHash
      << " ps_hash=0x" << pixelShaderHash
      << " stream0_handle=0x" << stream0Handle
      << " index_buffer=0x" << indexBufferHandle
      << std::dec
      << " texture0_width=" << (texture0 ? texture0->desc.width : 0u)
      << " texture0_height=" << (texture0 ? texture0->desc.height : 0u)
      << " texture0_format="
      << (texture0 ? static_cast<u32>(texture0->desc.format) : 0u)
      << " base_vertex=" << baseVertexIndex
      << " start_index=" << startIndex
      << " stream0_offset=" << static_cast<unsigned long long>(stream0Offset)
      << " stream0_stride=" << static_cast<unsigned long long>(stream0Stride)
      << " position_components=" << layout.positionComponents
      << " texcoord0_components=" << layout.texcoord0Components;
  if (validVertices != 0u) {
    out << " pos_min=(" << minX << "," << minY << "," << minZ << "," << minW << ")"
        << " pos_max=(" << maxX << "," << maxY << "," << maxZ << "," << maxW << ")";
  }
  if (hasUv) {
    out << " uv_min=(" << minU << "," << minV << ")"
        << " uv_max=(" << maxU << "," << maxV << ")";
  }
  if (hasAlpha) {
    out << " alpha_min=" << minAlpha
        << " alpha_max=" << maxAlpha;
  }
  if (projectVsConsts && projectFfpVsConsts) {
    out << " project=1"
        << " projected_refs=" << static_cast<unsigned long long>(projectedRefs)
        << " projected_visible_refs="
        << static_cast<unsigned long long>(projectedVisibleRefs)
        << " projected_inside_xy_refs="
        << static_cast<unsigned long long>(projectedInsideXyRefs)
        << " projected_z_inside_refs="
        << static_cast<unsigned long long>(projectedZInsideRefs)
        << " projected_behind_refs="
        << static_cast<unsigned long long>(projectedBehindRefs)
        << " half_pixel=(" << projectFfpVsConsts->halfPixelFixup[0] << ","
        << projectFfpVsConsts->halfPixelFixup[1] << ")"
        << " viewport_origin=(" << projectFfpVsConsts->viewportOrigin[0] << ","
        << projectFfpVsConsts->viewportOrigin[1] << ")"
        << " viewport_size=(" << projectFfpVsConsts->viewportSize[0] << ","
        << projectFfpVsConsts->viewportSize[1] << ")";
    if (projectPsConsts) {
      const auto& ps0 = projectPsConsts->psFloatConst[0];
      out << " ps0=(" << ps0[0] << "," << ps0[1] << "," << ps0[2] << ","
          << ps0[3] << ")";
    }
    if (projectedRefs != 0u) {
      out << " clip_min=(" << minClipX << "," << minClipY << "," << minClipZ
          << "," << minClipW << ")"
          << " clip_max=(" << maxClipX << "," << maxClipY << "," << maxClipZ
          << "," << maxClipW << ")"
          << " ndc_min=(" << minNdcX << "," << minNdcY << "," << minNdcZ
          << ")"
          << " ndc_max=(" << maxNdcX << "," << maxNdcY << "," << maxNdcZ
          << ")"
          << " screen_min=(" << minScreenX << "," << minScreenY << ")"
          << " screen_max=(" << maxScreenX << "," << maxScreenY << ")";
    }
  } else {
    out << " project=0";
  }
  out << refs.str() << ']';
  emitQueueTraceLine(out.str());
}


}  // namespace dxmt9::encoders
