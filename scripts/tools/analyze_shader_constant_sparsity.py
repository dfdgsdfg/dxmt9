#!/usr/bin/env python3
"""Analyze D3D9 shader constant-register sparsity from dxmt9 bytecode dumps.

`run_3dmark05_perf_probe.sh --dump-shaders` writes D3D bytecode under
`traces/<run-id>/analysis/shaders/bytecode`. This tool parses those dumps and
estimates how much of the current prefix-preserving VsConsts/PsConsts upload
shape is real register use versus holes. It does not prove a packed layout is
safe; indexed constant access still requires the current full-array fallback.
"""

from __future__ import annotations

import argparse
import csv
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


K_MAX_VERTEX_FLOAT = 256
K_MAX_PIXEL_FLOAT = 224
K_MAX_INT = 16
K_MAX_BOOL = 16
FLOAT4_BYTES = 16
INT4_BYTES = 16
BOOL_BYTES = 4
COMPONENT_NAMES = ("x", "y", "z", "w")

K_D3DSIO_NOP = 0
K_D3DSIO_MOV = 1
K_D3DSIO_ADD = 2
K_D3DSIO_SUB = 3
K_D3DSIO_MAD = 4
K_D3DSIO_MUL = 5
K_D3DSIO_RCP = 6
K_D3DSIO_RSQ = 7
K_D3DSIO_DP3 = 8
K_D3DSIO_DP4 = 9
K_D3DSIO_MIN = 10
K_D3DSIO_MAX = 11
K_D3DSIO_SLT = 12
K_D3DSIO_SGE = 13
K_D3DSIO_EXP = 14
K_D3DSIO_LOG = 15
K_D3DSIO_LIT = 16
K_D3DSIO_DST = 17
K_D3DSIO_LRP = 18
K_D3DSIO_FRC = 19
K_D3DSIO_M4X4 = 20
K_D3DSIO_M4X3 = 21
K_D3DSIO_M3X4 = 22
K_D3DSIO_M3X3 = 23
K_D3DSIO_M3X2 = 24
K_D3DSIO_CALL = 25
K_D3DSIO_CALLNZ = 26
K_D3DSIO_LOOP = 27
K_D3DSIO_RET = 28
K_D3DSIO_ENDLOOP = 29
K_D3DSIO_LABEL = 30
K_D3DSIO_DCL = 31
K_D3DSIO_POW = 32
K_D3DSIO_CRS = 33
K_D3DSIO_SGN = 34
K_D3DSIO_ABS = 35
K_D3DSIO_NRM = 36
K_D3DSIO_SINCOS = 37
K_D3DSIO_REP = 38
K_D3DSIO_ENDREP = 39
K_D3DSIO_IF = 40
K_D3DSIO_IFC = 41
K_D3DSIO_ELSE = 42
K_D3DSIO_ENDIF = 43
K_D3DSIO_BREAK = 44
K_D3DSIO_BREAKC = 45
K_D3DSIO_MOVA = 46
K_D3DSIO_DEFB = 47
K_D3DSIO_DEFI = 48
K_D3DSIO_TEXCOORD = 64
K_D3DSIO_TEXKILL = 65
K_D3DSIO_TEX = 66
K_D3DSIO_TEXBEM = 67
K_D3DSIO_TEXBEML = 68
K_D3DSIO_TEXREG2AR = 69
K_D3DSIO_TEXREG2GB = 70
K_D3DSIO_TEXM3X2PAD = 71
K_D3DSIO_TEXM3X2TEX = 72
K_D3DSIO_TEXM3X3PAD = 73
K_D3DSIO_TEXM3X3TEX = 74
K_D3DSIO_TEXM3X3DIFF = 75
K_D3DSIO_TEXM3X3SPEC = 76
K_D3DSIO_TEXM3X3VSPEC = 77
K_D3DSIO_EXPP = 78
K_D3DSIO_LOGP = 79
K_D3DSIO_CND = 80
K_D3DSIO_DEF = 81
K_D3DSIO_TEXREG2RGB = 82
K_D3DSIO_TEXDP3TEX = 83
K_D3DSIO_TEXM3X2DEPTH = 84
K_D3DSIO_TEXDP3 = 85
K_D3DSIO_TEXM3X3 = 86
K_D3DSIO_TEXDEPTH = 87
K_D3DSIO_CMP = 88
K_D3DSIO_BEM = 89
K_D3DSIO_DP2ADD = 90
K_D3DSIO_DSX = 91
K_D3DSIO_DSY = 92
K_D3DSIO_TEXLDD = 93
K_D3DSIO_SETP = 94
K_D3DSIO_TEXLDL = 95
K_D3DSIO_BREAKP = 96
K_D3DSIO_PHASE = 0xFFFd
K_D3DSIO_COMMENT = 0xFFFe
K_D3DSIO_END = 0xFFFF

K_D3DSPR_INPUT = 1
K_D3DSPR_CONST = 2
K_D3DSPR_ADDR = 3
K_D3DSPR_CONSTINT = 7
K_D3DSPR_CONST2 = 11
K_D3DSPR_CONST3 = 12
K_D3DSPR_CONST4 = 13
K_D3DSPR_CONSTBOOL = 14
K_D3DSPR_LOOP = 15
FLOAT_CONST_TYPES = {
    K_D3DSPR_CONST,
    K_D3DSPR_CONST2,
    K_D3DSPR_CONST3,
    K_D3DSPR_CONST4,
}

