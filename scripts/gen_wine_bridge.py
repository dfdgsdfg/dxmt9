#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import re
from dataclasses import dataclass


# Schema-content version tag. Bump when the canonical schema string format
# below changes (e.g. addition of new fields / sizeof emission). Bumping
# the tag forces a hash mismatch between previously-built winemetal.dll /
# winemetal.so even when the underlying device_c.h prototypes are unchanged,
# so old binaries are caught instead of silently misbehaving.
ABI_HASH_VERSION_TAG = "dxmt9-bridge-abi-v1"

# 64-bit FNV-1a constants. Chosen because the implementation is trivially
# deterministic across Python versions and does not depend on hashlib's
# digest behavior. Output is rendered as a 16-hex-digit literal that fits
# in a constexpr uint64_t.
_FNV_OFFSET_BASIS_64 = 0xCBF29CE484222325
_FNV_PRIME_64 = 0x100000001B3
_U64_MASK = 0xFFFFFFFFFFFFFFFF


def fnv1a_64(data: bytes) -> int:
  hash_value = _FNV_OFFSET_BASIS_64
  for byte in data:
    hash_value ^= byte
    hash_value = (hash_value * _FNV_PRIME_64) & _U64_MASK
  # FNV-1a never produces 0 for non-empty input given the chosen offset
  # basis, but guard explicitly so kBridgeAbiHash != 0 is enforceable as a
  # static_assert on the consuming side.
  return hash_value or 1


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_C_HEADER = REPO_ROOT / "include" / "dxmt9" / "device_c.h"

OPAQUE_HANDLE_TYPES = {
    "D9CFactory",
    "D9CDevice",
    "D9CSwapChain",
    "D9CTexture",
    "D9CBuffer",
    "D9CSurface",
    "D9CQuery",
    "D9CStateBlock",
    "D9CVertexDecl",
    "D9CShader",
}


@dataclass
class TypeInfo:
    original: str
    base_type: str
    pointer_depth: int


@dataclass
class Param:
    signature_decl: str
    field_decl: str
    field_type: str
    name: str
    type_info: TypeInfo


@dataclass
class Proto:
    return_type: str
    return_info: TypeInfo
    name: str
    params: list[Param]


PROTOTYPE_RE = re.compile(
    r"^(?P<ret>.+?)\s+(?P<name>dxmt9c_[A-Za-z0-9_]+)\s*\((?P<params>.*)\)$",
    re.S,
)


def normalize_whitespace(value: str) -> str:
    return re.sub(r"\s+", " ", value.strip())


def split_params(raw: str) -> list[str]:
    if not raw or raw == "void":
        return []
    depth = 0
    current: list[str] = []
    result: list[str] = []
    for ch in raw:
        if ch == "," and depth == 0:
            piece = "".join(current).strip()
            if piece:
                result.append(piece)
            current = []
            continue
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        current.append(ch)
    piece = "".join(current).strip()
    if piece:
        result.append(piece)
    return result


def analyze_type(type_part: str, *, extra_pointer_depth: int = 0) -> TypeInfo:
    normalized = normalize_whitespace(type_part)
    pointer_depth = normalized.count("*") + extra_pointer_depth
    base = normalized.replace("*", " ")
    tokens = [token for token in base.split() if token not in {"const", "volatile", "struct", "enum", "class"}]
    base_type = tokens[-1] if tokens else normalized
    return TypeInfo(original=normalized, base_type=base_type, pointer_depth=pointer_depth)


def is_pointer_like(type_info: TypeInfo) -> bool:
    return type_info.pointer_depth > 0


def is_opaque_handle_pointer(type_info: TypeInfo) -> bool:
    return type_info.base_type in OPAQUE_HANDLE_TYPES and type_info.pointer_depth > 0


