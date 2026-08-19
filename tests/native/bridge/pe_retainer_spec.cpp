#include "d3d9_pe_retainer.hpp"

#include <cstdint>
#include <iostream>

struct RefCounter {
  std::uint32_t refs = 1;
};

struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template<typename T>
void addRef(T* value) {
  ++value->refs;
}

template<typename T>
std::uint32_t release(T* value) {
  return --value->refs;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return release(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return release(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return release(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return release(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return release(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return release(value);
}

namespace {

bool check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "pe_retainer_spec failed: " << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  D3D9PePendingCommandRetainer retainer;
  D9CQuery first{};
  D9CQuery second{};

  auto firstAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, firstAcquire);
  retainer.retainQuery(&first, firstAcquire);
  if (!check(first.refs == 2u, "query is addref'd exactly once") ||
      !check(retainer.size() == 1u, "query occupies one flat-set entry")) {
    return 1;
  }

  auto rollbackAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, rollbackAcquire);
  retainer.retainQuery(&second, rollbackAcquire);
  if (!check(first.refs == 2u, "existing query is not re-retained") ||
      !check(second.refs == 2u, "new query is retained")) {
    return 1;
  }
  retainer.rollback(rollbackAcquire);
  if (!check(first.refs == 2u, "rollback preserves pre-checkpoint query") ||
      !check(second.refs == 1u, "rollback releases new query") ||
      !check(retainer.size() == 1u, "rollback restores flat arena checkpoint")) {
    return 1;
  }

  auto objectAcquire = retainer.beginAcquire();
  retainer.retainWireObject(D9C_CHUNK_HANDLE_KIND_QUERY, &first,
                            objectAcquire);
  if (!check(first.refs == 2u, "typed canonical retain de-duplicates query")) {
    return 1;
  }

  retainer.clear();
  if (!check(first.refs == 1u, "clear releases retained query") ||
      !check(retainer.size() == 0u, "clear preserves an empty flat set")) {
    return 1;
  }

  // --- Cross-epoch pin aggregation -------------------------------------
  // A chunk boundary is endEpoch(); a discard is clear(). The point of the
  // former is that an object named by consecutive chunks is addref'd once, not
  // once per chunk, so the wire pair never happens.
  {
    D3D9PePendingCommandRetainer warm;
    D9CBuffer hot{};
    D9CBuffer cold{};

    for (int chunk = 0; chunk < 5; ++chunk) {
      auto acquire = warm.beginAcquire();
      warm.retainBuffer(&hot, acquire);
      if (chunk == 0) {
        warm.retainBuffer(&cold, acquire);
      }
      warm.endEpoch();
    }
    if (!check(hot.refs == 2u,
               "an object named every chunk is addref'd exactly once") ||
        !check(warm.size() == 1u,
               "the cold entry is evicted and the hot one is kept")) {
      return 1;
    }
    // cold was named only in epoch 0; kWarmEpochs == 1 keeps it through the
    // end of epoch 1 and releases it when epoch 2 closes.
    if (!check(cold.refs == 1u, "a cold entry releases its pin on eviction")) {
      return 1;
    }

    // Naming it again after eviction takes a fresh pin.
    auto reacquire = warm.beginAcquire();
    warm.retainBuffer(&cold, reacquire);
    if (!check(cold.refs == 2u, "an evicted object is re-pinned when named") ||
        !check(warm.size() == 2u, "re-pinning appends a new arena entry")) {
      return 1;
    }

    // A record that only dedupe-hits a warm entry must not release it on
    // rollback: the entry belongs to an earlier epoch, below the checkpoint.
    auto rollbackWarm = warm.beginAcquire();
    warm.retainBuffer(&hot, rollbackWarm);
    warm.rollback(rollbackWarm);
    if (!check(hot.refs == 2u,
               "rollback does not release a warm entry it only re-touched") ||
        !check(warm.size() == 2u, "rollback leaves the warm arena intact")) {
      return 1;
    }

    // Exact eviction boundary: touching an entry inside the warm window keeps
    // it, and one epoch past the window drops it.
    D9CBuffer boundary{};
    auto boundaryAcquire = warm.beginAcquire();
    warm.retainBuffer(&boundary, boundaryAcquire);
    warm.endEpoch();  // closes the epoch it was named in
    if (!check(boundary.refs == 2u, "an entry survives its own epoch close")) {
      return 1;
    }
    warm.endEpoch();  // one fully idle epoch, still inside kWarmEpochs
    if (!check(boundary.refs == 2u, "an entry survives one idle epoch")) {
      return 1;
    }
    warm.endEpoch();  // second idle epoch, now cold
    if (!check(boundary.refs == 1u,
               "an entry is released after kWarmEpochs + 1 idle epochs")) {
      return 1;
    }

    // Interleaved kinds share one arena and one epoch clock.
    D9CTexture texture{};
    D9CShader shader{};
    auto mixedAcquire = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture,
                          mixedAcquire);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAcquire);
    warm.endEpoch();
    auto mixedAgain = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture, mixedAgain);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAgain);
    warm.endEpoch();
    if (!check(texture.refs == 2u, "texture pin survives a chunk boundary") ||
        !check(shader.refs == 2u, "shader pin survives a chunk boundary")) {
      return 1;
    }

    // The three idle epochs above evicted `hot` and `cold` as well; re-pin
    // them so the discard assertions below are not vacuous.
    auto finalAcquire = warm.beginAcquire();
    warm.retainBuffer(&hot, finalAcquire);
    warm.retainBuffer(&cold, finalAcquire);
    if (!check(hot.refs == 2u && cold.refs == 2u,
               "both buffers are pinned again before the discard check")) {
      return 1;
    }

    // The discard path must release every warm pin, whatever its epoch — this
    // is what device teardown / Reset / ResetEx rely on so nothing is still
    // pinned when dxmt9c_device_reset* runs.
    warm.clear();
    if (!check(hot.refs == 1u, "clear releases a warm pin") ||
        !check(cold.refs == 1u, "clear releases a re-pinned entry") ||
        !check(texture.refs == 1u, "clear releases a warm texture pin") ||
        !check(shader.refs == 1u, "clear releases a warm shader pin") ||
        !check(warm.size() == 0u, "clear empties the warm arena")) {
      return 1;
    }
  }

  return 0;
}