BYTECODE_FILE_RE = re.compile(r"^shader-(?P<hash>\d+)\.(?P<kind>bin|txt)$")

OUTPUT_FIELDS = (
    "shader_hash",
    "shader_hash_hex",
    "stage_from_bytecode",
    "version",
    "major",
    "minor",
    "word_count",
    "unknown",
    "float_prefix_regs",
    "float_used_regs",
    "float_hole_regs",
    "float_max_index",
    "int_prefix_regs",
    "int_used_regs",
    "int_hole_regs",
    "bool_prefix_regs",
    "bool_used_regs",
    "bool_hole_regs",
    "indexed_float",
    "indexed_int",
    "indexed_bool",
    "indexed_float_accesses",
    "indexed_float_static_min",
    "indexed_float_static_max",
    "indexed_float_static_span",
    "indexed_float_static_offsets",
    "indexed_float_rel_sources",
    "indexed_int_accesses",
    "indexed_int_static_min",
    "indexed_int_static_max",
    "indexed_int_static_span",
    "indexed_int_static_offsets",
    "indexed_int_rel_sources",
    "indexed_bool_accesses",
    "indexed_bool_static_min",
    "indexed_bool_static_max",
    "indexed_bool_static_span",
    "indexed_bool_static_offsets",
    "indexed_bool_rel_sources",
    "vs_current_required_bytes",
    "vs_exact_packed_bytes",
    "vs_theoretical_gap_bytes",
    "vs_safe_packed_saves_bytes",
    "ps_current_required_bytes",
    "ps_exact_packed_bytes",
    "ps_theoretical_gap_bytes",
    "ps_safe_packed_saves_bytes",
    "safe_packed_possible",
    "path",
    "parse_error",
)

DRAW_FIELDS = (
    "stage",
    "shader_hash",
    "shader_hash_hex",
    "draws",
    "primitive_count",
    "vertex_count",
    "current_required_bytes_per_draw",
    "exact_packed_bytes_per_draw",
    "safe_packed_saves_bytes_per_draw",
    "theoretical_gap_bytes_per_draw",
    "weighted_current_required_bytes",
    "weighted_exact_packed_bytes",
    "weighted_safe_packed_saves_bytes",
    "weighted_theoretical_gap_bytes",
    "indexed_draws",
    "unknown_draws",
    "missing_draws",
    "indexed_float_static_offsets",
    "indexed_float_static_span",
    "indexed_float_rel_sources",
)


@dataclass
class Usage:
    float_regs: set[int] = field(default_factory=set)
    int_regs: set[int] = field(default_factory=set)
    bool_regs: set[int] = field(default_factory=set)
    indexed_float: bool = False
    indexed_int: bool = False
    indexed_bool: bool = False
    indexed_float_offsets: set[int] = field(default_factory=set)
    indexed_int_offsets: set[int] = field(default_factory=set)
    indexed_bool_offsets: set[int] = field(default_factory=set)
    indexed_float_rel_sources: set[str] = field(default_factory=set)
    indexed_int_rel_sources: set[str] = field(default_factory=set)
    indexed_bool_rel_sources: set[str] = field(default_factory=set)
    indexed_float_accesses: int = 0
    indexed_int_accesses: int = 0
    indexed_bool_accesses: int = 0
    unknown: bool = True


@dataclass
class ShaderDump:
    shader_hash: int
    path: Path
    words: list[int]


def parse_hash(value: Any) -> int:
    text = str(value or "").strip()
    if not text:
        return 0
    try:
        return int(text, 16) if text.startswith(("0x", "0X")) else int(text)
    except ValueError:
        return 0


def parse_int(value: Any) -> int:
    text = str(value or "").strip()
    if not text:
        return 0
    try:
        return int(float(text))
    except ValueError:
        return 0


def fmt_ratio(value: float) -> str:
    return f"{value:.3f}"


def summarize_int_set(values: set[int]) -> str:
    if not values:
        return ""
    return ";".join(str(value) for value in sorted(values))


def set_min(values: set[int]) -> str | int:
    return min(values) if values else ""


def set_max(values: set[int]) -> str | int:
    return max(values) if values else ""


def set_span(values: set[int]) -> int:
    return max(values) - min(values) + 1 if values else 0


def register_type_label(reg_type: int) -> str:
    if reg_type == K_D3DSPR_ADDR:
        return "a"
    if reg_type == K_D3DSPR_LOOP:
        return "aL"
    if reg_type == K_D3DSPR_INPUT:
        return "v"
    if reg_type in FLOAT_CONST_TYPES:
        return "c"
    if reg_type == K_D3DSPR_CONSTINT:
        return "i"
    if reg_type == K_D3DSPR_CONSTBOOL:
        return "b"
    return f"spr{reg_type}"


def rel_addr_component(token: int) -> int:
    return (token >> 16) & 0x3


