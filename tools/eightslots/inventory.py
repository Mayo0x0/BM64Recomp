"""Finds the globals the game indexes at run time, with the stride of the index.

Writes inventory.txt (every indexed base) and candidates.txt (the ones sized like player arrays).
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import THREE, load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
PLAYERS = 0x800AEDF0
PLAYER_END = PLAYERS + 5 * 0xC8
# Enough entries for 3 to 8 players; fewer or more is a table of something else.
MIN_ENTRIES, MAX_ENTRIES = 3, 8


def strides(instructions):
    """Linear guess of each register's multiple of an unknown index: addu vram -> stride."""
    mult = {}
    out = {}
    for ins in instructions:
        m = THREE.match(ins.args)
        if not m:
            continue
        dst, a, b = m.groups()
        if ins.op == "sll" and not b.startswith("$"):
            shift = int(b, 16) if "x" in b else int(b)
            mult[dst] = mult.get(a, 1) << shift
        elif ins.op == "addu":
            ma, mb = mult.get(a), mult.get(b)
            out[ins.vram] = ma or mb or 1
            if ma or mb or a != "$zero":
                mult[dst] = (ma or 1) + (mb or 1) if (a not in ("$zero",) and b not in ("$zero",)) else (ma or mb or 1)
        elif ins.op == "subu":
            mult[dst] = (mult.get(a) or 1) - (mult.get(b) or 1)
        elif ins.op == "mflo":
            mult.pop(dst, None)
        else:
            mult.pop(dst, None)
    return out


def main():
    funcs = load_funcs()
    code = {k: v for k, v in funcs.items() if "secure_call" not in k}
    indexed = collections.defaultdict(lambda: collections.defaultdict(set))
    touched = set()
    player_funcs = set()
    for name, instructions in code.items():
        refs, _ = resolve(instructions)
        table = strides(instructions)
        for r in refs:
            touched.add(r.target)
            if PLAYERS <= r.target <= PLAYER_END:
                player_funcs.add(name)
            if r.indexed:
                indexed[r.target][table.get(r.indexed, 0)].add(name)

    known = sorted(touched)
    position = {a: i for i, a in enumerate(known)}
    all_lines = []
    candidates = []
    for address in sorted(indexed):
        i = position[address]
        gap = known[i + 1] - address if i + 1 < len(known) else 0
        for stride, users in sorted(indexed[address].items()):
            line = f"{address:08X} stride={stride:<5X} next+{gap:<6X} users={len(users):<3} {' '.join(sorted(users)[:6])}"
            all_lines.append(line)
            if stride > 0 and MIN_ENTRIES <= gap / stride <= MAX_ENTRIES and not all(u.startswith("func_ovl") for u in users):
                by_players = sum(1 for u in users if u in player_funcs)
                candidates.append(f"{line} [players:{by_players}]")
    with open(os.path.join(HERE, "inventory.txt"), "w") as f:
        f.write("\n".join(all_lines) + "\n")
    with open(os.path.join(HERE, "candidates.txt"), "w") as f:
        f.write("\n".join(candidates) + "\n")
    print(f"functions: {len(code)}, addressing g_Players: {len(player_funcs)}")
    print(f"indexed bases: {len(indexed)}, sized like player arrays: {len(candidates)}")


if __name__ == "__main__":
    main()
