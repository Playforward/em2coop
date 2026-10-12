#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build
i686-w64-mingw32-g++ \
  -std=c++17 -O2 -Wall -Wextra \
  -static-libgcc -static-libstdc++ -static \
  -DWIN32_LEAN_AND_MEAN \
  -s -shared -Wl,--enable-stdcall-fixup -Wl,--exclude-all-symbols \
  -o build/binkw32.dll \
  src/main.cpp src/util.cpp src/config.cpp src/game_thread.cpp \
  src/players.cpp src/coop.cpp src/input.cpp src/split_screen.cpp \
  src/world_views.cpp src/fixes.cpp src/proxy_exports.def \
  -luser32

echo "built build/binkw32.dll ($(stat -c %s build/binkw32.dll) bytes)"
