#pragma once

#include <cstdio>
#include <cstdlib>

namespace dxmt9::core::detail {

[[noreturn]] inline void assertFail(const char* expression, const char* file, int line,
                                    const char* function) {
  std::fprintf(stderr, "DXMT_ASSERT failed: %s (%s:%d in %s)\n", expression, file, line, function);
  std::abort();
}

}  // namespace dxmt9::core::detail

#ifndef NDEBUG
#define DXMT_ASSERT(expr)                                                             \
  do {                                                                                \
    if (!(expr)) {                                                                    \
      ::dxmt9::core::detail::assertFail(#expr, __FILE__, __LINE__, __func__);         \
    }                                                                                 \
  } while (false)
#else
#define DXMT_ASSERT(expr) ((void)0)
#endif
