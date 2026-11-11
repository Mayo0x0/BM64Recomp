"""Finds tables indexed after their address is formed: `addiu t0, base` then `addu a1, index, t0`.

inventory.py only sees an index added before the low half (base register + index, then `lw x(base)`),
which is how 0x802ACF68 (the CPU standstill) slipped through. Writes late_index.txt: every such table
not yet moved, with its stride, the room up to the next known address, and the functions using it.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import LUI, THREE, load_funcs, resolve
from inventory import strides

HERE = os.path.dirname(os.path.abspath(__file__))
ADDIU = re.compile(r"(\$\w+), (\$\w+), (-?0x[0-9A-F]+|\d+)$")
SCAN = 24  # instructions after the address is formed


def moved_ranges():
    ranges = []
    with open(os.path.join(HERE, "moves.txt")) as f:
        for line in f:
            m = re.match(r"([0-9A-F]{8})-([0-9A-F]{8})", line)
            if m:
                ranges.append((int(m.group(1), 16), int(m.group(2), 16)))
    return ranges


def late_indexed(instructions, refs):
    """(target, stride) for addr refs whose register is later added to another register."""
    at = {ins.vram: i for i, ins in enumerate(instructions)}
    table = strides(instructions)
    for r in refs:
        if r.kind != "addr":
            continue
        i = at[r.use_vram]
        m = ADDIU.match(instructions[i].args)
        if not m:
            continue
        reg = m.group(1)
        for ins in instructions[i + 1:i + 1 + SCAN]:
            t = THREE.match(ins.args)
            if ins.op in ("addu", "subu") and t and reg in t.groups()[1:]:
                yield r.target, table.get(ins.vram, 1)
                break
            # the register is overwritten
            if (t and t.group(1) == reg) or ((m2 := ADDIU.match(ins.args)) and m2.group(1) == reg) \
                    or ((m3 := LUI.match(ins.args)) and ins.op == "lui" and m3.group(1) == reg):
                break


def main():
    funcs = load_funcs()
    code = {k: v for k, v in funcs.items() if "secure_call" not in k}
    moved = moved_ranges()
    found = collections.defaultdict(lambda: collections.defaultdict(set))
    touched = set()
    for name, instructions in code.items():
        refs, _ = resolve(instructions)
        touched.update(r.target for r in refs)
        for target, stride in late_indexed(instructions, refs):
            found[target][stride].add(name)
    known = sorted(touched)
    position = {a: i for i, a in enumerate(known)}
    lines = []
    for address in sorted(found):
        if any(lo <= address < hi for lo, hi in moved):
            continue
        i = position[address]
        gap = known[i + 1] - address if i + 1 < len(known) else 0
        for stride, users in sorted(found[address].items()):
            entries = gap / stride if stride > 0 else 0
            mark = "  <- 3 to 5 entries" if 3 <= entries <= 5 else ""
            lines.append(f"{address:08X} stride={stride:<5X} next+{gap:<6X} users={len(users):<3} "
                         f"{' '.join(sorted(users)[:5])}{mark}")
    with open(os.path.join(HERE, "late_index.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"late indexed tables not moved: {len(lines)}, sized for 3 to 5: {sum('<-' in l for l in lines)}")


if __name__ == "__main__":
    main()
