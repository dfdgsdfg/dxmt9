#pragma once

#include "d3d9_pe_child_context.hpp"
#include "d3d9_pe_diagnostic_observer.hpp"

#include <type_traits>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#define DXMT9_PE_CALLSITE_PC() (__builtin_return_address(0))
#else
#define DXMT9_PE_CALLSITE_PC() nullptr
#endif

template<typename Context>
class D3D9PeChildOperationGuard final {
public:
  explicit D3D9PeChildOperationGuard(Context *context) noexcept
      : context_(context) {
    if (context_) context_->LockStateBlockOperationForChild();
  }
  ~D3D9PeChildOperationGuard() noexcept {
    if (context_) context_->UnlockStateBlockOperationForChild();
  }
  D3D9PeChildOperationGuard(const D3D9PeChildOperationGuard&) = delete;
  D3D9PeChildOperationGuard& operator=(const D3D9PeChildOperationGuard&) = delete;
private:
  Context *context_ = nullptr;
};

class D3D9PeChildCallScope {
public:
  D3D9PeChildCallScope(D3D9PeDiagnosticObserver &observer,
                       const char *callName, const void *callerPc) noexcept {
    const D3D9PePresentCallSlot slot = observer.pushCallScope(callName, callerPc);
    if (slot == kD3D9PePresentCallSlotNone) return;
    observer_ = &observer;
    slot_ = slot;
  }
  D3D9PeChildCallScope(const D3D9PeChildCallScope&) = delete;
  D3D9PeChildCallScope& operator=(const D3D9PeChildCallScope&) = delete;
  ~D3D9PeChildCallScope() noexcept {
    if (observer_) observer_->popCallScope(slot_);
  }
  HRESULT finish(const char *callName, HRESULT hr) noexcept {
    if (observer_) observer_->notifyCallScopeReturn(slot_, callName, hr);
    return hr;
  }
private:
  D3D9PeDiagnosticObserver *observer_ = nullptr;
  D3D9PePresentCallSlot slot_ = kD3D9PePresentCallSlotNone;
};

struct D3D9PeNullChildCallScope {
  HRESULT finish(const char *, HRESULT hr) const noexcept { return hr; }
};
inline constexpr D3D9PeNullChildCallScope d3d9PeNullChildCallScope{};

template<typename Body>
  requires std::is_nothrow_invocable_v<Body&&, D3D9PeChildCallScope&> &&
           std::is_nothrow_invocable_v<Body&&, const D3D9PeNullChildCallScope&>
__attribute__((always_inline))
inline HRESULT d3d9PeWithChildCallScope(
    D3D9PeDiagnosticObserver *observer, const char *callName,
    const void *callerPc, Body &&body) noexcept {
  if (!observer) return std::forward<Body>(body)(d3d9PeNullChildCallScope);
  D3D9PeChildCallScope peCall(*observer, callName, callerPc);
  return std::forward<Body>(body)(peCall);
}
