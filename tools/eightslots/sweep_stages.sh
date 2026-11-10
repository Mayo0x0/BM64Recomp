#!/bin/bash
# Runs the battle test on every stage. Usage: sweep_stages.sh <toolchain folder> <test folder> <seconds> <minutes>
TOOLS="${1:?toolchain folder}"
TEST_DIR="${2:?test folder}"
SECONDS_EACH="${3:-60}"
MINUTES="${4:-1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LLVM="$TOOLS/portable-llvm/LLVM-19.1.3-Windows-X64/bin"

for stage in 0 1 2 3 4 5 6 7 8 9; do
    (cd "$HERE/testmod" && /c/devkitPro/msys2/usr/bin/make -B \
        EXTRA_DEFINES="-DPLAYERS_ADDRESS=0x80700000 -DTEST_STAGE=$stage -DTEST_MINUTES=$MINUTES" \
        CC="$LLVM/clang.exe" LD="$LLVM/ld.lld.exe" OS= > "$TOOLS/testmod.log" 2>&1 \
        && "$TOOLS/N64RecompSource/cmake-build/RecompModTool.exe" mod.toml build > /dev/null 2>&1) \
        || { echo "stage $stage: test mod failed"; continue; }
    label="stage${stage}m${MINUTES}"
    echo "=== stage $stage"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/test_battle.ps1")" \
        -Label "$label" -TestDir "$(cygpath -w "$TEST_DIR")" -Players 8 -Seconds "$SECONDS_EACH" 2>&1 \
        | grep -E 'FAIL|RESULT|CRASH|out=|func_8|func_ovl' | grep -v 'print_crash' | cut -c1-90 | head -12
done
