#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"$repo_root/scripts/run_python.sh" "$repo_root/scripts/tools/shader_corpus_tool.py" drift "$@"
