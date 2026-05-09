#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_encoder_internal.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

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

}  // namespace dxmt9::encoders
