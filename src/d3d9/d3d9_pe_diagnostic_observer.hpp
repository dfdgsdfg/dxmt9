#pragma once

#include <cstdint>

class D3D9DeviceImpl;

// Opaque register-sized handle to one entry sample of the PE call-tracking
// diagnostic. The diagnostic owns the sample; child wrappers only keep this
// slot for the duration of one synchronous COM call.
using D3D9PePresentCallSlot = std::uint32_t;
inline constexpr D3D9PePresentCallSlot kD3D9PePresentCallSlotNone =
    static_cast<D3D9PePresentCallSlot>(-1);

// Concrete, non-owning observer used only when DXMT9_PE_RECORDER_STATS is on.
// Child wrappers cache a nullable pointer at construction, so the disabled
// path performs one null test and never dispatches through D3D9PeRecorderFlush.
class D3D9PeDiagnosticObserver {
public:
  explicit D3D9PeDiagnosticObserver(D3D9DeviceImpl *device = nullptr) noexcept
      : device_(device) {}

  void notifyFirstCallAfterPresent(const char *callName,
                                   const void *callerPc = nullptr) noexcept;
  D3D9PePresentCallSlot pushCallScope(const char *callName,
                                     const void *callerPc) noexcept;
  void notifyCallScopeReturn(D3D9PePresentCallSlot slot, const char *callName,
                             std::int32_t hr) noexcept;
  void popCallScope(D3D9PePresentCallSlot slot) noexcept;
  void notifyStateBlockFault(bool entered, std::int32_t hr) noexcept;

private:
  D3D9DeviceImpl *device_ = nullptr;
};
