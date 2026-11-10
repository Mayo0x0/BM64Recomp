#!/bin/bash
# Generates the patches and rebuilds the game. Usage: build.sh <folder with llvm, N64RecompSource, build_game.bat>
# The folder holds the toolchain described in tools/eightslots/README.md.
set -u
TOOLS="${1:?toolchain folder}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

python -I tools/eightslots/gen_patches.py || exit 1
./N64Recomp.exe bm64.eightslots.toml > /dev/null 2>"$TOOLS/recomp.log" || { tail -5 "$TOOLS/recomp.log"; exit 1; }

# CMake does not know that the C patches depend on the generated symbol file, nor that
# register_patches.cpp includes their generated function table.
touch patches/required_patches.c src/main/register_patches.cpp

cmd.exe //c "$(cygpath -w "$TOOLS/build_game.bat")" > "$TOOLS/build.log" 2>&1
status=$?
grep -E 'error:|FAILED' "$TOOLS/build.log" | cut -c1-200 | head -8
tail -1 "$TOOLS/build.log" | cut -c1-100
exit $status
