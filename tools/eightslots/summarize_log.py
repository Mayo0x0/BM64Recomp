"""Prints the flow of a battle test log: one line whenever the flags, the alive count or the out list change."""
import re
import sys

STATE = re.compile(r"TEST f=(\d+) flags=(\w+) alive=(-?\d+) t=(-?\d+):(-?\d+)(.*)")
SLOT = re.compile(r"\| (\d) c(\d)")


def main():
    last = None
    with open(sys.argv[1], encoding="utf-8-sig") as f:
        for line in f:
            m = STATE.match(line)
            if not m:
                continue
            out = "".join(slot for slot, control in SLOT.findall(m.group(6)) if control == "3")
            key = (m.group(2), m.group(3), out)
            if key != last:
                print(f"f={m.group(1)} flags={m.group(2)} alive={m.group(3)} t={m.group(4)}:{m.group(5)} out={out}")
            last = key


if __name__ == "__main__":
    main()
