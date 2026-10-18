#pragma once

#include "d3d9_pe_render_tape_arm_request.hpp"

#include <windows.h>
#include <cstring>

namespace dxmt9::d3d9 {

class RenderTapeRequestFile {
public:
  explicit RenderTapeRequestFile(HANDLE handle) noexcept : handle_(handle) {}
  ~RenderTapeRequestFile() {
    if (handle_ != INVALID_HANDLE_VALUE && handle_) CloseHandle(handle_);
  }
  RenderTapeRequestFile(const RenderTapeRequestFile&) = delete;
  RenderTapeRequestFile& operator=(const RenderTapeRequestFile&) = delete;
  HANDLE get() const noexcept { return handle_; }
private:
  HANDLE handle_;
};

inline std::uint64_t renderTapeRequestProcessCreation(HANDLE process) noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0u;
  return (std::uint64_t{created.dwHighDateTime} << 32u) | created.dwLowDateTime;
}

inline bool initializeRenderTapeArmRequestSelector(
    RenderTapeArmRequestSelector& selector, std::string_view path,
    std::string_view nonce, std::uint32_t skipPresents, bool processOwner) noexcept {
  selector.initialized = true;
  selector.enabled = true;
  auto& gate = selector.gate;
  gate.state = RenderTapeArmRequestState::Rejected;
  if (!processOwner || skipPresents || !renderTapeArmRequestPathValid(path) ||
      !parseRenderTapeArmNonce(nonce, gate.nonce)) return false;
  std::memcpy(selector.path.data(), path.data(), path.size());
  selector.path[path.size()] = '\0';
  // A stale path (even an empty file) is never a pending new request.
  if (GetFileAttributesA(selector.path.data()) != INVALID_FILE_ATTRIBUTES ||
      GetLastError() != ERROR_FILE_NOT_FOUND) return false;
  gate.processId = GetCurrentProcessId();
  gate.processCreation = renderTapeRequestProcessCreation(GetCurrentProcess());
  if (!gate.processCreation) return false;
  gate.readyMs = gate.lastMs = GetTickCount64();
  gate.state = RenderTapeArmRequestState::Waiting;
  return true;
}

inline RenderTapeArmRequestInput readRenderTapeArmRequest(
    const char* path, RenderTapeArmRequest& request) noexcept {
  const RenderTapeRequestFile file(CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return RenderTapeArmRequestInput::Missing;
    if (error == ERROR_SHARING_VIOLATION) return RenderTapeArmRequestInput::WriterBusy;
    return RenderTapeArmRequestInput::Invalid;
  }
  LARGE_INTEGER size{};
  DWORD read = 0u;
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart != sizeof(request) ||
      !ReadFile(file.get(), &request, sizeof(request), &read, nullptr) ||
      read != sizeof(request)) return RenderTapeArmRequestInput::Invalid;
  return RenderTapeArmRequestInput::Ready;
}

inline RenderTapeArmRequestDecision pollRenderTapeArmRequest(
    RenderTapeArmRequestSelector& selector) noexcept {
  if (selector.gate.state != RenderTapeArmRequestState::Waiting)
    return RenderTapeArmRequestDecision::Terminal;
  RenderTapeArmRequest request{};
  const auto input = readRenderTapeArmRequest(selector.path.data(), request);
  return selector.gate.observe(input, request, GetTickCount64());
}

} // namespace dxmt9::d3d9
