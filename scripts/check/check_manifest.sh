#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
manifest="$repo_root/tests/shader_runner/corpus/MANIFEST.toml"
tests_dir="$repo_root/tests/shader_runner/corpus"

if [[ ! -f "$manifest" ]]; then
  echo "manifest missing: $manifest" >&2
  exit 1
fi

actual_files=()
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  actual_files+=("$file")
done < <(find "$tests_dir" -name '*.shader_test' -type f | sed "s#^$repo_root/tests/shader_runner/corpus/##" | sort)

manifest_files=()
while IFS= read -r file; do
  [[ -z "$file" ]] && continue
  manifest_files+=("$file")
done < <(grep -E '^[[:space:]]*file[[:space:]]*=' "$manifest" | sed -E 's/.*"([^"]+)".*/\1/' | sort)

diff_output="$(
  comm -3 \
    <(printf '%s\n' "${actual_files[@]}") \
    <(printf '%s\n' "${manifest_files[@]}") || true
)"
if [[ -n "$diff_output" ]]; then
  echo "manifest mismatch:" >&2
  printf '%s\n' "$diff_output" >&2
  exit 1
fi

python3 - "$manifest" <<'PY'
from __future__ import annotations

import sys
import tomllib
from pathlib import Path

manifest = Path(sys.argv[1])
required = {"file", "status", "source", "source_kind", "license", "license_scope", "models", "opcodes", "oracle", "oracle-date"}
valid_source_kind = {
    "project-authored",
    "behavioral-oracle",
    "structure-reference",
    "third-party-fixture",
    "implementation-source",
}
valid_license_scope = {"project-mit", "third-party-fixture", "external-not-vendored"}
valid_status = {"passing", "failing", "skipped"}
valid_models = {
    "ffp",
    "hlsl",
    "ps_1_1",
    "ps_1_2",
    "ps_1_3",
    "ps_1_4",
    "ps_2_0",
    "ps_3_0",
    "vs_1_1",
    "vs_2_0",
    "vs_3_0",
}
valid_opcodes = {
    "ABS", "ADD", "ALPHA_TEST", "BREAK", "BREAKC", "BREAKP", "CALL", "CALLNZ",
    "CLEAR", "CMP", "CND", "CRS", "DCL", "DEF", "DEFB", "DEFI", "DP2ADD",
    "DP3", "DP4", "DSX", "DSY", "ELSE", "ENDIF", "ENDLOOP", "ENDREP", "EXP",
    "EXPP", "FRC", "HLSL", "IF", "IFC", "LABEL", "LIT", "LOG", "LOGP", "LOOP",
    "LRP", "M3x2", "M3x3", "M3x4", "M4x3", "M4x4", "MAD", "MAX", "MIN",
    "MOV", "MOVA", "MUL", "NOP", "NRM", "POW", "PROBE", "RCP", "REP",
    "RET", "RSQ", "SETP", "SGE", "SGN", "SINCOS", "SLT", "SUB", "TEX",
    "TEXBEM", "TEXBEML", "TEXCOORD", "TEXDEPTH", "TEXDP3", "TEXDP3TEX",
    "TEXKILL", "TEXLDD", "TEXLDL", "TEXM3x2DEPTH", "TEXM3x2PAD",
    "TEXM3x2TEX", "TEXM3x3", "TEXM3x3DIFF", "TEXM3x3PAD",
    "TEXM3x3SPEC", "TEXM3x3TEX", "TEXM3x3VSPEC", "TEXREG2AR",
    "TEXREG2GB", "TEXREG2RGB",
}

data = tomllib.loads(manifest.read_text())
errors: list[str] = []
for index, entry in enumerate(data.get("test", []), 1):
    missing = sorted(required - entry.keys())
    if missing:
        errors.append(f"manifest test #{index}: missing fields: {', '.join(missing)}")
    if entry.get("source_kind") not in valid_source_kind:
        errors.append(f"manifest test #{index}: invalid source_kind {entry.get('source_kind')!r}")
    if entry.get("license_scope") not in valid_license_scope:
        errors.append(f"manifest test #{index}: invalid license_scope {entry.get('license_scope')!r}")
    if entry.get("license_scope") == "project-mit" and entry.get("license") != "MIT":
        errors.append(f"manifest test #{index}: project-mit entries must use license = \"MIT\"")
    if entry.get("status") not in valid_status:
        errors.append(f"manifest test #{index}: invalid status {entry.get('status')!r}")
    models = entry.get("models")
    if not isinstance(models, list) or not models:
        errors.append(f"manifest test #{index}: models must be a non-empty array")
    else:
        invalid = sorted(model for model in models if not isinstance(model, str) or model not in valid_models)
        if invalid:
            errors.append(f"manifest test #{index}: invalid models: {', '.join(map(repr, invalid))}")
    opcodes = entry.get("opcodes")
    if not isinstance(opcodes, list) or not opcodes:
        errors.append(f"manifest test #{index}: opcodes must be a non-empty array")
    else:
        invalid = sorted(opcode for opcode in opcodes if not isinstance(opcode, str) or opcode not in valid_opcodes)
        if invalid:
            errors.append(f"manifest test #{index}: invalid opcodes: {', '.join(map(repr, invalid))}")

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    raise SystemExit(1)
PY

missing_provenance=0
for file in "${actual_files[@]}"; do
  full_path="$tests_dir/$file"
  source_line="$(grep -m1 '^; source:' "$full_path" || true)"
  source_value="${source_line#; source: }"
  if ! grep -q '^; \[provenance\]' "$full_path" || ! grep -q '^; source:' "$full_path" || ! grep -q '^; source_kind:' "$full_path" || ! grep -q '^; license:' "$full_path" || ! grep -q '^; license_scope:' "$full_path" || ! grep -q '^; oracle:' "$full_path" || ! grep -q '^; oracle-date:' "$full_path"; then
    echo "missing provenance fields: $file" >&2
    missing_provenance=1
  elif [[ "$source_value" == vkd3d || "$source_value" == wine/visual.c:* ]]; then
    if ! grep -q '^; upstream-commit:' "$full_path"; then
      echo "missing upstream provenance: $file" >&2
      missing_provenance=1
    fi
  fi
done

if [[ "$missing_provenance" -ne 0 ]]; then
  exit 1
fi

echo "manifest ok: ${#actual_files[@]} shader tests"
