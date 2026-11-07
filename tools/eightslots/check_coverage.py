"""Lists every lui that loads a RAM address but that the linear scan did not tie to a use.

A relocated array is only safe if none of these reach it, so anything left here is read by hand.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import LUI, load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
FLOAT_USES = ("mtc1",)


def main():
    funcs = load_funcs()
    by_hi = collections.Counter()
    float_luis = other_luis = 0
    out = []
    for name, instructions in funcs.items():
        if "secure_call" in name:
            continue
        _, used = resolve(instructions)
        for pos, ins in enumerate(instructions):
            if ins.op != "lui" or ins.vram in used:
                continue
            m = LUI.match(ins.args)
            if not m:
                continue
            reg, hi = m.group(1), int(m.group(2), 16)
            # Float constants are built as lui + mtc1, often with other instructions between.
            window = instructions[pos + 1:pos + 16]
            if any(i.op in FLOAT_USES and i.args.startswith(reg + ",") for i in window):
                float_luis += 1
                continue
            if not 0x8000 <= hi <= 0x8040:
                other_luis += 1
                continue
            by_hi[hi] += 1
            next_ins = " | ".join(f"{i.op} {i.args}" for i in window[:4])
            out.append(f"{name} {ins.vram:08X} {ins.op} {ins.args} -> {next_ins}")
    with open(os.path.join(HERE, "coverage.txt"), "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"float constants: {float_luis}, other non-RAM values: {other_luis}")
    print(f"RAM-looking lui without a resolved use: {sum(by_hi.values())}")
    for hi, count in sorted(by_hi.items()):
        print(f"  {hi:04X}: {count}")


if __name__ == "__main__":
    main()