def parse_param(raw: str, index: int) -> Param:
    raw = normalize_whitespace(raw)
    array = ""
    array_match = re.search(r"(\[[^\]]+\])$", raw)
    body = raw
    if array_match:
        array = array_match.group(1)
        body = raw[: array_match.start()].rstrip()

    name_match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)$", body)
    name = None
    type_part = body
    if name_match:
        candidate = name_match.group(1)
        prefix = body[: name_match.start()].rstrip()
        if prefix:
            name = candidate
            type_part = prefix
    if name is None:
        name = f"arg{index}"
        type_part = raw

    type_part = normalize_whitespace(type_part)
    field_type = type_part
    extra_pointer_depth = 0
    if array:
        field_type = f"{field_type}*"
        extra_pointer_depth = 1
    field_decl = f"{field_type} {name}"
    signature_decl = f"{type_part} {name}{array}".strip()
    return Param(
        signature_decl=signature_decl,
        field_decl=field_decl,
        field_type=field_type,
        name=name,
        type_info=analyze_type(type_part, extra_pointer_depth=extra_pointer_depth),
    )


def parse_prototype(raw: str) -> Proto:
    raw = normalize_whitespace(raw)
    match = PROTOTYPE_RE.match(raw)
    if not match:
        raise ValueError(f"unsupported prototype: {raw!r}")
    return_type = normalize_whitespace(match.group("ret"))
    params = [parse_param(param, index) for index, param in enumerate(split_params(match.group("params")))]
    return Proto(
        return_type=return_type,
        return_info=analyze_type(return_type),
        name=match.group("name"),
        params=params,
    )


def load_schema_text(path: pathlib.Path, visited: set[pathlib.Path] | None = None) -> str:
    if visited is None:
        visited = set()
    path = path.resolve()
    if path in visited:
        return ""
    visited.add(path)

    lines: list[str] = []
    include_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
    for raw_line in path.read_text().splitlines():
        match = include_re.match(raw_line)
        if not match:
            lines.append(raw_line)
            continue

        include_path = match.group(1)
        resolved = (path.parent / include_path).resolve()
        if not resolved.exists():
            resolved = (REPO_ROOT / include_path).resolve()
        if not resolved.exists():
            lines.append(raw_line)
            continue

        lines.append(load_schema_text(resolved, visited))

    return "\n".join(lines)


def collect_device_c_prototypes(schema_header: pathlib.Path) -> list[Proto]:
    text = load_schema_text(schema_header)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"^\s*#.*$", "", text, flags=re.M)
    matches = re.finditer(
        r"([A-Za-z_][A-Za-z0-9_\s\*]*?\s+dxmt9c_[A-Za-z0-9_]+\s*\([^;]*?\)\s*;)",
        text,
        flags=re.S,
    )
    return [parse_prototype(match.group(1)[:-1]) for match in matches]


def collect_prototypes(schema_headers: list[pathlib.Path]) -> list[Proto]:
    ordered: dict[str, Proto] = {}
    for schema_header in schema_headers:
        for proto in collect_device_c_prototypes(schema_header):
            ordered.setdefault(proto.name, proto)
    return list(ordered.values())


def _canonical_type(type_info: TypeInfo) -> str:
  # Collapse "T*" / "T **" / "const T*" into a stable canonical form so two
  # builds that whitespace-differently spell the same C type produce
  # identical hash input. Pointer depth and base type are the load-bearing
  # bits for ABI compatibility; const/volatile are intentionally dropped
  # because they never alter the marshalled record layout.
  base = type_info.base_type
  return base + ("*" * type_info.pointer_depth)


def canonicalize_schema(protos: list[Proto]) -> str:
  # Sort by op name so the same set of prototypes always produces the same
  # canonical text, regardless of header include / declaration order. The
  # ABI-hash slot itself is not part of the dxmt9c_* prototype set, so it
  # is intentionally excluded — adding or removing the reserved slot is a
  # separate, deliberate compat break.
  lines: list[str] = [f"version:{ABI_HASH_VERSION_TAG}"]
  lines.append(f"opcount:{len(protos)}")
  for proto in sorted(protos, key=lambda p: p.name):
    parts = [
      "op",
      proto.name,
      _canonical_type(proto.return_info),
    ]
    for param in proto.params:
      parts.append(_canonical_type(param.type_info))
    lines.append("|".join(parts))
  return "\n".join(lines) + "\n"


