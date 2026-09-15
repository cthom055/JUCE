#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "These tests require Linux." >&2
    exit 1
fi

test_dir="$(cd "$(dirname "$0")" && pwd)"
test_build="$(mktemp -d)"
trap 'rm -rf "$test_build"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    "$test_dir/linux-webview-transport-tests.cpp" -o "$test_build/linux-webview-transport-tests"
timeout 15 "$test_build/linux-webview-transport-tests"
