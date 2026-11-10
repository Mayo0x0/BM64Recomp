"""Finds the arrays the game indexes by player slot, by following the slot through registers and calls.

A value is a "slot" if it is a known function's slot argument, or a loop counter that is passed on as one.
Writes slot_arrays.txt: base address, stride, the functions, and the gap to the next address in use.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import LOAD_STORE, THREE, load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
PLAYERS = 0x800AEDF0
PLAYER_STRIDE = 0xC8
JAL = re.compile(r"0x([0-9A-F]+)")
ARG_REGS = ("$a0", "$a1", "$a2", "$a3")


def stride_map(instructions, tainted_at_entry):
    """Walks a function once. Returns (used bases as addu vram -> multiple of the slot, slot arguments of calls)."""
    mult = {reg: 1 for reg in tainted_at_entry}  # register -> multiple of the slot
    bases = {}
    calls = []
    stack = {}
    for pos, ins in enumerate(instructions):
        if ins.op == "jal":
            target = JAL.search(ins.args)
            # The delay slot sets an argument too.
            state = dict(mult)
            if pos + 1 < len(instructions):
                d = THREE.match(instructions[pos + 1].args)
                if d and instructions[pos + 1].op == "or" and d.group(3) == "$zero" and d.group(2) in state:
                    state[d.group(1)] = state[d.group(2)]
                elif d and d.group(1) in state:
                    del state[d.group(1)]
            if target:
                calls.append((int(target.group(1), 16), [i for i, r in enumerate(ARG_REGS) if state.get(r) == 1]))
            for reg in list(mult):
                if not (reg.startswith("$s") and reg != "$sp") and reg != "$fp":
                    del mult[reg]
            continue
        if ins.op == "sw":
            m = LOAD_STORE.match(ins.args)
            if m and m.group(3) == "$sp":
                if m.group(1) in mult:
                    stack[m.group(2)] = mult[m.group(1)]
                else:
                    stack.pop(m.group(2), None)
            continue
        if ins.op in ("lw", "lh", "lb", "lbu", "lhu"):
            m = LOAD_STORE.match(ins.args)
            if m:
                if m.group(3) == "$sp" and m.group(2) in stack:
                    mult[m.group(1)] = stack[m.group(2)]
                else:
                    mult.pop(m.group(1), None)
            continue
        m = THREE.match(ins.args)
        if not m:
            first = ins.args.split(",")[0].strip()
            if ins.op not in ("sw", "sh", "sb", "beq", "bne", "b", "jr", "nop", "swc1", "multu", "mult", "div", "divu"):
                mult.pop(first, None)
            continue
        dst, a, b = m.groups()
        if ins.op == "sll" and a in mult:
            mult[dst] = mult[a] << int(b, 16)
        elif ins.op == "addu":
            if a in mult and b in mult:
                mult[dst] = mult[a] + mult[b]
            elif a in mult or b in mult:
                bases[ins.vram] = mult[a if a in mult else b]
                mult.pop(dst, None)
            else:
                mult.pop(dst, None)
        elif ins.op == "subu" and a in mult and b in mult:
            mult[dst] = mult[a] - mult[b]
        elif ins.op == "or" and b == "$zero" and a in mult:
            mult[dst] = mult[a]
        elif ins.op == "addiu" and a in mult and dst == a:
            pass  # a counter stepping keeps being the slot
        else:
            mult.pop(dst, None)
    return bases, calls


def loop_counters(instructions):
    """A register compared with 4 or 5 in a backward branch, starting at zero, is a slot loop's counter."""
    counters = set()
    limits = {}
    for ins in instructions:
        m = THREE.match(ins.args)
        if m and ins.op == "addiu" and m.group(2) == "$zero" and m.group(3) in ("0x4", "0x5"):
            limits[m.group(1)] = True
        if ins.op in ("bne", "beq"):
            parts = [p.strip() for p in ins.args.split(",")]
            if len(parts) == 3 and (parts[0] in limits or parts[1] in limits):
                other = parts[1] if parts[0] in limits else parts[0]
                if other.startswith("$s"):
                    counters.add(other)
    return counters


