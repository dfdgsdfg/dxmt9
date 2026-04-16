#pragma once

#import <Foundation/Foundation.h>

#include "dxmt9_backend_types.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9_queue.hpp"

#include <functional>
#include <string>
#include <vector>

namespace dxmt9::core::metalhud {

enum CompatFlagBits : u32 {
  CompatFlagFp16 = 1u << 0,
  CompatFlagMrt = 1u << 1,
  CompatFlagSrgb = 1u << 2,
  CompatFlagProjected = 1u << 3,
  CompatFlagMsaa = 1u << 4,
  CompatFlagQuery = 1u << 5,
};

bool compatHudEnabled();
bool isFloatRenderTargetFormat(Format format);
bool matrixIsIdentity(const Matrix4x4& matrix);
std::string formatCompatFlags(u32 flags);

class DeveloperHudState {
 public:
  DeveloperHudState();
  ~DeveloperHudState();

  void update(u32 frame, u64 seqId, u32 flags, const std::string& errorSummary);

 private:
  bool ensureInitialized();
  void addLabel(const char* label, const char* after);
  void updateLine(size_t index, const std::string& value);

  bool initialized_ = false;
  bool available_ = false;
  id hud_ = nil;
  std::vector<NSString*> labels_{};
};

class DeveloperHudController {
 public:
  struct CompatFlagResolver {
    std::function<u32(const DrawDesc&)> draw;
    std::function<u32(const ClearDesc&)> clear;
    std::function<u32(const SwapDesc&, Handle)> present;
  };

  template <typename CommandContainer>
  metalqueue::CommandBufferDiagnostics prepareForSubmission(u64 seqId,
                                                            size_t slotIndex,
                                                            const CommandContainer& commands,
                                                            const CompatFlagResolver& resolver) {
    std::vector<metalqueue::ChunkObservation> observations;
    observations.reserve(commands.size());
    for (const auto& command : commands) {
      switch (command.kind) {
        case MetalCommandRecord::Kind::Draw:
          observations.push_back(metalqueue::ChunkObservation{
              .kind = metalqueue::ChunkObservationKind::Draw,
              .compatFlags = resolver.draw ? resolver.draw(command.draw) : 0,
          });
          break;
        case MetalCommandRecord::Kind::Clear:
          observations.push_back(metalqueue::ChunkObservation{
              .kind = metalqueue::ChunkObservationKind::Draw,
              .compatFlags = resolver.clear ? resolver.clear(command.clear) : 0,
          });
          break;
        case MetalCommandRecord::Kind::SurfaceCopy:
        case MetalCommandRecord::Kind::StretchRect:
        case MetalCommandRecord::Kind::Readback:
          observations.push_back(metalqueue::ChunkObservation{
              .kind = metalqueue::ChunkObservationKind::Blit,
              .compatFlags = 0,
          });
          break;
        case MetalCommandRecord::Kind::ColorFill:
          observations.push_back(metalqueue::ChunkObservation{
              .kind = metalqueue::ChunkObservationKind::Draw,
              .compatFlags = 0,
          });
          break;
        case MetalCommandRecord::Kind::Present:
          observations.push_back(metalqueue::ChunkObservation{
              .kind = metalqueue::ChunkObservationKind::Present,
              .compatFlags = resolver.present ? resolver.present(command.present, command.presentSource) : 0,
          });
          break;
      }
    }
    return prepareForSubmission(metalqueue::summarizeChunk(
        seqId, slotIndex, std::span<const metalqueue::ChunkObservation>(observations.data(), observations.size())));
  }

  metalqueue::CommandBufferDiagnostics prepareForSubmission(metalqueue::CommandBufferDiagnostics diagnostics);
  bool observeCompletion(id<MTLCommandBuffer> commandBuffer,
                         const metalqueue::CommandBufferDiagnostics& diagnostics,
                         metalqueue::CompletionTracker& completionTracker,
                         const char* context = "queue");
  void completeSubmission(const metalqueue::CommandBufferDiagnostics& diagnostics,
                          const metalqueue::CompletionTracker& completionTracker);

 private:
  DeveloperHudState state_{};
  u32 presentedFrame_ = 0;
  u32 lastCompatFlags_ = 0;
};

}  // namespace dxmt9::core::metalhud
