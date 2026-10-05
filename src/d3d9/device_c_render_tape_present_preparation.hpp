#pragma once

#include <cstdint>

struct D9CDevice;

namespace dxmt9::d3d9 {

// Shared production capture-Present preparation. Joins software replay and
// performs the original reservation flush without allocating a mirror,
// publishing initializer uploads explicitly, or changing observer cuts.
// Replay callers must admit the original producer contract independently of
// captured publication counts and invoke this before entering the Present raw.
[[nodiscard]] std::int32_t prepareRenderTapePresentReservation(
    D9CDevice* device) noexcept;

} // namespace dxmt9::d3d9
