// Cold supervisor helper and controlled transport test; never loads D3D9.
#include "../../../src/d3d9/d3d9_pe_render_tape_arm_request_win32.hpp"

#include <charconv>
#include <cstdio>
#include <limits>
#include <string>

using namespace dxmt9::d3d9;

bool decimal(const char* text, std::uint64_t& value) {
  const auto end = text + std::strlen(text);
  const auto result = std::from_chars(text, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

bool publish(const char* path, const RenderTapeArmRequest& request) {
  const RenderTapeRequestFile file(CreateFileA(path, GENERIC_WRITE, 0u, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0u;
  // On error preserve the partial file as failure evidence; never refresh it.
  return WriteFile(file.get(), &request, sizeof(request), &written, nullptr) &&
      written == sizeof(request) && FlushFileBuffers(file.get());
}

int selfTest(const char* path) {
  RenderTapeArmRequestSelector selector{};
  constexpr auto nonce = "0123456789abcdeffedcba9876543210";
  if (!initializeRenderTapeArmRequestSelector(selector, path, nonce, 0u, true) ||
      pollRenderTapeArmRequest(selector) != RenderTapeArmRequestDecision::Wait) return 1;
  RenderTapeArmRequest request{};
  request.processId = selector.gate.processId;
  request.processCreation = selector.gate.processCreation;
  request.nonce = selector.gate.nonce;
  request.issuedMs = GetTickCount64();
  request.expiresMs = request.issuedMs + 30000u;
  {
    const RenderTapeRequestFile writer(CreateFileA(path, GENERIC_WRITE, 0u, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (writer.get() == INVALID_HANDLE_VALUE) return 2;
    DWORD written = 0u;
    if (!WriteFile(writer.get(), &request, sizeof(request), &written, nullptr) ||
        written != sizeof(request) ||
        pollRenderTapeArmRequest(selector) != RenderTapeArmRequestDecision::Wait) return 3;
  }
  const auto decision = pollRenderTapeArmRequest(selector);
  RenderTapeArmRequestSelector stale{};
  const bool staleAccepted = initializeRenderTapeArmRequestSelector(
      stale, path, nonce, 0u, true);
  const bool repeated = publish(path, request);
  const bool okay = decision == RenderTapeArmRequestDecision::Accept &&
      pollRenderTapeArmRequest(selector) == RenderTapeArmRequestDecision::Terminal &&
      selector.gate.mayFinishBootstrap(GetTickCount64()) && !staleAccepted && !repeated;
  // Only this CREATE_NEW-owned test file is removed; never a caller's old file.
  if (!DeleteFileA(path)) return 4;
  if (!okay) return 5;
  RenderTapeArmRequestSelector broken{};
  if (!initializeRenderTapeArmRequestSelector(broken, path, nonce, 0u, true)) return 6;
  {
    const RenderTapeRequestFile writer(CreateFileA(path, GENERIC_WRITE, 0u, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (writer.get() == INVALID_HANDLE_VALUE) return 7;
    DWORD written = 0u;
    if (!WriteFile(writer.get(), &request, 1u, &written, nullptr) || written != 1u) return 8;
  }
  const bool rejected = pollRenderTapeArmRequest(broken) == RenderTapeArmRequestDecision::Reject;
  if (!DeleteFileA(path)) return 9;
  if (!rejected) return 10;
  std::puts("arm-request transport: missing, busy, exact read, one-shot, stale, no-overwrite, short read passed");
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "--receiver-test") == 0) {
    RenderTapeArmRequestSelector selector{};
    if (!initializeRenderTapeArmRequestSelector(selector, argv[2], argv[3], 0u, true)) return 1;
    std::printf("receiver_ready pid=%u creation=%llu\n", selector.gate.processId,
        static_cast<unsigned long long>(selector.gate.processCreation));
    std::fflush(stdout);
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 5000u) {
      const auto decision = pollRenderTapeArmRequest(selector);
      if (decision == RenderTapeArmRequestDecision::Accept) {
        std::puts("receiver_accepted");
        return 0;
      }
      if (decision != RenderTapeArmRequestDecision::Wait) return 2;
      Sleep(10u);
    }
    return 3;
  }
  if (argc == 3 && std::strcmp(argv[1], "--self-test") == 0)
    return selfTest(argv[2]);
  if (argc != 6) {
    std::fprintf(stderr, "usage: %s PATH NONCE PID CREATION_FILETIME TTL_MS\n"
        "       %s --self-test NEW_PATH\n", argv[0], argv[0]);
    return 2;
  }
  RenderTapeArmRequest request{};
  std::uint64_t pid = 0u, creation = 0u, ttl = 0u;
  if (!renderTapeArmRequestPathValid(argv[1]) || !parseRenderTapeArmNonce(argv[2], request.nonce) ||
      !decimal(argv[3], pid) || !pid || pid > std::numeric_limits<DWORD>::max() ||
      !decimal(argv[4], creation) || !creation || !decimal(argv[5], ttl) || !ttl ||
      ttl > RenderTapeArmRequestGate::kMaxRequestMs) return 3;
  const RenderTapeRequestFile process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
  if (!process.get() || renderTapeRequestProcessCreation(process.get()) != creation ||
      WaitForSingleObject(process.get(), 0u) != WAIT_TIMEOUT) return 4;
  request.processId = static_cast<DWORD>(pid);
  request.processCreation = creation;
  request.issuedMs = GetTickCount64();
  if (request.issuedMs > std::numeric_limits<std::uint64_t>::max() - ttl) return 5;
  request.expiresMs = request.issuedMs + ttl;
  if (!publish(argv[1], request)) return 6;
  std::printf("request_written pid=%u creation=%llu request=%016llx%016llx issued_ms=%llu expires_ms=%llu\n",
      request.processId, static_cast<unsigned long long>(creation),
      static_cast<unsigned long long>(request.nonce[0]),
      static_cast<unsigned long long>(request.nonce[1]),
      static_cast<unsigned long long>(request.issuedMs),
      static_cast<unsigned long long>(request.expiresMs));
  return 0;
}
