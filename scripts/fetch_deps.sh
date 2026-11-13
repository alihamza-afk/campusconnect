#!/bin/sh
# Downloads the two third-party headers the backend needs, and verifies their
# SHA-256 checksums. Same versions and checksums as the Dockerfile, so a native
# build and a container build use byte-identical library code.
#
# Usage: sh scripts/fetch_deps.sh [target-dir]     (default: backend)
set -eu

dir="${1:-backend}"
httplib_url="https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h"
httplib_sha="a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5"
json_url="https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
json_sha="9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

fetch() {
    file="$1"
    url="$2"
    expected="$3"
    if [ -f "$file" ] && [ "$(sha256_of "$file")" = "$expected" ]; then
        return 0
    fi
    echo "downloading $(basename "$file") ..."
    curl -fsSL -o "$file" "$url"
    actual="$(sha256_of "$file")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$file"
        echo "ERROR: checksum mismatch for $file" >&2
        echo "  expected $expected" >&2
        echo "  got      $actual" >&2
        exit 1
    fi
}

mkdir -p "$dir"
fetch "$dir/httplib.h" "$httplib_url" "$httplib_sha"
fetch "$dir/json.hpp" "$json_url" "$json_sha"
echo "dependencies ready in $dir/"
