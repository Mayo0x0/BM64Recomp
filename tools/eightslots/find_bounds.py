"""Finds addresses used as a loop's end: compared with a stepped pointer that started somewhere else.

Such an address can equal the start of another array, so moving it by its value would be wrong.
gen_patches.py imports this and stops on any case it has no rule for.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import THREE, load_funcs, resolve


def loop_bounds(name, instructions, refs):
    """Returns (bound's use vram, bound's address, pointer's start address) for each pointer loop."""
    by_vram = {i.vram: i for i in instructions}
    address_of = {}
    steps = set()
    for r in refs:
        ins = by_vram.get(r.use_vram)
        if r.kind == "addr" and not r.indexed and ins is not None:
            m = THREE.match(ins.args)
            if m:
                address_of.setdefault(m.group(1), []).append((r.use_vram, r.target))
    for ins in instructions:
        m = THREE.match(ins.args)
        if m and ins.op == "addiu" and m.group(1) == m.group(2) and not m.group(3).startswith("$"):
            steps.add(m.group(1))

    found = []
    for pos, ins in enumerate(instructions):
        pair = None
        if ins.op in ("bne", "beq"):
            parts = [p.strip() for p in ins.args.split(",")]
            pair = parts[:2]
        elif ins.op in ("sltu", "slt"):
            m = THREE.match(ins.args)
            if m and m.group(3).startswith("$"):
                pair = [m.group(2), m.group(3)]
        if not pair or "$zero" in pair or "$at" in pair:
            continue
        a, b = pair
        for pointer, bound in ((a, b), (b, a)):
            if pointer in steps and pointer in address_of and bound in address_of:
                for use, target in address_of[bound]:
                    if use < ins.vram:
                        start = address_of[pointer][-1][1]
                        if target > start:
                            found.append((use, target, start))
    return sorted(set(found))


def main():
    for name, instructions in load_funcs().items():
        if "secure_call" in name:
            continue
        refs, _ = resolve(instructions)
        for use, target, start in loop_bounds(name, instructions, refs):
            print(f"{name} {use:08X} bound={target:08X} pointer starts={start:08X} span={target - start:#x}")


if __name__ == "__main__":
    main()
