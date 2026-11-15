#!/bin/sh
set -e
TOOLS="${CACHE_DIR:-/cache}/tools"
[ -w "$(dirname "$TOOLS")" ] || TOOLS="$HOME/.luma-ci-tools"
mkdir -p "$TOOLS"
if ! command -v cmake >/dev/null 2>&1; then
  if [ ! -x "$TOOLS/cmake/bin/cmake" ]; then
    curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz -o "$TOOLS/cmake.tgz"
    mkdir -p "$TOOLS/cmake" && tar xzf "$TOOLS/cmake.tgz" -C "$TOOLS/cmake" --strip-components=1 && rm "$TOOLS/cmake.tgz"
  fi
  PATH="$TOOLS/cmake/bin:$PATH"
fi
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci -j 2
cd build-ci && ctest --output-on-failure