def compute_bridge_abi_hash(protos: list[Proto]) -> int:
  return fnv1a_64(canonicalize_schema(protos).encode("utf-8"))


def wow64_field_decl(param: Param) -> str:
    if is_pointer_like(param.type_info):
        return f"uint32_t {param.name}"
    return param.field_decl


def wow64_return_decl(proto: Proto) -> str:
    if is_pointer_like(proto.return_info):
        return "uint32_t ret"
    return f"{proto.return_type} ret"


def write_ops_header(path: pathlib.Path, protos: list[Proto]) -> None:
    abi_hash = compute_bridge_abi_hash(protos)
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include <stdint.h>')
    lines.append('#include "dxmt9/device_c.h"')
    # Pulls in DXMT9_WINEMETAL_BRIDGE_OP_BASE — first slot consumed by the
    # device_c bridge. Slots 0..BASE-1 are owned by the shader unix-call IDs
    # in the same dispatch table; renumbering BridgeOpcode here keeps the
    # two ID spaces from colliding when winemetal.so unifies them.
    lines.append('#include "winemetal/winemetal_thunks.hpp"')
    lines.append("")
    lines.append("namespace dxmt9::bridge {")
    # Runtime ABI-handshake constant. Computed at codegen time over the
    # canonicalized schema (sorted op names, return + param types) so two
    # builds of the same device_c.h + winemetal_unix_schema.h produce
    # identical hashes. The PE-side DllMain calls
    # DXMT9_WINEMETAL_CALL_ABI_HASH and refuses to load on mismatch.
    # Schema canonicalization version: ABI_HASH_VERSION_TAG in
    # scripts/gen_wine_bridge.py. The reserved ABI-hash slot is NOT part
    # of the hashed input — adding or removing the slot itself is a
    # separate, deliberate compat break.
    lines.append(f"constexpr uint64_t kBridgeAbiHash = 0x{abi_hash:016x}ULL;")
    lines.append("static_assert(kBridgeAbiHash != 0,")
    lines.append('              "bridge ABI hash must be non-zero");')
    lines.append("")
    lines.append("enum class BridgeOpcode : unsigned int {")
    for index, proto in enumerate(protos):
        if index == 0:
            lines.append(f"  {proto.name} = DXMT9_WINEMETAL_BRIDGE_OP_BASE,")
        else:
            lines.append(f"  {proto.name},")
    lines.append("};")
    lines.append("")
    for proto in protos:
        lines.append(f"struct Args_{proto.name} {{")
        for param in proto.params:
            lines.append(f"  {param.field_decl};")
        if proto.return_type != "void":
            lines.append(f"  {proto.return_type} ret;")
        lines.append("};")
        lines.append("")
        lines.append(f"struct Args32_{proto.name} {{")
        for param in proto.params:
            lines.append(f"  {wow64_field_decl(param)};")
        if proto.return_type != "void":
            lines.append(f"  {wow64_return_decl(proto)};")
        lines.append("};")
        lines.append("")
    lines.append(f"constexpr unsigned int kBridgeOpcodeCount = {len(protos)};")
    lines.append("}  // namespace dxmt9::bridge")
    lines.append("")
    path.write_text("\n".join(lines))


def failure_return_expr(return_type: str) -> str:
    return f"dxmt9::bridge::bridgeFailure<{return_type}>(status)"


