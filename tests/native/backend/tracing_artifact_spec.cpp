#include "../../../src/dxmt9/tracing/artifact.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main() {
  using dxmt9::tracing::detail::AtomicArtifact;

  const auto root = std::filesystem::temp_directory_path() /
      ("dxmt9-tracing-artifact-" + std::to_string(getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto output = root / "trace.pftrace";
  {
    AtomicArtifact artifact(output.string());
    assert(artifact.open());
    assert(artifact.write("trace", 5));
    assert(artifact.commit());
  }
  assert(read_file(output) == "trace");

  {
    AtomicArtifact collision(output.string());
    assert(collision.open());
    assert(collision.write("replacement", 11));
    assert(!collision.commit());
  }
  assert(read_file(output) == "trace");

  const auto target = root / "target";
  const auto symlink = root / "symlink.pftrace";
  {
    std::ofstream sentinel(target, std::ios::binary);
    sentinel << "sentinel";
  }
  assert(::symlink(target.c_str(), symlink.c_str()) == 0);
  {
    AtomicArtifact through_symlink(symlink.string());
    assert(through_symlink.open());
    assert(through_symlink.write("overwrite", 9));
    assert(!through_symlink.commit());
  }
  assert(read_file(target) == "sentinel");
  assert(std::filesystem::is_symlink(symlink));

  {
    AtomicArtifact aborted((root / "aborted").string());
    assert(aborted.open());
    assert(aborted.write("discard", 7));
    aborted.abort();
    assert(!aborted.commit());
  }
  assert(!std::filesystem::exists(root / "aborted"));
  for (const auto& entry : std::filesystem::directory_iterator(root))
    assert(entry.path().filename().string().find(".dxmt9-artifact-") != 0);

  std::filesystem::remove_all(root);
}
