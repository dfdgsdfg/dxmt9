#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "d3d9_constant_economy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dxmt9::conformance {
namespace {

constexpr UINT kStartRegister = 8;
constexpr UINT kMaterializedStart = 7;
constexpr UINT kMaterializedCount = 6;
constexpr std::uint32_t kMeasuredCalls = 50'000'000;
constexpr std::uint32_t kWarmupCalls = 10'000;

enum class Family : unsigned {
  VsFloat,
  PsFloat,
  VsInt,
  PsInt,
  VsBool,
  PsBool,
};

enum class Shape : unsigned {
  Equal,
  Last,
  All,
  Middle,
};

const char *family_name(Family family) {
  switch (family) {
    case Family::VsFloat: return "vs_float";
    case Family::PsFloat: return "ps_float";
    case Family::VsInt: return "vs_int";
    case Family::PsInt: return "ps_int";
    case Family::VsBool: return "vs_bool";
    case Family::PsBool: return "ps_bool";
  }
  return "unknown";
}

const char *shape_name(Shape shape) {
  switch (shape) {
    case Shape::Equal: return "equal";
    case Shape::Last: return "last";
    case Shape::All: return "all";
    case Shape::Middle: return "middle";
  }
  return "unknown";
}

template <Family family>
struct FamilyOps;

template <>
struct FamilyOps<Family::VsFloat> {
  using Value = float;
  static constexpr std::size_t kWidth = 4;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetVertexShaderConstantF(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetVertexShaderConstantF(start, data, count);
  }
};

template <>
struct FamilyOps<Family::PsFloat> {
  using Value = float;
  static constexpr std::size_t kWidth = 4;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetPixelShaderConstantF(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetPixelShaderConstantF(start, data, count);
  }
};

template <>
struct FamilyOps<Family::VsInt> {
  using Value = int;
  static constexpr std::size_t kWidth = 4;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetVertexShaderConstantI(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetVertexShaderConstantI(start, data, count);
  }
};

template <>
struct FamilyOps<Family::PsInt> {
  using Value = int;
  static constexpr std::size_t kWidth = 4;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetPixelShaderConstantI(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetPixelShaderConstantI(start, data, count);
  }
};

template <>
struct FamilyOps<Family::VsBool> {
  using Value = BOOL;
  static constexpr std::size_t kWidth = 1;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetVertexShaderConstantB(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetVertexShaderConstantB(start, data, count);
  }
};

template <>
struct FamilyOps<Family::PsBool> {
  using Value = BOOL;
  static constexpr std::size_t kWidth = 1;
  static HRESULT set(IDirect3DDevice9 *device, UINT start, const Value *data,
                     UINT count) {
    return device->SetPixelShaderConstantB(start, data, count);
  }
  static HRESULT get(IDirect3DDevice9 *device, UINT start, Value *data,
                     UINT count) {
    return device->GetPixelShaderConstantB(start, data, count);
  }
};

template <Family family>
using FamilyValue = typename FamilyOps<family>::Value;

template <Family family>
using RegisterValues = std::array<FamilyValue<family>,
    kMaterializedCount * FamilyOps<family>::kWidth>;

template <Family family>
FamilyValue<family> base_value(unsigned register_index, unsigned component) {
  using Value = FamilyValue<family>;
  if constexpr (family == Family::VsFloat || family == Family::PsFloat) {
    const float family_bias = family == Family::VsFloat ? 0.0f : 0.5f;
    return static_cast<Value>(0.125f + family_bias +
        static_cast<float>(register_index) * 0.03125f +
        static_cast<float>(component) * 0.0078125f);
  } else if constexpr (family == Family::VsInt || family == Family::PsInt) {
    const int family_bias = family == Family::VsInt ? 0 : 500;
    return static_cast<Value>(1000 + family_bias +
        static_cast<int>(register_index) * 37 + static_cast<int>(component));
  } else {
    return static_cast<Value>((register_index + component) & 1u ? TRUE : FALSE);
  }
}

template <Family family>
FamilyValue<family> changed_value(FamilyValue<family> value) {
  using Value = FamilyValue<family>;
  if constexpr (family == Family::VsFloat || family == Family::PsFloat) {
    return static_cast<Value>(value + 0.375f);
  } else if constexpr (family == Family::VsInt || family == Family::PsInt) {
    return static_cast<Value>(value + 701);
  } else {
    return value == FALSE ? TRUE : FALSE;
  }
}

template <Family family>
struct CellBuffers {
  RegisterValues<family> a{};
  RegisterValues<family> b{};
};

template <Family family>
CellBuffers<family> make_buffers(unsigned count, Shape shape) {
  CellBuffers<family> buffers;
  for (unsigned register_index = 0; register_index < kMaterializedCount;
       ++register_index) {
    for (std::size_t component = 0; component < FamilyOps<family>::kWidth;
         ++component) {
      const std::size_t index =
          register_index * FamilyOps<family>::kWidth + component;
      buffers.a[index] = base_value<family>(register_index, component);
      buffers.b[index] = buffers.a[index];
    }
  }
  if (shape == Shape::Equal) return buffers;

  const unsigned changed_register = 1u +
      (shape == Shape::Last ? count - 1u : (shape == Shape::Middle ? 1u : 0u));
  const unsigned first = shape == Shape::All ? 1u : changed_register;
  const unsigned last = shape == Shape::All ? 1u + count : changed_register + 1u;
  for (unsigned register_index = first; register_index < last;
       ++register_index) {
    for (std::size_t component = 0; component < FamilyOps<family>::kWidth;
         ++component) {
      const std::size_t index =
          register_index * FamilyOps<family>::kWidth + component;
      buffers.b[index] = changed_value<family>(buffers.a[index]);
    }
  }
  return buffers;
}

template <Family family>
HRESULT accumulate(HRESULT current, HRESULT next) {
  return static_cast<HRESULT>(static_cast<std::uint32_t>(current) |
      static_cast<std::uint32_t>(next));
}

std::uint64_t filetime_100ns(const FILETIME &time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) |
      static_cast<std::uint64_t>(time.dwLowDateTime);
}

