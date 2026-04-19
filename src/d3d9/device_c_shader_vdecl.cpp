#include "device_c_common.hpp"

using namespace dxmt9::d3d9::devicec;

extern "C" D9CShader* dxmt9c_device_create_vertex_shader(D9CDevice* d, const uint32_t* bytecode) {
  dxmt9DebugLog("device_create_vertex_shader begin device=%p bytecode=%p",
                static_cast<void*>(d), bytecode);
  if (!bytecode) {
    dxmt9DebugLog("device_create_vertex_shader failed: null bytecode");
    return nullptr;
  }

  size_t wordCount = 0;
  if (!computeShaderBytecodeWordCount(bytecode, &wordCount)) {
    dxmt9DebugLog("device_create_vertex_shader failed: invalid bytecode layout bytecode=%p",
                  bytecode);
    return nullptr;
  }

  dxmt9DebugLog("device_create_vertex_shader bytecode=%p dwords=%zu version=0x%08x end=0x%08x",
                bytecode, wordCount, bytecode[0], bytecode[wordCount - 1]);

  dxmt9::core::ShaderBytecode shaderBytecode;
  shaderBytecode.bytes.assign(reinterpret_cast<const uint8_t*>(bytecode),
                              reinterpret_cast<const uint8_t*>(bytecode) + wordCount * 4);
  shaderBytecode.hash = dxmt9::core::hashBytes(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(shaderBytecode.bytes.data()),
                                 shaderBytecode.bytes.size()));

  dxmt9::core::ShaderRef ref;
  ref.kind = dxmt9::core::ShaderRef::Kind::Bytecode;
  ref.hash = shaderBytecode.hash;
  ref.bytecode = std::move(shaderBytecode);
  maybeDumpShaderBytecode("shader", bytecode, wordCount, ref.hash);

  auto* shader = new D9CShader;
  shader->ref = std::move(ref);
  shader->bytecodeWords.assign(bytecode, bytecode + wordCount);
  return shader;
}

extern "C" D9CShader* dxmt9c_device_create_pixel_shader(D9CDevice* d, const uint32_t* bytecode) {
  dxmt9DebugLog("device_create_pixel_shader bytecode=%p", bytecode);
  return dxmt9c_device_create_vertex_shader(d, bytecode);
}

extern "C" D9CVertexDecl* dxmt9c_device_create_vertex_declaration(D9CDevice*,
                                                                  const D9CVertexElement* elems) {
  auto* decl = new D9CVertexDecl;
  for (const D9CVertexElement* element = elems;
       !(element->stream == 0xff && element->type == 17);
       ++element) {
    dxmt9::core::VertexElement vertexElement;
    vertexElement.stream = element->stream;
    vertexElement.offset = element->offset;
    vertexElement.type = element->type;
    vertexElement.method = element->method;
    vertexElement.usage = element->usage;
    vertexElement.usageIndex = element->usageIndex;
    decl->elements.push_back(vertexElement);
    decl->raw.push_back(*element);
  }
  decl->raw.push_back({0xff, 0, 17, 0, 0, 0});
  return decl;
}

extern "C" void dxmt9c_shader_addref(D9CShader* s) {
  if (s) {
    s->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_shader_release(D9CShader* s) {
  dxmt9DebugLog("shader_release begin shader=%p", static_cast<void*>(s));
  if (!s) {
    return 0;
  }
  const uint32_t refs = s->refs.fetch_sub(1) - 1;
  dxmt9DebugLog("shader_release shader=%p refs=%u dwords=%zu", static_cast<void*>(s), refs,
                s->bytecodeWords.size());
  if (refs == 0) {
    delete s;
  }
  return refs;
}

extern "C" int32_t dxmt9c_shader_get_bytecode(D9CShader* s, void* data, uint32_t* size) {
  dxmt9DebugLog("shader_get_bytecode begin shader=%p data=%p size_ptr=%p",
                static_cast<void*>(s), data, static_cast<void*>(size));
  if (!s) {
    dxmt9DebugLog("shader_get_bytecode failed: null shader");
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  const uint32_t bytes = static_cast<uint32_t>(s->bytecodeWords.size() * 4);
  if (!data) {
    dxmt9DebugLog("shader_get_bytecode query shader=%p bytes=%u", static_cast<void*>(s), bytes);
    if (size) {
      *size = bytes;
    }
    return dxmt9::core::D3D_OK;
  }
  if (size && *size < bytes) {
    dxmt9DebugLog("shader_get_bytecode too-small shader=%p provided=%u required=%u",
                  static_cast<void*>(s), *size, bytes);
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  dxmt9DebugLog("shader_get_bytecode copy shader=%p dst=%p bytes=%u",
                static_cast<void*>(s), data, bytes);
  std::memcpy(data, s->bytecodeWords.data(), bytes);
  if (size) {
    *size = bytes;
  }
  return dxmt9::core::D3D_OK;
}

extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* v) {
  if (v) {
    v->refs.fetch_add(1);
  }
}

extern "C" uint32_t dxmt9c_vdecl_release(D9CVertexDecl* v) {
  if (!v) {
    return 0;
  }
  const uint32_t refs = v->refs.fetch_sub(1) - 1;
  if (refs == 0) {
    delete v;
  }
  return refs;
}

extern "C" int32_t dxmt9c_vdecl_get_declaration(D9CVertexDecl* v, D9CVertexElement* out,
                                                uint32_t* count) {
  const uint32_t n = static_cast<uint32_t>(v->raw.size());
  if (!out) {
    if (count) {
      *count = n;
    }
    return dxmt9::core::D3D_OK;
  }
  std::memcpy(out, v->raw.data(), n * sizeof(D9CVertexElement));
  if (count) {
    *count = n;
  }
  return dxmt9::core::D3D_OK;
}
