#pragma once

#include <cstdint>

namespace dxmt9::encoders {

enum class SessionFinalizeCause : std::uint8_t {
  SessionCap,
  Independent,
  Initializer,
  ProducerWait,
  Drain,
  FailOrOther,
};

}  // namespace dxmt9::encoders
