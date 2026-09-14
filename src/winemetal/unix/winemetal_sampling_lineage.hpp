#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dxmt9::diagnostics {

// All storage and serialization are cold. No execution payload/ABI changes.
inline bool metalSamplingLineageEnabled() noexcept {
  static const bool enabled = [] {
    for (const char* name : {"DXMT9_EFFECT_DRAW_TRACE",
                            "DXMT9_EFFECT_DRAW_TRACE_BINDINGS",
                            "DXMT9_EFFECT_DRAW_TRACE_GEOMETRY"}) {
      const char* value = std::getenv(name);
      if (!value || !value[0] || std::strcmp(value, "0") == 0) return false;
    }
    return true;
  }();
  return enabled;
}

void noteMetalSamplerCreation(std::uint64_t sampler, const void* descriptor) noexcept;
// Process-local, non-reused token attached to the actual texture object.
// Zero means unavailable. Not a cross-process or Tape resource identity.
std::uint64_t metalTextureObservationId(std::uint64_t texture) noexcept;
void noteMetalFragmentTexture(std::uint64_t encoder, std::uint64_t texture,
                              std::uint32_t index) noexcept;
void noteMetalFragmentSampler(std::uint64_t encoder, std::uint64_t sampler,
                              std::uint32_t index) noexcept;
void noteMetalFragmentTable(std::uint64_t encoder, std::uint64_t buffer,
                            std::uint64_t offset, bool offsetOnly = false) noexcept;
void noteMetalArgumentTable(std::uint64_t encoder, std::uint64_t buffer,
                            std::uint64_t offset) noexcept;
void prepareMetalArgumentWrite(std::uint64_t encoder) noexcept;
// kind: 0 = buffer write (invalidate overlapping provenance), 1 = texture,
// 2 = sampler. Called after the actual Metal argument write.
void noteMetalArgumentWrite(std::uint64_t encoder, std::uint64_t resource,
                            std::uint32_t index, unsigned kind) noexcept;

// Snapshot actual issued descriptor inputs. Does not prove texture contents,
// shader ABI selection, residency, GPU reads, or cross-process object identity.
std::string metalSamplingBindingsJson(std::uint64_t encoder) noexcept;

// Actual depth/stencil descriptor and reference inputs, under the same cold
// binding/geometry gate. Unknown creation must not become a desired-state guess.
void noteMetalDepthStencilCreation(std::uint64_t state, const void* descriptor) noexcept;
void noteMetalDepthStencilBinding(std::uint64_t encoder, std::uint64_t state) noexcept;
void noteMetalStencilReference(std::uint64_t encoder, std::uint32_t reference) noexcept;
std::string metalDepthStencilBindingsJson(std::uint64_t encoder) noexcept;

// Direct uniform slots 0/3 only. Observe actual calls independently of the
// encoder's high-level binding cache. Associations never retain Metal buffers.
void noteMetalDirectBuffer(std::uint64_t encoder, bool vertex, std::uint32_t index,
                           std::uint64_t buffer, std::uint64_t offset,
                           bool offsetOnly = false) noexcept;
void noteMetalDirectBytes(std::uint64_t encoder, bool vertex, std::uint32_t index) noexcept;
const char* metalDirectBufferMatch(std::uint64_t encoder, bool vertex, std::uint32_t index,
                                   std::uint64_t expectedBuffer, std::uint64_t expectedOffset) noexcept;

}  // namespace dxmt9::diagnostics
