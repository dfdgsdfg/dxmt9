#pragma once
#include "diagnostic_report.hpp"
#include <fstream>
#include <filesystem>
#include <string>

namespace dxmt9::diagnostics {
// Only diagnostic output uses this adapter. Serialization still happens once.
// The sealed harness hashes and imports structured contents after writers close.
class OutputFile : public std::ofstream {
 public:
  OutputFile(const std::string& path, std::ios_base::openmode mode)
      : std::ofstream(path, mode), path_(path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (!error) path_ = absolute.string();
  }
  ~OutputFile() {
    bool success = good() && is_open();
    if (is_open()) {
      flush();
      success = success && good();
      close();
      success = success && !fail();
    }
    report(success ? "dxmt9-artifact-v1" : "dxmt9-artifact-failed-v1", path_);
  }
 private:
  std::string path_;
};
} // namespace dxmt9::diagnostics
