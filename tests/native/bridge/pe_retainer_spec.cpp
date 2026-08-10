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
  return 0;
}
