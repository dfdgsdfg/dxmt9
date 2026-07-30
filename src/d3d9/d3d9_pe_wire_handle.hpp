#pragma once

// The PE-side object-pointer <-> D9CWireHandle encoding.
//
// A wire handle is just the object pointer split into two 32-bit halves, so
// this pair has no Windows or COM dependency of its own. It used to sit in
// d3d9_pe_recorder.hpp, which includes d3d9_pe.hpp (windows.h + d3d9.h) and is
// therefore not natively buildable; d3d9_pe_producer.cpp needs the same
// encoding and must compile without Wine. Splitting the two helpers out is
// preferable to duplicating them: a second copy of the encoding is exactly the
// kind of thing that drifts and then mismatches silently on the wire.
//
// d3d9_pe_recorder.hpp includes this header, so every existing caller is
// unaffected.

#include "dxmt9/device_c.h"

#include <cstdint>

inline D9CWireHandle toWireHandle(const void* handle) {
    const auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(handle));
    return D9CWireHandle{
        static_cast<std::uint32_t>(value & 0xffffffffull),
        static_cast<std::uint32_t>(value >> 32),
    };
}

inline std::uint64_t d9cWireHandleValue(const D9CWireHandle& handle) {
    return static_cast<std::uint64_t>(handle.lo) |
           (static_cast<std::uint64_t>(handle.hi) << 32);
}
