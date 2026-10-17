#pragma once

#include "dxmt9_trace.hpp"
#include <cstdint>

namespace dxmt9::tracing {

// Cold object-lifetime hooks. Tokens are never addresses and are never reused.
void register_owner(const void* owner) noexcept;
void unregister_owner(const void* owner) noexcept;
void enable_owner_registration(uint64_t generation) noexcept;
void disable_owner_registration() noexcept;
uint64_t owner_id(const void* owner) noexcept;
uint64_t next_observation_id() noexcept;

// Call only behind enabled(): all diagnostic payload construction stays here.
void observe(Category category, EventId event, uint64_t owner, uint64_t work,
             uint64_t related = 0, Phase phase = Phase::Instant,
             uint64_t related_owner = 0) noexcept;

} // namespace dxmt9::tracing