def stepped_pointers(instructions, refs):
    """Loops that walk an array with a pointer: base address -> step, for a counted loop of 4 or 5."""
    if not loop_counters(instructions) and not any(i.op == "bne" for i in instructions):
        return {}
    out = {}
    steps = {}
    for ins in instructions:
        m = THREE.match(ins.args)
        if m and ins.op == "addiu" and m.group(1) == m.group(2) and not m.group(3).startswith("$"):
            step = int(m.group(3), 16) if "x" in m.group(3) else int(m.group(3))
            if 0 < step <= 0x400:
                steps.setdefault(m.group(1), step)
    by_vram = {i.vram: i for i in instructions}
    for r in refs:
        ins = by_vram.get(r.use_vram)
        if ins is None or r.kind != "addr":
            continue
        m = THREE.match(ins.args)
        if m and m.group(1) in steps:
            out[r.target] = steps[m.group(1)]
    return out


def main():
    funcs = load_funcs()
    code = {k: v for k, v in funcs.items() if "secure_call" not in k}
    by_vram = {v[0].vram: k for k, v in code.items() if v and not k.startswith("func_ovl")}

    # Seeds: functions that index g_Players by their first argument.
    slot_args = collections.defaultdict(set)
    for name, instructions in code.items():
        if name.startswith("func_ovl"):
            continue
        refs, _ = resolve(instructions)
        addus = {r.indexed for r in refs if r.indexed and PLAYERS <= r.target < PLAYERS + 5 * PLAYER_STRIDE}
        if not addus:
            continue
        bases, _ = stride_map(instructions, ["$a0"])
        if any(bases.get(a) == PLAYER_STRIDE for a in addus):
            slot_args[name].add("$a0")

    # Spread through calls until nothing new turns up.
    changed = True
    while changed:
        changed = False
        for name, instructions in code.items():
            if name.startswith("func_ovl"):
                continue
            start = set(slot_args[name]) | loop_counters(instructions)
            if not start:
                continue
            _, calls = stride_map(instructions, start)
            for target, args in calls:
                callee = by_vram.get(target)
                if not callee:
                    continue
                for i in args:
                    if ARG_REGS[i] not in slot_args[callee]:
                        slot_args[callee].add(ARG_REGS[i])
                        changed = True

    arrays = collections.defaultdict(lambda: collections.defaultdict(set))
    known = set()
    for name, instructions in code.items():
        refs, _ = resolve(instructions)
        known.update(r.target for r in refs)
        if name.startswith("func_ovl"):
            continue
        start = set(slot_args[name]) | loop_counters(instructions)
        if not start:
            continue
        bases, _ = stride_map(instructions, start)
        for r in refs:
            if r.indexed and r.indexed in bases:
                arrays[r.target][bases[r.indexed]].add(name)
        if loop_counters(instructions):
            for target, step in stepped_pointers(instructions, refs).items():
                arrays[target][step].add(name + "*")

    known = sorted(known)
    position = {a: i for i, a in enumerate(known)}
    lines = []
    for address in sorted(arrays):
        i = position[address]
        gap = known[i + 1] - address if i + 1 < len(known) else 0
        for stride, users in sorted(arrays[address].items()):
            entries = gap / stride if stride else 0
            lines.append(f"{address:08X} stride={stride:<5X} next+{gap:<6X} ~{entries:<5.1f} users={len(users):<3} {' '.join(sorted(users)[:5])}")
    with open(os.path.join(HERE, "slot_arrays.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"functions with a slot: {sum(1 for v in slot_args.values() if v)}, slot-indexed addresses: {len(arrays)}")


if __name__ == "__main__":
    main()
