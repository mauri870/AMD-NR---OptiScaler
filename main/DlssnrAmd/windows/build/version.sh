#!/usr/bin/env bash
# The version packages and DLLs are stamped with, from the git tags (tag v0.0.1 -> 0.0.1):
#   0.0.1                  a tagged commit
#   0.0.1-3-g1a2b3c4       three commits after v0.0.1
#   0.0.0-g1a2b3c4         before the first tag
# "-dirty" is appended when tracked files have uncommitted changes. NR_VERSION overrides it.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
if [[ -n "${NR_VERSION:-}" ]]; then
    echo "$NR_VERSION"
elif v=$(git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null); then
    echo "${v#v}"
elif v=$(git describe --always --dirty 2>/dev/null); then
    echo "0.0.0-g$v"
else
    echo "0.0.0-unknown"
fi
