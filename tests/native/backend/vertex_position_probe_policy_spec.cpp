#include "winemetal/unix/winemetal_vertex_position_probe_policy.hpp"
#include "vertex_skinning_probe_fixture.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "vertex probe policy failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

int main() {
  using namespace dxmt9::diagnostics;
  for (auto value : {"", "0", "-1", "+1", " 1", "1 ", "1x", "8193", "4294967296"})
    CHECK(!vertexProbeBound(value, 8192));
  CHECK(vertexProbeBound("8192", 8192) == 8192);
  CHECK(vertexProbeBound("1", 8192) == 1);
  CHECK(!vertexProbeBound("1", 0));
  CHECK(vertexProbeDigestValid(std::string(64, 'a')));
  CHECK(!vertexProbeDigestValid(std::string(64, 'A')));
  CHECK(!vertexProbeDigestValid(std::string(63, 'a')));
  CHECK(!vertexProbeDigestValid(std::string(64, 'g')));
  const auto digestA = std::string(64, 'a'), digestB = std::string(64, 'b');
  for (const auto& invalid : {std::string{}, std::string("0"), digestA + ",",
           "," + digestA, digestA + ", " + digestB, digestA + "," + digestA})
    CHECK(!vertexNoContractSelectionValid(invalid));
  CHECK(vertexNoContractSelectionContains(digestA + "," + digestB, digestB));
  CHECK(!vertexNoContractSelectionContains(digestA, digestB));
  std::string eightDigests;
  for (char digit = '0'; digit < '8'; ++digit) {
    if (!eightDigests.empty()) eightDigests += ',';
    eightDigests += std::string(64, digit);
  }
  CHECK(vertexNoContractSelectionValid(eightDigests));
  CHECK(!vertexNoContractSelectionValid(eightDigests + "," + digestA));
  // Small exhaustive admission truth table, including zero/overflow and every
  // independent boundary. The production predicate is the one being exercised.
  for (auto count : {0u, 3u, 6u, 7u, 24579u})
    for (auto requested : {0u, 3u, 6u, 7u, 24579u})
      for (auto capacity : {0u, 3u, 8192u, 8193u})
        for (auto index : {2u, 3u, 8192u})
          for (unsigned flags = 0; flags < 32; ++flags) {
            const auto expected = count == requested && count > 0 && count <= 24576 &&
                count % 3 == 0 && capacity > 0 && capacity <= 8192 && index < capacity && flags == 31;
            CHECK(vertexProbeDrawEligible(count, requested, (flags & 1) ? 1 : 2,
                (flags & 2) ? 0 : -1, (flags & 4) ? 0 : 1, index, capacity,
                flags & 8, flags & 16) == expected);
          }
  const std::string source = "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n"
      "  VSOut out; out.position = float4(0,0,0,1);\n  return out;\n}\n";
  const auto transformed = instrumentVertexPositionSource(source, 4);
  CHECK(transformed && transformed->find("vid < 4u") != std::string::npos);
  CHECK(transformed->find("atomic_fetch_min_explicit") != std::string::npos);
  CHECK(transformed->find("atomic_fetch_max_explicit") != std::string::npos);
  CHECK(transformed->find("atomic_fetch_add_explicit") != std::string::npos);
  CHECK(!instrumentVertexPositionSource(*transformed, 4));
  CHECK(!instrumentVertexPositionSource(source, 0));
  CHECK(!instrumentVertexPositionSource(source, 8193));
  CHECK(!instrumentVertexPositionSource(source + source, 4));
  CHECK(!instrumentVertexPositionSource(std::string(1024 * 1024, ' ') + source, 4));
  for (auto collision : {"[[buffer(29)]]", "[[ buffer ( 29 ) ]]", "[[buffer(\t29\n)]]"})
    CHECK(!instrumentVertexPositionSource(std::string(collision) + source, 4));
  auto early = source;
  early.insert(early.find("  VSOut"), "  if (vid) return\tout;\n");
  CHECK(!instrumentVertexPositionSource(early, 4));
  auto missingId = source;
  missingId.replace(missingId.find("uint vid"), 8, "uint id ");
  CHECK(!instrumentVertexPositionSource(missingId, 4));
  CHECK(!instrumentVertexPositionSource(source + "arbitrary trailing code", 4));
  CHECK(instrumentVertexPositionSource(source + "// decoded d3d hash 18320603168511268289\n", 4));
  CHECK(!instrumentVertexPositionSource(source + "// decoded d3d hash 18446744073709551616\n", 4));
  CHECK(!instrumentVertexPositionSource(source + "// decoded d3d hash 1\nint extra;", 4));
  CHECK(vertexProbeValueWords(false) == 4 && vertexProbeRecordWords(false) == 9);
  CHECK(vertexProbeValueWords(true) == 92 && vertexProbeRecordWords(true) == 185);
  const auto skin = vertexSkinningProbeFixture();
  const auto noContract = vertexNoContractControlSource(skin);
  CHECK(noContract && *noContract == "#pragma clang fp contract(off)\n" + skin);
  CHECK(!vertexNoContractControlSource(*noContract));
  CHECK(vertexNoContractControlSource(source));
  CHECK(!vertexNoContractControlSource("not a generated vertex entry"));
  CHECK(!vertexNoContractControlSource(early));
  CHECK(!vertexNoContractControlSource(*transformed));
  CHECK(!vertexNoContractControlSource(skin + skin));
  CHECK(!vertexNoContractControlSource(std::string(1024 * 1024, ' ') + skin));
  const auto detailed = instrumentVertexPositionSource(skin, 4, true);
  CHECK(detailed && detailed->find("dxmt9_input_bone_x0 * vin3.x") != std::string::npos);
  CHECK(detailed->find("const float4 dxmt9_input_blend0 = r[0];") != std::string::npos);
  CHECK(detailed->find("vid * 185u + 184u") != std::string::npos);
  CHECK(!instrumentVertexPositionSource(*detailed, 4, true));
  CHECK(!instrumentVertexPositionSource(skin, 2049, true));
  CHECK(!instrumentVertexPositionSource(source, 4, true));
  for (auto anchor : {"cFloat[192]", "// dp4 r0, v0, r0", "// mul r0, c0, v3", "ffpVs.halfPixelFixup"}) {
    auto malformed = skin;
    malformed.replace(malformed.find(anchor), std::string_view(anchor).size(), "missing");
    CHECK(!instrumentVertexPositionSource(malformed, 4, true));
  }
  CHECK(!instrumentVertexPositionSource(skin + "\n// cFloat[192]\n", 4, true));
  std::puts("vertex position probe bounded policy passed");
}
