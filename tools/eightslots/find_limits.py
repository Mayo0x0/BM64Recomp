"""Lists the constants 3, 4 and 5 in functions that touch moved player data, as possible player limits."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import THREE, load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
RANGES = [(0x800AEDF0, 0x800AF1D8), (0x800BCFE8, 0x800BD5B0), (0x802ACF08, 0x802ACF58), (0x802AC8A8, 0x802ACA90)]
LIMITS = {"0x3": 3, "0x4": 4, "0x5": 5}


def main():
    funcs = load_funcs()
    lines = []
    for name, instructions in funcs.items():
        if "secure_call" in name:
            continue
        refs, _ = resolve(instructions)
        if not any(a <= r.target < b for r in refs for a, b in RANGES):
            continue
        for pos, ins in enumerate(instructions):
            m = THREE.match(ins.args)
            if not m or m.group(3) not in LIMITS:
                continue
            if not (ins.op == "slti" or ins.op == "sltiu" or (ins.op == "addiu" and m.group(2) == "$zero")):
                continue
            following = " ; ".join(f"{i.op} {i.args}" for i in instructions[pos + 1:pos + 4])
            lines.append(f"{name} {ins.vram:08X} {ins.op} {ins.args} | {following}")
    with open(os.path.join(HERE, "limits.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} constants in {len({l.split()[0] for l in lines})} functions")


if __name__ == "__main__":
    main()
