#include "thread.hpp"

#include <functional>

namespace dxmt9::util {

std::uint32_t hardwareConcurrency() {
  return std::thread::hardware_concurrency();
}

namespace this_thread {

void yield() {
  std::this_thread::yield();
}

std::uint32_t get_id() {
  return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

}  // namespace this_thread

}  // namespace dxmt9::util
