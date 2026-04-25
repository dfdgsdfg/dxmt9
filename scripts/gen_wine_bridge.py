#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import re
from dataclasses import dataclass


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


def wow64_field_decl(param: Param) -> str:
    if is_pointer_like(param.type_info):
        return f"uint32_t {param.name}"
    return param.field_decl


def wow64_return_decl(proto: Proto) -> str:
    if is_pointer_like(proto.return_info):
        return "uint32_t ret"
    return f"{proto.return_type} ret"


def write_ops_header(path: pathlib.Path, protos: list[Proto]) -> None:
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include <stdint.h>')
    lines.append('#include "dxmt9/device_c.h"')
    lines.append("")
    lines.append("namespace dxmt9::bridge {")
    lines.append("enum class BridgeOpcode : unsigned int {")
    for proto in protos:
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
    lines.append("#include <type_traits>")
    lines.append("")
    lines.append('#include "dxmt9/device_c.h"')
    lines.append('#include "dxmt9/wineunixlib.h"')
    lines.append(f'#include "{ops_header_name}"')
    lines.append("")
    lines.append('extern "C" NTSTATUS dxmt9_winemetal_unix_call(unsigned int code, void *args);')
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
            f"  const NTSTATUS status = dxmt9_winemetal_unix_call(static_cast<unsigned int>(dxmt9::bridge::BridgeOpcode::{proto.name}), &args);"
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
        lines.append(f"NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
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
        lines.append(f"NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
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
        lines.append(f"NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
        lines.append(
            f"  auto *args = dxmt9::util::marshal::decodeOpaque<dxmt9::bridge::Args32_{proto.name}>(opaque);"
        )
        lines.append("  if (!args) return DXMT9_STATUS_INVALID_PARAMETER;")
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

    return False


def write_server_cpp(path: pathlib.Path, ops_header_name: str, protos: list[Proto]) -> None:
    lines: list[str] = []
    lines.append("// Generated by scripts/gen_wine_bridge.py. Do not edit by hand.")
    lines.append("#define WINE_UNIX_LIB 1")
    lines.append("")
    lines.append('#include "dxmt9/device_c.h"')
    lines.append('#include "dxmt9/wineunixlib.h"')
    lines.append(f'#include "{ops_header_name}"')
    lines.append('#include "util/unixcall_marshal.hpp"')
    lines.append("")
    for proto in protos:
        lines.append(f"NTSTATUS thunk_{proto.name}(void *opaque) {{")
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
        lines.append(f"NTSTATUS thunk_wow64_{proto.name}(void *opaque) {{")
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
        if proto.name.endswith("_release") and proto.params and is_opaque_handle_pointer(proto.params[0].type_info):
            lines.append(f"  dxmt9::util::marshal::wow64::eraseHandle(args->{proto.params[0].name});")
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