template <Family family>
bool read_and_check(IDirect3DDevice9 *device,
                    const RegisterValues<family> &expected) {
  RegisterValues<family> actual{};
  const HRESULT hr = FamilyOps<family>::get(device, kMaterializedStart,
      actual.data(), kMaterializedCount);
  return hr == D3D_OK && std::memcmp(actual.data(), expected.data(),
      sizeof(actual)) == 0;
}

template <Family family>
bool sources_match(const CellBuffers<family> &buffers,
                   const RegisterValues<family> &a_snapshot,
                   const RegisterValues<family> &b_snapshot) {
  return std::memcmp(buffers.a.data(), a_snapshot.data(),
             sizeof(buffers.a)) == 0 &&
      std::memcmp(buffers.b.data(), b_snapshot.data(),
          sizeof(buffers.b)) == 0;
}

template <Family family>
bool expected_shape(const CellBuffers<family> &buffers, unsigned count,
                    Shape shape) {
  using Value = FamilyValue<family>;
  const std::size_t register_bytes =
      FamilyOps<family>::kWidth * sizeof(Value);
  if (buffers.a.data() == buffers.b.data()) return false;
  for (unsigned materialized_register = 0;
       materialized_register < kMaterializedCount; ++materialized_register) {
    const bool differs = std::memcmp(
        buffers.a.data() + materialized_register * FamilyOps<family>::kWidth,
        buffers.b.data() + materialized_register * FamilyOps<family>::kWidth,
        register_bytes) != 0;
    bool expected_difference = false;
    if (shape == Shape::All) {
      expected_difference = materialized_register >= 1u &&
          materialized_register < 1u + count;
    } else if (shape == Shape::Last) {
      expected_difference = materialized_register == count;
    } else if (shape == Shape::Middle) {
      expected_difference = materialized_register == 2u;
    }
    if (differs != expected_difference) return false;
  }
  return true;
}

