#!/usr/bin/env bash
# Install the repository's commit-message check in this checkout when possible.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
cd "$root"
if ! command -v node >/dev/null 2>&1; then
    echo 'Node is unavailable; commit-msg hook was not installed.' >&2
    exit 0
fi
if ! command -v npm >/dev/null 2>&1; then
    echo 'Node is available but npm is missing; cannot install commit-msg hook.' >&2
    exit 1
fi
npm ci --ignore-scripts
env -u HUSKY ./node_modules/.bin/husky
