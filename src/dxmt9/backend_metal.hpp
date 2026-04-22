#pragma once

// Internal: build the Metal-backed BackendDevice. Not part of the public API —
// consumed only by src/dxmt9/dxmt9_device.cpp. Takes an externally-chosen WMT
// device so the upper runtime can expose that device to the COM layer.

#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "../winemetal/Metal.hpp"

#include <memory>
#include <string>

namespace dxmt9::core {

std::shared_ptr<BackendDevice> makeMetalBackendDevice(const BackendLimits& limits,
                                                      WMT::Reference<WMT::Device> wmtDevice,
                                                      dxmt9::CommandQueue& commandQueue,
                                                      WMT::Reference<WMT::BinaryArchive>& shaderArchive,
                                                      const std::string& shaderArchivePath,
                                                      dxmt9::Device& upperDevice,
                                                      dxmt9::resources::Pool& pool,
                                                      dxmt9::pipeline::Cache& pipelineCache,
                                                      dxmt9::scratch::FrameAllocators& allocators);

}  // namespace dxmt9::core
