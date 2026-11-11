#!/bin/bash
# Runs the battle with 4 and with 8 players and logs which words of the globals change (WATCH_CHANGES).
# watch_diff.py then lists the words only the 8 player runs change.
# Usage: watch_runs.sh <toolchain folder> <test folder> <stage>...
set -u
T="$1"; D="$2"; shift 2
cd "$(dirname "$0")/testmod"
for stage in "$@"; do
    for bots in 0 4; do
        PATH="$T/portable-llvm/LLVM-19.1.3-Windows-X64/bin:$PATH" /c/devkitPro/msys2/usr/bin/make -B \
            EXTRA_DEFINES="-DPLAYERS_ADDRESS=0x80700000 -DTEST_STAGE=$stage -DTEST_MINUTES=2 -DTEST_BOTS=$bots -DWATCH_CHANGES=1200" \
            2>&1 | grep -i " error" && exit 1
        "$T/N64RecompSource/cmake-build/RecompModTool.exe" mod.toml build > /dev/null
        powershell -NoProfile -File ../run_battle_test.ps1 -Label "watch${stage}_$bots" -Seconds 100 -TestDir "$D"
    done
done