template <Family family>
bool run_cell(IDirect3DDevice9 *device, unsigned count, Shape shape) {
  using Ops = FamilyOps<family>;
  using Value = FamilyValue<family>;
  CellBuffers<family> buffers = make_buffers<family>(count, shape);
  const RegisterValues<family> a_snapshot = buffers.a;
  const RegisterValues<family> b_snapshot = buffers.b;
  const Value *a = buffers.a.data() + Ops::kWidth;
  const Value *b = buffers.b.data() + Ops::kWidth;

  if (!expected_shape<family>(buffers, count, shape)) {
    std::printf("FAIL:pe_const_economy family=%s count=%u shape=%s "
        "invalid pre-timer payload shape\n",
        family_name(family), count, shape_name(shape));
    return false;
  }

  HRESULT hr_or = S_OK;
  hr_or = accumulate<family>(hr_or, Ops::set(device, kMaterializedStart,
      buffers.a.data(), kMaterializedCount));
  hr_or = accumulate<family>(hr_or, Ops::set(device, kStartRegister, b, count));
  for (std::uint32_t i = 0; i < kWarmupCalls; ++i) {
    const Value *source = (i & 1u) == 0u ? a : b;
    hr_or = accumulate<family>(hr_or,
        Ops::set(device, kStartRegister, source, count));
  }

  const bool source_precheck = sources_match<family>(buffers, a_snapshot,
      b_snapshot);
  const bool precheck = source_precheck &&
      read_and_check<family>(device, b_snapshot);
  LARGE_INTEGER frequency = {};
  LARGE_INTEGER qpc_start = {};
  LARGE_INTEGER qpc_end = {};
  FILETIME creation = {};
  FILETIME exit = {};
  FILETIME kernel_start = {};
  FILETIME user_start = {};
  FILETIME kernel_end = {};
  FILETIME user_end = {};
  const DWORD thread_id = GetCurrentThreadId();
  const DWORD process_id = GetCurrentProcessId();
  const bool frequency_ok = QueryPerformanceFrequency(&frequency) != FALSE;
  // GetProcessTimes is the calibrated Windows timing scope here. It includes
  // harness/Wine/provider helper threads and is not PE-thread-exclusive CPU.
  const bool process_start_ok = GetProcessTimes(
      GetCurrentProcess(), &creation, &exit, &kernel_start, &user_start) != FALSE;
  const bool qpc_start_ok = QueryPerformanceCounter(&qpc_start) != FALSE;
  for (std::uint32_t i = 0; i < kMeasuredCalls; ++i) {
    const Value *source = (i & 1u) == 0u ? a : b;
    hr_or = accumulate<family>(hr_or,
        Ops::set(device, kStartRegister, source, count));
  }
  const bool qpc_end_ok = QueryPerformanceCounter(&qpc_end) != FALSE;
  const bool process_end_ok = GetProcessTimes(
      GetCurrentProcess(), &creation, &exit, &kernel_end, &user_end) != FALSE;
  const DWORD ending_thread_id = GetCurrentThreadId();
  const DWORD ending_process_id = GetCurrentProcessId();
  const bool postcheck = read_and_check<family>(device, b_snapshot);
  const bool source_unchanged = sources_match<family>(buffers, a_snapshot,
      b_snapshot);

  const std::uint64_t qpc_ticks =
      qpc_end.QuadPart > qpc_start.QuadPart
          ? static_cast<std::uint64_t>(qpc_end.QuadPart - qpc_start.QuadPart)
          : 0;
  const std::uint64_t qpc_frequency = frequency.QuadPart > 0
      ? static_cast<std::uint64_t>(frequency.QuadPart) : 0;
  const std::uint64_t kernel_start_100ns = filetime_100ns(kernel_start);
  const std::uint64_t kernel_end_100ns = filetime_100ns(kernel_end);
  const std::uint64_t user_start_100ns = filetime_100ns(user_start);
  const std::uint64_t user_end_100ns = filetime_100ns(user_end);
  const bool time_ok = frequency_ok && process_start_ok && qpc_start_ok &&
      qpc_end_ok && process_end_ok && qpc_ticks > 0 && qpc_frequency > 0 &&
      kernel_end_100ns >= kernel_start_100ns &&
      user_end_100ns >= user_start_100ns &&
      kernel_end_100ns - kernel_start_100ns +
          user_end_100ns - user_start_100ns > 0 &&
      thread_id == ending_thread_id && process_id == ending_process_id;
  const std::uint64_t kernel_100ns = kernel_end_100ns >= kernel_start_100ns
      ? kernel_end_100ns - kernel_start_100ns : 0;
  const std::uint64_t user_100ns = user_end_100ns >= user_start_100ns
      ? user_end_100ns - user_start_100ns : 0;

  const std::uint32_t hr_bits = static_cast<std::uint32_t>(hr_or);
  std::printf(
      "[dxmt9-pe-const-economy] family=%s count=%u shape=%s calls=%llu "
      "warmup=%llu start=%u cpu_scope=process thread_id=%llu process_id=%llu "
      "qpc_ticks=%llu "
      "qpc_frequency=%llu kernel_100ns=%llu user_100ns=%llu hr_or=%u "
      "precheck=%u postcheck=%u source_unchanged=%u\n",
      family_name(family), count, shape_name(shape),
      static_cast<unsigned long long>(kMeasuredCalls),
      static_cast<unsigned long long>(kWarmupCalls), kStartRegister,
      static_cast<unsigned long long>(thread_id),
      static_cast<unsigned long long>(process_id),
      static_cast<unsigned long long>(qpc_ticks),
      static_cast<unsigned long long>(qpc_frequency),
      static_cast<unsigned long long>(kernel_100ns),
      static_cast<unsigned long long>(user_100ns), hr_bits,
      precheck ? 1u : 0u, postcheck ? 1u : 0u,
      source_unchanged ? 1u : 0u);

  const bool valid = hr_bits == 0 && precheck && postcheck &&
      source_unchanged && time_ok;
  if (!valid) {
    std::printf("FAIL:pe_const_economy family=%s count=%u shape=%s "
        "hr_or=%u precheck=%u postcheck=%u source_unchanged=%u timing=%u\n",
        family_name(family), count, shape_name(shape), hr_bits,
        precheck ? 1u : 0u, postcheck ? 1u : 0u,
        source_unchanged ? 1u : 0u, time_ok ? 1u : 0u);
  }
  return valid;
}

template <Family family>
bool run_family(IDirect3DDevice9 *device) {
  bool ok = true;
  constexpr std::array<Shape, 3> kShapes = {
      Shape::Equal, Shape::Last, Shape::All};
  for (unsigned count : {1u, 3u, 4u}) {
    for (Shape shape : kShapes) {
      ok = run_cell<family>(device, count, shape) && ok;
    }
  }
  ok = run_cell<family>(device, 3u, Shape::Middle) && ok;
  ok = run_cell<family>(device, 0u, Shape::Equal) && ok;
  return ok;
}

}  // namespace

bool runConstantSetterEconomy(IDirect3DDevice9 *device) {
  if (!device) {
    std::printf("FAIL:pe_const_economy null device\n");
    return false;
  }
  bool ok = true;
  ok = run_family<Family::VsFloat>(device) && ok;
  ok = run_family<Family::PsFloat>(device) && ok;
  ok = run_family<Family::VsInt>(device) && ok;
  ok = run_family<Family::PsInt>(device) && ok;
  ok = run_family<Family::VsBool>(device) && ok;
  ok = run_family<Family::PsBool>(device) && ok;
  if (ok) std::printf("PASS:pe_const_economy cells=66\n");
  return ok;
}

}  // namespace dxmt9::conformance
