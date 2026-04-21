#pragma once

// Internal: build the Metal-backed BackendDevice. Not part of the public API —
// consumed only by src/dxmt9/dxmt9_device.cpp. Takes an externally-chosen WMT
// device so the upper runtime can expose that device to the COM layer.

#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "../winemetal/Metal.hpp"

#include <future>
#include <memory>
#include <string>

namespace dxmt9::core {

std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits,
                                                      WMT::Reference<WMT::Device> wmtDevice,
                                                      dxmt9::CommandQueue& commandQueue,
                                                      WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                                                      const std::string& shaderArchivePath,
                                                      dxmt9::Device& upperDevice);

// Build a textured blit (present) pipeline on a background task. Exposed so
// dxmt9::Presenter can own its own pipeline cache, matching dxmt's Presenter.
std::shared_future<WMT::Reference<WMT::RenderPipelineState>>
buildPresentPipeline(WMT::Reference<WMT::Device> device, bool opaqueAlpha,
                     WMT::Reference<WMT::BinaryArchive>* archive,
                     const std::string* archivePath);

}  // namespace dxmt9::core
