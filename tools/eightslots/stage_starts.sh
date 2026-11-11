#!/bin/bash
# Builds the test mod for each battle stage and logs the tile under every bomber at the start.
# Usage: stage_starts.sh <toolchain folder> <test folder>
set -u
T="$1"; D="$2"
cd "$(dirname "$0")/testmod"
for stage in 0 1 2 3 4 5 6 7 8 9; do
    PATH="$T/portable-llvm/LLVM-19.1.3-Windows-X64/bin:$PATH" /c/devkitPro/msys2/usr/bin/make -B \
        EXTRA_DEFINES="-DPLAYERS_ADDRESS=0x80700000 -DTEST_STAGE=$stage -DTEST_MINUTES=2" 2>&1 | grep -i " error" && exit 1
    "$T/N64RecompSource/cmake-build/RecompModTool.exe" mod.toml build > /dev/null
    powershell -NoProfile -File ../run_battle_test.ps1 -Label "start$stage" -Seconds 14 -TestDir "$D" > /dev/null
    echo "stage $stage: $(grep -a 'TEST tiles' ../logs/start$stage.test.txt | head -1)"
done
