"""Names the function behind a crash offset from Windows' event log, using the linker map.

Usage: crash_at.py 0x19376a [more offsets]
"""
import bisect
import os
import re
import sys

MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "BM64Recompiled.map")
IMAGE_BASE = 0x140000000
ENTRY = re.compile(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(.*)$")


def main():
    addresses, names = [], []
    with open(MAP, errors="ignore") as f:
        for line in f:
            m = ENTRY.match(line)
            if m:
                addresses.append(int(m.group(2), 16))
                names.append((m.group(1), m.group(3).strip()))
    order = sorted(range(len(addresses)), key=addresses.__getitem__)
    addresses = [addresses[i] for i in order]
    names = [names[i] for i in order]
    for text in sys.argv[1:]:
        target = IMAGE_BASE + int(text, 16)
        i = bisect.bisect_right(addresses, target) - 1
        print(f"{text}: {names[i][0]} +{target - addresses[i]:#x} ({names[i][1]})")


if __name__ == "__main__":
    main()