def write_client_cpp(path: pathlib.Path, ops_header_name: str, protos: list[Proto]) -> None:
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    lines.append("#define WIN32_LEAN_AND_MEAN")
    lines.append('#include <windows.h>')
    lines.append("#include <cwchar>")
    lines.append("#include <mutex>")
    lines.append("#include <string>")
    lines.append("#include <type_traits>")
    lines.append("")
    lines.append('#include "dxmt9/device_c.h"')
    lines.append('#include "dxmt9/wineunixlib.h"')
    lines.append(f'#include "{ops_header_name}"')
    lines.append("")
    lines.append("#if defined(DXMT9_DYNAMIC_WINEMETAL_BRIDGE)")
    lines.append("namespace {")
    lines.append("using WinemetalUnixCallFn = NTSTATUS (*)(unsigned int code, void *args);")
    lines.append("extern \"C\" IMAGE_DOS_HEADER __ImageBase;")
    lines.append("std::once_flag g_winemetal_bridge_once;")
    lines.append("NTSTATUS g_winemetal_bridge_status = DXMT9_STATUS_DLL_NOT_FOUND;")
    lines.append("HMODULE g_winemetal_bridge_module = nullptr;")
    lines.append("WinemetalUnixCallFn g_winemetal_unix_call = nullptr;")
    lines.append("")
    lines.append("std::wstring moduleSiblingPath(HMODULE module, const wchar_t *leaf) {")
    lines.append("  constexpr DWORD kBufferLength = 32768;")
    lines.append("  wchar_t buffer[kBufferLength] = {};")
    lines.append("  const DWORD len = GetModuleFileNameW(module, buffer, kBufferLength);")
    lines.append("  if (len == 0 || len >= kBufferLength) return {};")
    lines.append("  std::wstring path(buffer, len);")
    lines.append("  const auto slash = path.find_last_of(L\"\\\\/\");")
    lines.append("  if (slash == std::wstring::npos) return leaf;")
    lines.append("  path.resize(slash + 1);")
    lines.append("  path += leaf;")
    lines.append("  return path;")
    lines.append("}")
    lines.append("")
    lines.append("void initializeWinemetalBridge() {")
    lines.append("  const std::wstring path = moduleSiblingPath(reinterpret_cast<HMODULE>(&__ImageBase), L\"winemetal.dll\");")
    lines.append("  if (path.empty()) {")
    lines.append("    g_winemetal_bridge_status = DXMT9_STATUS_DLL_NOT_FOUND;")
    lines.append("    return;")
    lines.append("  }")
    lines.append("  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);")
    lines.append("  if (!module) {")
    lines.append("    module = LoadLibraryExW(path.c_str(), nullptr,")
    lines.append("                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);")
    lines.append("  }")
    lines.append("  if (!module) {")
    lines.append("    g_winemetal_bridge_status = DXMT9_STATUS_DLL_NOT_FOUND;")
    lines.append("    return;")
    lines.append("  }")
    lines.append("  const auto call = reinterpret_cast<WinemetalUnixCallFn>(")
    lines.append("      GetProcAddress(module, \"dxmt9_winemetal_unix_call\"));")
    lines.append("  if (!call) {")
    lines.append("    g_winemetal_bridge_status = DXMT9_STATUS_ENTRYPOINT_NOT_FOUND;")
    lines.append("    return;")
    lines.append("  }")
    lines.append("  g_winemetal_bridge_module = module;")
    lines.append("  g_winemetal_unix_call = call;")
    lines.append("  g_winemetal_bridge_status = DXMT9_STATUS_SUCCESS;")
    lines.append("}")
    lines.append("")
    lines.append("NTSTATUS bridgeUnixCall(unsigned int code, void *args) {")
    lines.append("  std::call_once(g_winemetal_bridge_once, initializeWinemetalBridge);")
    lines.append("  if (g_winemetal_bridge_status != DXMT9_STATUS_SUCCESS) return g_winemetal_bridge_status;")
    lines.append("  return g_winemetal_unix_call(code, args);")
    lines.append("}")
    lines.append("}  // namespace")
    lines.append("#else")
    lines.append('extern "C" NTSTATUS dxmt9_winemetal_unix_call(unsigned int code, void *args);')
    lines.append("namespace {")
    lines.append("NTSTATUS bridgeUnixCall(unsigned int code, void *args) {")
    lines.append("  return dxmt9_winemetal_unix_call(code, args);")
    lines.append("}")
    lines.append("}  // namespace")
    lines.append("#endif")
    lines.append("")
    lines.append("namespace dxmt9::bridge {")
    lines.append("template <typename T>")
    lines.append("T bridgeFailure(NTSTATUS status) {")
    lines.append("  if constexpr (std::is_pointer_v<T>) {")
    lines.append("    return nullptr;")
    lines.append("  } else {")
    lines.append("    return static_cast<T>(status);")
    lines.append("  }")
    lines.append("}")
    lines.append("}  // namespace dxmt9::bridge")
    lines.append("")
    for proto in protos:
        param_decl = ", ".join(param.signature_decl for param in proto.params)
        lines.append(f'extern "C" {proto.return_type} {proto.name}({param_decl}) {{')
        lines.append(f"  dxmt9::bridge::Args_{proto.name} args{{}};")
        for param in proto.params:
            lines.append(f"  args.{param.name} = {param.name};")
        lines.append(
            f"  const NTSTATUS status = bridgeUnixCall(static_cast<unsigned int>(dxmt9::bridge::BridgeOpcode::{proto.name}), &args);"
        )
        if proto.return_type == "void":
            lines.append("  (void)status;")
        else:
            lines.append(f"  if (status != DXMT9_STATUS_SUCCESS) return {failure_return_expr(proto.return_type)};")
            lines.append("  return args.ret;")
        lines.append("}")
        lines.append("")
    path.write_text("\n".join(lines))