def rel_addr_source_label(token: int) -> str:
    component = COMPONENT_NAMES[rel_addr_component(token)]
    return f"{register_type_label(register_type(token))}{register_index(token)}.{component}"


def load_words(path: Path) -> list[int]:
    if path.suffix == ".bin":
        data = path.read_bytes()
        if len(data) % 4 != 0:
            raise ValueError("bytecode binary length is not a multiple of 4")
        return list(struct.unpack("<" + "I" * (len(data) // 4), data))

    words: list[int] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.search(r":\s*0x([0-9a-fA-F]{8})\b", line)
        if match:
            words.append(int(match.group(1), 16))
    if not words:
        raise ValueError("no bytecode words found in text dump")
    return words


def index_bytecode_dumps(bytecode_dir: Path) -> dict[int, ShaderDump]:
    indexed: dict[int, ShaderDump] = {}
    if not bytecode_dir.exists():
        return indexed
    candidates: dict[int, dict[str, Path]] = {}
    for path in sorted(bytecode_dir.iterdir()):
        match = BYTECODE_FILE_RE.match(path.name)
        if not match:
            continue
        shader_hash = int(match.group("hash"))
        candidates.setdefault(shader_hash, {})[match.group("kind")] = path
    for shader_hash, paths in sorted(candidates.items()):
        path = paths.get("bin") or paths.get("txt")
        if path is None:
            continue
        indexed[shader_hash] = ShaderDump(shader_hash, path, load_words(path))
    return indexed


def register_type(token: int) -> int:
    return ((token >> 28) & 0x7) | (((token >> 11) & 0x3) << 3)


def register_index(token: int) -> int:
    return token & 0x7ff


def has_relative_addressing(token: int) -> bool:
    return ((token >> 13) & 0x1) != 0


def legacy_pixel_operand_count(
    opcode: int, major: int, minor: int, stage: str
) -> int | None:
    if stage != "ps" or major != 1:
        return None
    if opcode in (K_D3DSIO_TEXCOORD, K_D3DSIO_TEX):
        return 2 if minor >= 4 else 1
    if opcode == K_D3DSIO_TEXDEPTH:
        return 1
    if opcode in {
        K_D3DSIO_TEXBEM,
        K_D3DSIO_TEXBEML,
        K_D3DSIO_TEXREG2AR,
        K_D3DSIO_TEXREG2GB,
        K_D3DSIO_TEXM3X2PAD,
        K_D3DSIO_TEXM3X2TEX,
        K_D3DSIO_TEXM3X3PAD,
        K_D3DSIO_TEXM3X3TEX,
        K_D3DSIO_TEXM3X3DIFF,
        K_D3DSIO_TEXM3X3VSPEC,
        K_D3DSIO_TEXREG2RGB,
        K_D3DSIO_TEXDP3TEX,
        K_D3DSIO_TEXM3X2DEPTH,
        K_D3DSIO_TEXDP3,
        K_D3DSIO_TEXM3X3,
    }:
        return 2
    if opcode in (K_D3DSIO_TEXM3X3SPEC, K_D3DSIO_BEM):
        return 3
    return None


def fixed_operand_count(opcode: int) -> int:
    if opcode in {
        K_D3DSIO_NOP,
        K_D3DSIO_PHASE,
        K_D3DSIO_ELSE,
        K_D3DSIO_ENDIF,
        K_D3DSIO_ENDLOOP,
        K_D3DSIO_ENDREP,
        K_D3DSIO_RET,
        K_D3DSIO_BREAK,
    }:
        return 0
    if opcode in {
        K_D3DSIO_IF,
        K_D3DSIO_LOOP,
        K_D3DSIO_REP,
        K_D3DSIO_TEXKILL,
        K_D3DSIO_LABEL,
        K_D3DSIO_CALL,
    }:
        return 1
    if opcode in {
        K_D3DSIO_MOV,
        K_D3DSIO_DEFB,
        K_D3DSIO_RCP,
        K_D3DSIO_RSQ,
        K_D3DSIO_FRC,
        K_D3DSIO_DSX,
        K_D3DSIO_DSY,
        K_D3DSIO_SETP,
        K_D3DSIO_BREAKP,
        K_D3DSIO_MOVA,
        K_D3DSIO_LOG,
        K_D3DSIO_LOGP,
        K_D3DSIO_EXP,
        K_D3DSIO_EXPP,
        K_D3DSIO_SGN,
        K_D3DSIO_ABS,
        K_D3DSIO_NRM,
        K_D3DSIO_IFC,
        K_D3DSIO_BREAKC,
        K_D3DSIO_CALLNZ,
        K_D3DSIO_LIT,
        K_D3DSIO_DST,
    }:
        return 2
    if opcode in {
        K_D3DSIO_ADD,
        K_D3DSIO_SUB,
        K_D3DSIO_MUL,
        K_D3DSIO_DP3,
        K_D3DSIO_DP4,
        K_D3DSIO_MIN,
        K_D3DSIO_MAX,
        K_D3DSIO_POW,
        K_D3DSIO_CRS,
        K_D3DSIO_TEXLDL,
        K_D3DSIO_SLT,
        K_D3DSIO_SGE,
        K_D3DSIO_M4X4,
        K_D3DSIO_M4X3,
        K_D3DSIO_M3X4,
        K_D3DSIO_M3X3,
        K_D3DSIO_M3X2,
        K_D3DSIO_SINCOS,
    }:
        return 3
    if opcode in {
        K_D3DSIO_MAD,
        K_D3DSIO_LRP,
        K_D3DSIO_CND,
        K_D3DSIO_CMP,
        K_D3DSIO_DP2ADD,
    }:
        return 4
    if opcode in {K_D3DSIO_TEXLDD, K_D3DSIO_DEF, K_D3DSIO_DEFI}:
        return 5
    return 0


def operand_is_register(opcode: int, operand_index: int) -> bool:
    if opcode in {K_D3DSIO_DEF, K_D3DSIO_DEFI, K_D3DSIO_DEFB}:
        return operand_index == 0
    if opcode in {K_D3DSIO_LABEL, K_D3DSIO_CALL}:
        return False
    if opcode == K_D3DSIO_CALLNZ:
        return operand_index != 0
    return True


def opcode_writes_first_operand(opcode: int) -> bool:
    return opcode in {
        K_D3DSIO_DEF,
        K_D3DSIO_DEFI,
        K_D3DSIO_DEFB,
        K_D3DSIO_MOV,
        K_D3DSIO_ADD,
        K_D3DSIO_SUB,
        K_D3DSIO_MUL,
        K_D3DSIO_MAD,
        K_D3DSIO_MIN,
        K_D3DSIO_MAX,
        K_D3DSIO_SLT,
        K_D3DSIO_SGE,
        K_D3DSIO_EXP,
        K_D3DSIO_LOG,
        K_D3DSIO_EXPP,
        K_D3DSIO_LOGP,
        K_D3DSIO_M4X4,
        K_D3DSIO_M4X3,
        K_D3DSIO_M3X4,
        K_D3DSIO_M3X3,
        K_D3DSIO_M3X2,
        K_D3DSIO_RCP,
        K_D3DSIO_RSQ,
        K_D3DSIO_FRC,
        K_D3DSIO_LRP,
        K_D3DSIO_DP3,
        K_D3DSIO_DP4,
        K_D3DSIO_CND,
        K_D3DSIO_CMP,
        K_D3DSIO_DP2ADD,
        K_D3DSIO_POW,
        K_D3DSIO_CRS,
        K_D3DSIO_SGN,
        K_D3DSIO_ABS,
        K_D3DSIO_NRM,
        K_D3DSIO_TEX,
        K_D3DSIO_DSX,
        K_D3DSIO_DSY,
        K_D3DSIO_TEXLDD,
        K_D3DSIO_TEXLDL,
        K_D3DSIO_LIT,
        K_D3DSIO_DST,
    }


def matrix_rows(opcode: int) -> int:
    if opcode in (K_D3DSIO_M4X4, K_D3DSIO_M3X4):
        return 4
    if opcode in (K_D3DSIO_M4X3, K_D3DSIO_M3X3):
        return 3
    if opcode == K_D3DSIO_M3X2:
        return 2
    return 1


def stage_from_version(version: int) -> str:
    kind = (version >> 16) & 0xffff
    if kind == 0xFFFE:
        return "vs"
    if kind == 0xFFFF:
        return "ps"
    return "unknown"


def note_usage(
    usage: Usage,
    reg_type: int,
    index: int,
    indexed: bool,
    rel_token: int = 0,
    span: int = 1,
) -> None:
    regs = set(range(index, index + max(1, span)))
    if reg_type in FLOAT_CONST_TYPES:
        usage.float_regs.update(regs)
        usage.indexed_float = usage.indexed_float or indexed
        if indexed:
            usage.indexed_float_offsets.update(regs)
            usage.indexed_float_accesses += 1
            if rel_token:
                usage.indexed_float_rel_sources.add(rel_addr_source_label(rel_token))
    elif reg_type == K_D3DSPR_CONSTINT:
        usage.int_regs.update(regs)
        usage.indexed_int = usage.indexed_int or indexed
        if indexed:
            usage.indexed_int_offsets.update(regs)
            usage.indexed_int_accesses += 1
            if rel_token:
                usage.indexed_int_rel_sources.add(rel_addr_source_label(rel_token))
    elif reg_type == K_D3DSPR_CONSTBOOL:
        usage.bool_regs.update(regs)
        usage.indexed_bool = usage.indexed_bool or indexed
        if indexed:
            usage.indexed_bool_offsets.update(regs)
            usage.indexed_bool_accesses += 1
            if rel_token:
                usage.indexed_bool_rel_sources.add(rel_addr_source_label(rel_token))


def scan_usage(words: list[int]) -> Usage:
    usage = Usage()
    if not words:
        return usage
    version = words[0]
    stage = stage_from_version(version)
    major = (version >> 8) & 0xff
    minor = version & 0xff
    offset = 1
    usage.unknown = False

    while offset < len(words):
        token = words[offset]
        offset += 1
        opcode = token & 0xffff
        if opcode == K_D3DSIO_END:
            return usage
        if opcode == K_D3DSIO_COMMENT:
            comment_words = (token >> 16) & 0x7fff
            if offset + comment_words > len(words):
                usage.unknown = True
                return usage
            offset += comment_words
            continue
        if opcode == K_D3DSIO_PHASE:
            continue

        operand_count = legacy_pixel_operand_count(opcode, major, minor, stage)
        if operand_count is None:
            operand_count = fixed_operand_count(opcode)
        if operand_count == 0 and ((token >> 24) & 0xf) != 0:
            operand_count = (token >> 24) & 0xf
        if operand_count > 8 or offset + operand_count > len(words):
            usage.unknown = True
            return usage

        operands: list[int] = []
        indexed_operands: list[bool] = []
        rel_tokens: list[int] = []
        for i in range(operand_count):
            operand = words[offset]
            offset += 1
            operands.append(operand)
            indexed = False
            rel_token = 0
            if operand_is_register(opcode, i) and has_relative_addressing(operand):
                if offset >= len(words):
                    usage.unknown = True
                    return usage
                rel_token = words[offset]
                offset += 1
                indexed = True
            indexed_operands.append(indexed)
            rel_tokens.append(rel_token)

        source_begin = 0
        if opcode_writes_first_operand(opcode):
            source_begin = 1
            if operands:
                note_usage(
                    usage,
                    register_type(operands[0]),
                    register_index(operands[0]),
                    indexed_operands[0],
                    rel_tokens[0],
                )
        if opcode in {
            K_D3DSIO_DEF,
            K_D3DSIO_DEFI,
            K_D3DSIO_DEFB,
            K_D3DSIO_DCL,
            K_D3DSIO_LABEL,
            K_D3DSIO_CALL,
        }:
            source_begin = len(operands)
        for i in range(source_begin, len(operands)):
            span = matrix_rows(opcode) if i == 2 else 1
            note_usage(
                usage,
                register_type(operands[i]),
                register_index(operands[i]),
                indexed_operands[i],
                rel_tokens[i],
                span,
            )

    usage.unknown = True
    return usage


def prefix_count(values: set[int], limit: int) -> int:
    if not values:
        return 0
    return min(max(values) + 1, limit)


def full_bytes(stage: str) -> int:
    max_float = K_MAX_PIXEL_FLOAT if stage == "ps" else K_MAX_VERTEX_FLOAT
    return max_float * FLOAT4_BYTES + K_MAX_INT * INT4_BYTES + K_MAX_BOOL * BOOL_BYTES


def prefix_bytes(usage: Usage, stage: str) -> int:
    max_float = K_MAX_PIXEL_FLOAT if stage == "ps" else K_MAX_VERTEX_FLOAT
    float_count = prefix_count(usage.float_regs, max_float)
    int_count = prefix_count(usage.int_regs, K_MAX_INT)
    bool_count = prefix_count(usage.bool_regs, K_MAX_BOOL)
    float_end = float_count * FLOAT4_BYTES
    int_end = 0 if int_count == 0 else max_float * FLOAT4_BYTES + int_count * INT4_BYTES
    bool_end = (
        0
        if bool_count == 0
        else max_float * FLOAT4_BYTES + K_MAX_INT * INT4_BYTES + bool_count * BOOL_BYTES
    )
    return max(float_end, int_end, bool_end) or FLOAT4_BYTES


def current_required_bytes(usage: Usage, stage: str) -> int:
    if (
        usage.unknown
        or usage.indexed_float
        or usage.indexed_int
        or usage.indexed_bool
    ):
        return full_bytes(stage)
    return prefix_bytes(usage, stage)


def exact_packed_bytes(usage: Usage) -> int:
    bytes_used = (
        len(usage.float_regs) * FLOAT4_BYTES
        + len(usage.int_regs) * INT4_BYTES
        + len(usage.bool_regs) * BOOL_BYTES
    )
    return bytes_used or FLOAT4_BYTES


def safe_packed_possible(usage: Usage) -> bool:
    return not (
        usage.unknown
        or usage.indexed_float
        or usage.indexed_int
        or usage.indexed_bool
    )


def shader_row(shader: ShaderDump) -> dict[str, Any]:
    parse_error = ""
    try:
        usage = scan_usage(shader.words)
    except (IndexError, ValueError) as exc:
        usage = Usage()
        parse_error = str(exc)
    version = shader.words[0] if shader.words else 0
    stage = stage_from_version(version)
    float_limit = K_MAX_PIXEL_FLOAT if stage == "ps" else K_MAX_VERTEX_FLOAT
    max_float_index = max(usage.float_regs) if usage.float_regs else ""
    float_prefix_regs = prefix_count(usage.float_regs, float_limit)
    safe = safe_packed_possible(usage)
    vs_current = current_required_bytes(usage, "vs")
    ps_current = current_required_bytes(usage, "ps")
    packed = exact_packed_bytes(usage)
    return {
        "shader_hash": str(shader.shader_hash),
        "shader_hash_hex": f"0x{shader.shader_hash:x}",
        "stage_from_bytecode": stage,
        "version": f"0x{version:08x}",
        "major": (version >> 8) & 0xff,
        "minor": version & 0xff,
        "word_count": len(shader.words),
        "unknown": int(usage.unknown),
        "float_prefix_regs": float_prefix_regs,
        "float_used_regs": len(usage.float_regs),
        "float_hole_regs": max(0, float_prefix_regs - len(usage.float_regs)),
        "float_max_index": max_float_index,
        "int_prefix_regs": prefix_count(usage.int_regs, K_MAX_INT),
        "int_used_regs": len(usage.int_regs),
        "int_hole_regs": max(0, prefix_count(usage.int_regs, K_MAX_INT) - len(usage.int_regs)),
        "bool_prefix_regs": prefix_count(usage.bool_regs, K_MAX_BOOL),
        "bool_used_regs": len(usage.bool_regs),
        "bool_hole_regs": max(0, prefix_count(usage.bool_regs, K_MAX_BOOL) - len(usage.bool_regs)),
        "indexed_float": int(usage.indexed_float),
        "indexed_int": int(usage.indexed_int),
        "indexed_bool": int(usage.indexed_bool),
        "indexed_float_accesses": usage.indexed_float_accesses,
        "indexed_float_static_min": set_min(usage.indexed_float_offsets),
        "indexed_float_static_max": set_max(usage.indexed_float_offsets),
        "indexed_float_static_span": set_span(usage.indexed_float_offsets),
        "indexed_float_static_offsets": summarize_int_set(usage.indexed_float_offsets),
        "indexed_float_rel_sources": ";".join(sorted(usage.indexed_float_rel_sources)),
        "indexed_int_accesses": usage.indexed_int_accesses,
        "indexed_int_static_min": set_min(usage.indexed_int_offsets),
        "indexed_int_static_max": set_max(usage.indexed_int_offsets),
        "indexed_int_static_span": set_span(usage.indexed_int_offsets),
        "indexed_int_static_offsets": summarize_int_set(usage.indexed_int_offsets),
        "indexed_int_rel_sources": ";".join(sorted(usage.indexed_int_rel_sources)),
        "indexed_bool_accesses": usage.indexed_bool_accesses,
        "indexed_bool_static_min": set_min(usage.indexed_bool_offsets),
        "indexed_bool_static_max": set_max(usage.indexed_bool_offsets),
        "indexed_bool_static_span": set_span(usage.indexed_bool_offsets),
        "indexed_bool_static_offsets": summarize_int_set(usage.indexed_bool_offsets),
        "indexed_bool_rel_sources": ";".join(sorted(usage.indexed_bool_rel_sources)),
        "vs_current_required_bytes": vs_current,
        "vs_exact_packed_bytes": packed,
        "vs_theoretical_gap_bytes": max(0, vs_current - packed),
        "vs_safe_packed_saves_bytes": max(0, vs_current - packed) if safe else 0,
        "ps_current_required_bytes": ps_current,
        "ps_exact_packed_bytes": packed,
        "ps_theoretical_gap_bytes": max(0, ps_current - packed),
        "ps_safe_packed_saves_bytes": max(0, ps_current - packed) if safe else 0,
        "safe_packed_possible": int(safe),
        "path": str(shader.path),
        "parse_error": parse_error,
    }


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def unique_rows_by_hash(rows: Iterable[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    indexed: dict[int, dict[str, Any]] = {}
    for row in rows:
        shader_hash = parse_hash(row.get("shader_hash"))
        indexed[shader_hash] = row
    return indexed


def draw_aggregate_rows(
    probe_rows: list[dict[str, str]],
    shader_rows: dict[int, dict[str, Any]],
    seq_filter: str | None,
) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, int], dict[str, Any]] = {}
    for row in probe_rows:
        if seq_filter and row.get("seq") != seq_filter:
            continue
        for stage, field in (("vs", "vs"), ("ps", "ps")):
            shader_hash = parse_hash(row.get(field))
            if shader_hash == 0:
                continue
            key = (stage, shader_hash)
            out = grouped.setdefault(
                key,
                {
                    "stage": stage,
                    "shader_hash": str(shader_hash),
                    "shader_hash_hex": f"0x{shader_hash:x}",
                    "draws": 0,
                    "primitive_count": 0,
                    "vertex_count": 0,
                    "weighted_current_required_bytes": 0,
                    "weighted_exact_packed_bytes": 0,
                    "weighted_safe_packed_saves_bytes": 0,
                    "weighted_theoretical_gap_bytes": 0,
                    "indexed_draws": 0,
                    "unknown_draws": 0,
                    "missing_draws": 0,
                    "indexed_float_static_offsets": "",
                    "indexed_float_static_span": 0,
                    "indexed_float_rel_sources": "",
                },
            )
            out["draws"] += 1
            out["primitive_count"] += parse_int(row.get("primitive_count"))
            out["vertex_count"] += parse_int(row.get("vertex_count"))
            shader = shader_rows.get(shader_hash)
            if shader is None:
                out["missing_draws"] += 1
                continue
            out["indexed_float_static_offsets"] = shader.get("indexed_float_static_offsets", "")
            out["indexed_float_static_span"] = shader.get("indexed_float_static_span", 0)
            out["indexed_float_rel_sources"] = shader.get("indexed_float_rel_sources", "")
            current = parse_int(shader.get(f"{stage}_current_required_bytes"))
            packed = parse_int(shader.get(f"{stage}_exact_packed_bytes"))
            saved = parse_int(shader.get(f"{stage}_safe_packed_saves_bytes"))
            gap = parse_int(shader.get(f"{stage}_theoretical_gap_bytes"))
            out["weighted_current_required_bytes"] += current
            out["weighted_exact_packed_bytes"] += packed
            out["weighted_safe_packed_saves_bytes"] += saved
            out["weighted_theoretical_gap_bytes"] += gap
            if parse_int(shader.get("unknown")):
                out["unknown_draws"] += 1
            if (
                parse_int(shader.get("indexed_float"))
                or parse_int(shader.get("indexed_int"))
                or parse_int(shader.get("indexed_bool"))
            ):
                out["indexed_draws"] += 1
    for out in grouped.values():
        draws = max(1, parse_int(out.get("draws")))
        out["current_required_bytes_per_draw"] = fmt_ratio(
            parse_int(out.get("weighted_current_required_bytes")) / draws
        )
        out["exact_packed_bytes_per_draw"] = fmt_ratio(
            parse_int(out.get("weighted_exact_packed_bytes")) / draws
        )
        out["safe_packed_saves_bytes_per_draw"] = fmt_ratio(
            parse_int(out.get("weighted_safe_packed_saves_bytes")) / draws
        )
        out["theoretical_gap_bytes_per_draw"] = fmt_ratio(
            parse_int(out.get("weighted_theoretical_gap_bytes")) / draws
        )
    return sorted(
        grouped.values(),
        key=lambda item: (
            parse_int(item.get("weighted_safe_packed_saves_bytes")),
            parse_int(item.get("weighted_theoretical_gap_bytes")),
            parse_int(item.get("draws")),
        ),
        reverse=True,
    )


def write_csv(path: Path, rows: list[dict[str, Any]], fields: tuple[str, ...]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def sum_int(rows: list[dict[str, Any]], key: str) -> int:
    return sum(parse_int(row.get(key)) for row in rows)


def write_markdown(
    path: Path,
    bytecode_dir: Path,
    shader_rows: list[dict[str, Any]],
    draw_rows: list[dict[str, Any]],
    probe_draws: Path | None,
    top: int,
) -> None:
    safe_rows = [row for row in shader_rows if parse_int(row.get("safe_packed_possible"))]
    indexed_rows = [
        row for row in shader_rows
        if parse_int(row.get("indexed_float"))
        or parse_int(row.get("indexed_int"))
        or parse_int(row.get("indexed_bool"))
    ]
    total_vs_current = sum_int(shader_rows, "vs_current_required_bytes")
    total_vs_saved = sum_int(shader_rows, "vs_safe_packed_saves_bytes")
    total_vs_gap = sum_int(shader_rows, "vs_theoretical_gap_bytes")
    total_ps_current = sum_int(shader_rows, "ps_current_required_bytes")
    total_ps_saved = sum_int(shader_rows, "ps_safe_packed_saves_bytes")
    total_ps_gap = sum_int(shader_rows, "ps_theoretical_gap_bytes")

    lines = [
        "# Shader Constant Sparsity Report",
        "",
        f"- Bytecode dir: `{bytecode_dir}`",
        f"- Shader bytecode dumps: `{len(shader_rows)}`",
        f"- Safe non-indexed packed candidates: `{len(safe_rows)}`",
        f"- Indexed/unknown fallback candidates: `{len(shader_rows) - len(safe_rows)}`",
        f"- Indexed constant shaders: `{len(indexed_rows)}`",
        f"- Corpus VS current/safe-save bytes: `{total_vs_current}` / `{total_vs_saved}`",
        f"- Corpus VS theoretical gap bytes: `{total_vs_gap}`",
        f"- Corpus PS current/safe-save bytes: `{total_ps_current}` / `{total_ps_saved}`",
        f"- Corpus PS theoretical gap bytes: `{total_ps_gap}`",
        "",
        "## Top Shader Candidates",
        "",
        "| Shader | Stage | float prefix/used/hole | indexed | indexed static offsets | rel sources | VS current | VS packed | VS gap | VS safe-save | PS current | PS packed | PS gap | PS safe-save |",
        "|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    sorted_shader_rows = sorted(
        shader_rows,
        key=lambda row: max(
            parse_int(row.get("vs_safe_packed_saves_bytes")),
            parse_int(row.get("ps_safe_packed_saves_bytes")),
            parse_int(row.get("vs_theoretical_gap_bytes")),
            parse_int(row.get("ps_theoretical_gap_bytes")),
        ),
        reverse=True,
    )
    for row in sorted_shader_rows[:top]:
        indexed = (
            parse_int(row.get("indexed_float"))
            + parse_int(row.get("indexed_int"))
            + parse_int(row.get("indexed_bool"))
        )
        lines.append(
            "| {shader} | {stage} | {prefix}/{used}/{holes} | {indexed} | {offsets} | {sources} | "
            "{vs_current} | {vs_packed} | {vs_gap} | {vs_save} | "
            "{ps_current} | {ps_packed} | {ps_gap} | {ps_save} |".format(
                shader=row.get("shader_hash_hex", ""),
                stage=row.get("stage_from_bytecode", ""),
                prefix=row.get("float_prefix_regs", ""),
                used=row.get("float_used_regs", ""),
                holes=row.get("float_hole_regs", ""),
                indexed=indexed,
                offsets=row.get("indexed_float_static_offsets", "") or "-",
                sources=row.get("indexed_float_rel_sources", "") or "-",
                vs_current=row.get("vs_current_required_bytes", ""),
                vs_packed=row.get("vs_exact_packed_bytes", ""),
                vs_gap=row.get("vs_theoretical_gap_bytes", ""),
                vs_save=row.get("vs_safe_packed_saves_bytes", ""),
                ps_current=row.get("ps_current_required_bytes", ""),
                ps_packed=row.get("ps_exact_packed_bytes", ""),
                ps_gap=row.get("ps_theoretical_gap_bytes", ""),
                ps_save=row.get("ps_safe_packed_saves_bytes", ""),
            )
        )

    if probe_draws:
        lines.extend([
            "",
            "## Draw-Weighted Estimate",
            "",
            f"- Probe draws: `{probe_draws}`",
            f"- Joined shader rows: `{len(draw_rows)}`",
            f"- Weighted current bytes/draw sum: `{sum_int(draw_rows, 'weighted_current_required_bytes')}`",
            f"- Weighted safe-save bytes/draw sum: `{sum_int(draw_rows, 'weighted_safe_packed_saves_bytes')}`",
            f"- Weighted theoretical gap bytes/draw sum: `{sum_int(draw_rows, 'weighted_theoretical_gap_bytes')}`",
            "",
            "| Stage | Shader | draws | prims | current/draw | packed/draw | gap/draw | safe-save/draw | indexed draws | indexed static offsets | rel sources | missing |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|",
        ])
        for row in draw_rows[:top]:
            lines.append(
                "| {stage} | {shader} | {draws} | {prims} | {current} | "
                "{packed} | {gap} | {save} | {indexed} | {offsets} | {sources} | {missing} |".format(
                    stage=row.get("stage", ""),
                    shader=row.get("shader_hash_hex", ""),
                    draws=row.get("draws", ""),
                    prims=row.get("primitive_count", ""),
                    current=row.get("current_required_bytes_per_draw", ""),
                    packed=row.get("exact_packed_bytes_per_draw", ""),
                    gap=row.get("theoretical_gap_bytes_per_draw", ""),
                    save=row.get("safe_packed_saves_bytes_per_draw", ""),
                    indexed=row.get("indexed_draws", ""),
                    offsets=row.get("indexed_float_static_offsets", "") or "-",
                    sources=row.get("indexed_float_rel_sources", "") or "-",
                    missing=row.get("missing_draws", ""),
                )
            )

    lines.extend([
        "",
        "## Interpretation",
        "",
        "- `current` is the byte count required by the existing prefix/full-array cbuf ABI.",
        "- `packed` is the lower bound for a shader-specific packed constant ABI.",
        "- `gap` is theoretical only. It includes indexed shaders that are not safe to pack with the current translator.",
        "- `safe-save` is zero for unknown or indexed constant access, because dynamic `c[a0+n]` needs the current full array unless the shader translator proves a narrower dynamic window.",
        "- `indexed static offsets` are only the literal `+n` side of `c[a0+n]`. They do not bound the runtime `a0` value.",
        "- The draw-weighted table is a shader-use opportunity estimate. It is not a direct cbuf-upload byte forecast because constant uploads are driven by dirty/hash churn, not every draw.",
        "",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bytecode-dir", type=Path, required=True)
    parser.add_argument("--probe-draws", type=Path)
    parser.add_argument("--seq", help="Optional probe-draw seq filter, e.g. 60")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path, required=True)
    parser.add_argument("--draw-csv-output", type=Path)
    parser.add_argument("--top", type=int, default=12)
    args = parser.parse_args()

    if args.top <= 0:
        raise SystemExit("--top must be positive")

    dumps = index_bytecode_dumps(args.bytecode_dir)
    shader_rows = [shader_row(dump) for dump in dumps.values()]
    shader_rows.sort(
        key=lambda row: max(
            parse_int(row.get("vs_safe_packed_saves_bytes")),
            parse_int(row.get("ps_safe_packed_saves_bytes")),
            parse_int(row.get("vs_theoretical_gap_bytes")),
            parse_int(row.get("ps_theoretical_gap_bytes")),
        ),
        reverse=True,
    )

    draw_rows: list[dict[str, Any]] = []
    if args.probe_draws and args.probe_draws.exists():
        draw_rows = draw_aggregate_rows(
            read_csv(args.probe_draws),
            unique_rows_by_hash(shader_rows),
            args.seq,
        )

    write_csv(args.csv_output, shader_rows, OUTPUT_FIELDS)
    if args.draw_csv_output:
        write_csv(args.draw_csv_output, draw_rows, DRAW_FIELDS)
    write_markdown(
        args.output,
        args.bytecode_dir,
        shader_rows,
        draw_rows,
        args.probe_draws,
        args.top,
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
