#include "../../../src/dxmt9/tracing/config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
  using dxmt9::tracing::ParsedConfig;
  using dxmt9::tracing::parse_request;

  const auto path = std::filesystem::temp_directory_path() /
                    ("dxmt9-trace-config-" + std::to_string(getpid()) + ".json");
  auto rejects = [&](const char* request) {
    {
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file << request;
    }
    ParsedConfig parsed;
    std::string error;
    assert(!parse_request(path.string(), parsed, error));
    assert(!error.empty());
  };

  rejects(R"({"schema":"dxmt9.trace.request.v1","schema":"dxmt9.trace.request.v1"})");
  rejects(R"({"schema":)");
  rejects(R"({"schema":"dxmt9.trace.request.v1","config_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})");
  std::filesystem::remove(path);
  std::cout << "Tracing config rejection cases passed\n";
}
