#pragma once
#include "diagnostic_report.hpp"
#include <fstream>
#include <filesystem>
#include <string>
#include <streambuf>
#include <algorithm>

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

// Structured JSON is acquired once into an owned bounded buffer while the same
// bytes are optionally written to the compatibility artifact. Artifact failure
// never prevents the synchronous diagnostic consumer from receiving the data.
class StructuredOutputFile : public std::ostream {
 public:
  StructuredOutputFile(const std::string& path, std::ios_base::openmode mode,
                       const char* schema, std::size_t limit = 191u * 64u)
      : std::ostream(nullptr), file_(path, mode), buffer_(file_, limit),
        schema_(schema ? schema : ""), path_(path) {
    rdbuf(&buffer_);
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (!error) path_ = absolute.string();
  }
  ~StructuredOutputFile() override { close(); }
  void close() noexcept {
    if (finalized_) return;
    finalized_ = true;
    flush();
    report(schema_.c_str(), buffer_.data(), buffer_.truncated());
    bool success = file_.good() && file_.is_open();
    if (file_.is_open()) {
      file_.close();
      success = success && !file_.fail();
    }
    report(success ? "dxmt9-artifact-v1" : "dxmt9-artifact-failed-v1", path_);
  }
 private:
  class TeeBuffer final : public std::streambuf {
   public:
    TeeBuffer(std::ofstream& file, std::size_t limit)
        : file_(file), limit_(limit) { data_.reserve(limit); }
    std::string_view data() const noexcept { return data_; }
    bool truncated() const noexcept { return truncated_; }
   protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
      if (count <= 0) return 0;
      const auto size = static_cast<std::size_t>(count);
      const auto retained = std::min(size, limit_ - data_.size());
      data_.append(source, retained);
      truncated_ = truncated_ || retained != size;
      if (file_.is_open()) file_.write(source, count);
      return count;
    }
    int_type overflow(int_type ch) override {
      if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
      const char value = traits_type::to_char_type(ch);
      (void)xsputn(&value, 1);
      return ch;
    }
    int sync() override {
      if (file_.is_open()) file_.flush();
      return 0;
    }
   private:
    std::ofstream& file_;
    std::size_t limit_;
    std::string data_;
    bool truncated_ = false;
  };
  std::ofstream file_;
  TeeBuffer buffer_;
  std::string schema_;
  std::string path_;
  bool finalized_ = false;
};
} // namespace dxmt9::diagnostics
