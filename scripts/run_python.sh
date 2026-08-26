#!/usr/bin/env bash
set -euo pipefail

# The project environment is an implementation detail owned by uv. Resolve the
# optional mise-pinned uv first and a PATH uv second, then let uv own Python and
# the project environment without requiring activation or PATH mutation.
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
caller_pwd=$PWD
python_request=$(<"$repo_root/.python-version")

# Child processes launched by this wrapper inherit uv's project environment.
# Reuse it without starting mise and uv again.
if [[ "${VIRTUAL_ENV:-}" == "$repo_root/.venv" &&
      -x "$VIRTUAL_ENV/bin/python" ]]; then
  exec "$VIRTUAL_ENV/bin/python" "$@"
fi

uv_args=(
  run
  --project "$repo_root"
  --directory "$caller_pwd"
  --locked
  --managed-python
  --python "$python_request"
  python
)

if command -v mise >/dev/null 2>&1; then
  mise_uv=$(cd -- "$repo_root" && mise which uv 2>/dev/null || true)
  if [[ -x "$mise_uv" ]]; then
    exec mise exec -C "$repo_root" -- uv "${uv_args[@]}" "$@"
  fi
fi

if path_uv=$(command -v uv 2>/dev/null); then
  exec "$path_uv" "${uv_args[@]}" "$@"
fi

echo "dxmt9 requires uv: install uv or run 'mise install'" >&2
exit 2
