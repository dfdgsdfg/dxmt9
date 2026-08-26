#!/usr/bin/env bash
set -euo pipefail

# The project environment is an implementation detail owned by uv. Resolve the
# pinned tools through mise, then let uv create or synchronize that environment
# before executing Python without requiring activation or PATH mutation.
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
caller_pwd=$PWD

# Child processes launched by this wrapper inherit uv's project environment.
# Reuse it without starting mise and uv again.
if [[ "${VIRTUAL_ENV:-}" == "$repo_root/.venv" &&
      -x "$VIRTUAL_ENV/bin/python" ]]; then
  exec "$VIRTUAL_ENV/bin/python" "$@"
fi

exec mise exec -C "$repo_root" -- uv run \
  --project "$repo_root" \
  --directory "$caller_pwd" \
  --locked \
  --python python \
  python "$@"
