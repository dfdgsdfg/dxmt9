#pragma once

#include <cstdint>
#include <thread>

namespace dxmt9::util {

using thread = std::thread;

std::uint32_t hardwareConcurrency();

namespace this_thread {

void yield();
std::uint32_t get_id();

}  // namespace this_thread

}  // namespace dxmt9::util
