#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten
#
# Assembles the project site (site/README.md) into an output directory: the landing page, the
# WebGL demo from an Emscripten build of the OpenGL sample with the sample packs, and the API
# reference with Doxygen. Run from the repository root:
#
#     site/build.sh <emscripten bin directory> <output directory>
#
# for example site/build.sh build/emscripten/bin/Release build/site.

set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: site/build.sh <emscripten bin directory> <output directory>" >&2
    exit 2
fi
web=$1
out=$2

rm -rf "$out"
mkdir -p "$out"

# The landing page and its files, without the build's own inputs.
cp -r site/. "$out/"
rm -f "$out/README.md" "$out/build.sh" "$out/Doxyfile" "$out/api-main.md"

# The demo: the OpenGL sample's page, loading coi.js first so that it is cross-origin isolated
# on a host without the headers, and the sample packs at data/packs beside it, where the page
# looks without a pack in its address.
for file in vgl-gl.html vgl-gl.js vgl-gl.wasm vgl-gl.data; do
    cp "$web/$file" "$out/demo/"
done
sed -i 's#<head>#<head><script src=coi.js></script>#' "$out/demo/vgl-gl.html"
if ! grep -q 'src=coi.js' "$out/demo/vgl-gl.html"; then
    echo "error: could not add coi.js to the demo's page" >&2
    exit 1
fi
mkdir -p "$out/demo/data"
cp -r samples/data/packs "$out/demo/data/"
# GitHub Pages compresses unknown file types and then serves ranges of the compressed stream,
# so the packs' files get a .zip suffix, a type it serves as it is; coi-sw.js maps the demo's
# reads to them.
find "$out/demo/data/packs" -type f -exec mv {} {}.zip \;

# The API reference, at api/. Any warning fails the build, so that the reference stays whole.
version=$(sed -nE 's/^ *VERSION ([0-9]+\.[0-9]+\.[0-9]+)$/\1/p' CMakeLists.txt | head -1)
doxygen_out=$(mktemp -d)
VGL_VERSION="$version" VGL_DOXYGEN_OUTPUT="$doxygen_out" doxygen site/Doxyfile
if [ -s "$doxygen_out/doxygen-warnings.txt" ]; then
    cat "$doxygen_out/doxygen-warnings.txt" >&2
    echo "error: Doxygen warned" >&2
    exit 1
fi
mv "$doxygen_out/api" "$out/api"
rm -rf "$doxygen_out"

# GitHub Pages serves the files as they are.
touch "$out/.nojekyll"
echo "site: $out (vgl $version)"