def wow64_param_call_expr(param: Param, lines: list[str], post_lines: list[str]) -> str:
    info = param.type_info
    if is_opaque_handle_pointer(info) and info.pointer_depth == 1:
        return f"dxmt9::util::marshal::wow64::decodeHandle<{param.field_type}>(args->{param.name})"
    if is_opaque_handle_pointer(info) and info.pointer_depth == 2:
        lines.append(
            f"  auto wow64_{param.name} = dxmt9::util::marshal::wow64::decodePtr<uint32_t *>(args->{param.name});"
        )
        lines.append(f"  {info.base_type} *native_{param.name} = nullptr;")
        post_lines.append(
            f"  if (wow64_{param.name}) *wow64_{param.name} = dxmt9::util::marshal::wow64::encodeHandle(native_{param.name});"
        )
        return f"&native_{param.name}"
    if is_pointer_like(info):
        return f"dxmt9::util::marshal::wow64::decodePtr<{param.field_type}>(args->{param.name})"
    return f"args->{param.name}"


def emit_custom_wow64_thunk(proto: Proto, lines: list[str]) -> bool:
    if proto.name == "dxmt9c_texture_lock_rect":
        lines.append(f"extern \"C\" NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        lines.append("  dxmt9::d3d9::devicec::ScopedWow64ClientCall wow64_client_call;")
        lines.append(
            "  auto *wow64_out = dxmt9::util::marshal::wow64::decodePtr<dxmt9::util::marshal::wow64::LockedRect32 *>(args->out);"
        )
        lines.append("  D9CLockedRect native_out{};")
        lines.append(
            "  args->ret = dxmt9c_texture_lock_rect("
            "dxmt9::util::marshal::wow64::decodeHandle<D9CTexture*>(args->arg0), "
            "args->level, wow64_out ? &native_out : nullptr, "
            "dxmt9::util::marshal::wow64::decodePtr<const D9CRect*>(args->arg3), "
            "args->flags);"
        )
        lines.append("  if (wow64_out) dxmt9::util::marshal::wow64::storeLockedRect(wow64_out, native_out);")
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
        return True

    if proto.name == "dxmt9c_surface_lock_rect":
        lines.append(f"extern \"C\" NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        lines.append("  dxmt9::d3d9::devicec::ScopedWow64ClientCall wow64_client_call;")
        lines.append(
            "  auto *wow64_out = dxmt9::util::marshal::wow64::decodePtr<dxmt9::util::marshal::wow64::LockedRect32 *>(args->arg1);"
        )
        lines.append("  D9CLockedRect native_out{};")
        lines.append(
            "  args->ret = dxmt9c_surface_lock_rect("
            "dxmt9::util::marshal::wow64::decodeHandle<D9CSurface*>(args->arg0), "
            "wow64_out ? &native_out : nullptr, "
            "dxmt9::util::marshal::wow64::decodePtr<const D9CRect*>(args->arg2), "
            "args->flags);"
        )
        lines.append("  if (wow64_out) dxmt9::util::marshal::wow64::storeLockedRect(wow64_out, native_out);")
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
        return True

    if proto.name == "dxmt9c_buffer_lock":
        lines.append(f"extern \"C\" NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        lines.append("  dxmt9::d3d9::devicec::ScopedWow64ClientCall wow64_client_call;")
        lines.append("  void *native_data = nullptr;")
        lines.append(
            "  args->ret = dxmt9c_buffer_lock("
            "dxmt9::util::marshal::wow64::decodeHandle<D9CBuffer*>(args->arg0), "
            "args->offset, args->size, "
            "args->data ? &native_data : nullptr, "
            "args->flags);"
        )
        lines.append("  dxmt9::util::marshal::wow64::storeEncodedPointer(args->data, native_data);")
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
        return True

    if proto.name == "dxmt9c_device_commit_chunk":
        lines.append(f"extern \"C\" NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append("  dxmt9::d3d9::devicec::ScopedWow64ClientCall wow64_client_call;")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        lines.append(
            "  auto *client_chunk = dxmt9::util::marshal::wow64::decodePtr<const D9CCommandChunk *>(args->arg1);"
        )
        lines.append("  if (!client_chunk) {")
        lines.append(
            "    args->ret = dxmt9c_device_commit_chunk("
            "dxmt9::util::marshal::wow64::decodeHandle<D9CDevice*>(args->arg0), nullptr);"
        )
        lines.append("    return DXMT9_STATUS_SUCCESS;")
        lines.append("  }")
        lines.append("  D9CCommandChunk native_chunk = *client_chunk;")
        lines.append("  std::vector<std::uint8_t> native_records;")
        lines.append("  std::unique_ptr<dxmt9::d3d9::devicec::ScopedWow64NativePointerAllowance> native_records_allowance;")
        lines.append("  if (native_chunk.recordBytes != 0) {")
        lines.append("    constexpr std::uint32_t kMaxWow64CommitChunkBytes = 16u * 1024u * 1024u;")
        lines.append("    if (native_chunk.recordBytes > kMaxWow64CommitChunkBytes) {")
        lines.append("      args->ret = dxmt9::core::D3DERR_INVALIDCALL;")
        lines.append("      return DXMT9_STATUS_SUCCESS;")
        lines.append("    }")
        lines.append(
            "    const std::uint64_t client_record_ptr = "
            "static_cast<std::uint64_t>(native_chunk.records.lo) | "
            "(static_cast<std::uint64_t>(native_chunk.records.hi) << 32);"
        )
        lines.append("    if (!client_record_ptr) {")
        lines.append("      args->ret = dxmt9::core::D3DERR_INVALIDCALL;")
        lines.append("      return DXMT9_STATUS_SUCCESS;")
        lines.append("    }")
        lines.append(
            "    const auto *client_records = reinterpret_cast<const std::uint8_t *>("
            "static_cast<std::uintptr_t>(client_record_ptr));"
        )
        lines.append("    native_records.resize(native_chunk.recordBytes);")
        lines.append("    std::memcpy(native_records.data(), client_records, native_records.size());")
        lines.append("    native_records_allowance = std::make_unique<dxmt9::d3d9::devicec::ScopedWow64NativePointerAllowance>(")
        lines.append("        native_records.data(), native_records.size());")
        lines.append("    const auto native_record_ptr = reinterpret_cast<std::uintptr_t>(native_records.data());")
        lines.append("    native_chunk.records.lo = static_cast<std::uint32_t>(native_record_ptr & 0xffffffffull);")
        lines.append("    native_chunk.records.hi = static_cast<std::uint32_t>(native_record_ptr >> 32);")
        lines.append("  }")
        lines.append(
            "  args->ret = dxmt9c_device_commit_chunk("
            "dxmt9::util::marshal::wow64::decodeHandle<D9CDevice*>(args->arg0), &native_chunk);"
        )
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
        return True

    return False


def write_server_cpp(path: pathlib.Path, ops_header_name: str, protos: list[Proto]) -> None:
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    lines.append("#define WINE_UNIX_LIB 1")
    lines.append("")
    lines.append('#include "dxmt9/device_c.h"')
    lines.append('#include "dxmt9/wineunixlib.h"')
    lines.append(f'#include "{ops_header_name}"')
    lines.append('#include "d3d9/device_c_common.hpp"')
    lines.append('#include "util/unixcall_marshal.hpp"')
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("#include <cstring>")
    lines.append("#include <memory>")
    lines.append("#include <vector>")
    lines.append("")
    for proto in protos:
        lines.append(f"extern \"C\" NTSTATUS thunk_{proto.name}(void *opaque) {{")
        lines.append(f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args_{proto.name}>(opaque);")
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        call_args = ", ".join(f"args->{param.name}" for param in proto.params)
        if proto.return_type == "void":
            lines.append(f"  {proto.name}({call_args});")
        else:
            lines.append(f"  args->ret = {proto.name}({call_args});")
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
        if emit_custom_wow64_thunk(proto, lines):
            continue
        lines.append(f"extern \"C\" NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);")
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
        wow64_prelude: list[str] = []
        wow64_post: list[str] = []
        wow64_args = [wow64_param_call_expr(param, wow64_prelude, wow64_post) for param in proto.params]
        lines.extend(wow64_prelude)
        call_expr = f"{proto.name}({', '.join(wow64_args)})"
        if proto.return_type == "void":
            lines.append(f"  {call_expr};")
        elif is_opaque_handle_pointer(proto.return_info):
            lines.append(f"  args->ret = dxmt9::util::marshal::wow64::encodeHandle({call_expr});")
        elif is_pointer_like(proto.return_info):
            lines.append(f"  args->ret = dxmt9::util::marshal::wow64::encodePtr({call_expr});")
        else:
            lines.append(f"  args->ret = {call_expr};")
        lines.extend(wow64_post)
        if proto.name.endswith("_addref") and proto.params and is_opaque_handle_pointer(proto.params[0].type_info):
            lines.append(f"  dxmt9::util::marshal::wow64::retainHandle(args->{proto.params[0].name});")
        if proto.name.endswith("_release") and proto.params and is_opaque_handle_pointer(proto.params[0].type_info):
            lines.append(f"  dxmt9::util::marshal::wow64::releaseHandle(args->{proto.params[0].name});")
        lines.append("  return DXMT9_STATUS_SUCCESS;")
        lines.append("}")
        lines.append("")
    path.write_text("\n".join(lines))


def write_server_entries(path: pathlib.Path, protos: list[Proto]) -> None:
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    for proto in protos:
        lines.append(f"DXMT9_BRIDGE_UNIX_ENTRY(thunk_{proto.name}, thunk_wow64_{proto.name})")
    lines.append("")
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-header", dest="schema_headers", action="append")
    parser.add_argument("--ops-header", required=True)
    parser.add_argument("--client-cpp", required=True)
    parser.add_argument("--server-cpp", required=True)
    parser.add_argument("--server-entries", required=True)
    args = parser.parse_args()

    schema_headers = (
        [pathlib.Path(path) for path in args.schema_headers]
        if args.schema_headers
        else [DEVICE_C_HEADER]
    )
    protos = collect_prototypes(schema_headers)

    ops_header = pathlib.Path(args.ops_header)
    client_cpp = pathlib.Path(args.client_cpp)
    server_cpp = pathlib.Path(args.server_cpp)
    server_entries = pathlib.Path(args.server_entries)

    write_ops_header(ops_header, protos)
    write_client_cpp(client_cpp, ops_header.name, protos)
    write_server_cpp(server_cpp, ops_header.name, protos)
    write_server_entries(server_entries, protos)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
