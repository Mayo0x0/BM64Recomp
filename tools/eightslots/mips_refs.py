"""Reads the recompiler's output and resolves which absolute addresses each function uses.

The recompiled C keeps every MIPS instruction as a comment, so this needs no disassembler.
Usage from a script: funcs = load_funcs(); refs = resolve(funcs[name]).
"""
import glob
import os
import pickle
import re
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# The unpatched output (make_analysis.sh): the game's own output shows the patches already applied.
FUNCS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".analysis")
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".funcs.cache")

LINE = re.compile(r"\s*// (0x[0-9A-F]+):\s+(\S+)\s*(.*)$")
FUNC = re.compile(r"RECOMP_FUNC void (\w+)\(")
LOAD_STORE = re.compile(r"(\$\w+), (-?0x[0-9A-F]+|\d+)\((\$\w+)\)$")
THREE = re.compile(r"(\$\w+), (\$\w+), (\$\w+|-?0x[0-9A-F]+|\d+)$")
LUI = re.compile(r"(\$\w+), (0x[0-9A-F]+)$")

LOADS_STORES = {"lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr", "ld", "lwc1", "ldc1",
                "sw", "sh", "sb", "swl", "swr", "sd", "swc1", "sdc1"}

# What a call leaves intact under the o32 ABI; only used to drop stale values.
CALLEE_SAVED = {f"$s{i}" for i in range(8)} | {"$fp", "$gp", "$sp"}


@dataclass(frozen=True)
class Ins:
    vram: int
    op: str
    args: str


@dataclass(frozen=True)
class Ref:
    use_vram: int      # the instruction holding the low 16 bits
    lui_vram: int      # the lui holding the high 16 bits
    target: int        # the resolved absolute address
    kind: str          # "mem" for a load or store, "addr" for addiu or ori
    indexed: int       # vram of the addu that put a run-time index on the base, or 0


def load_funcs():
    sources = sorted(glob.glob(os.path.join(FUNCS_DIR, "funcs_*.c")))
    stamp = max(os.path.getmtime(s) for s in sources)
    if os.path.exists(CACHE) and os.path.getmtime(CACHE) > stamp:
        with open(CACHE, "rb") as f:
            return pickle.load(f)

    funcs = {}
    for source in sources:
        current = None
        with open(source, errors="ignore") as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    current = funcs.setdefault(m.group(1), [])
                    continue
                m = LINE.match(line)
                if m and current is not None:
                    ins = Ins(int(m.group(1), 16), m.group(2), m.group(3).strip())
                    # The recompiler repeats delay slots; the first copy has the real position.
                    if not any(i.vram == ins.vram for i in current[-3:]):
                        current.append(ins)
    for ins_list in funcs.values():
        ins_list.sort(key=lambda i: i.vram)
    with open(CACHE, "wb") as f:
        pickle.dump(funcs, f)
    return funcs


def _imm(text):
    return int(text, 16) if "x" in text else int(text)


BRANCH_TARGET = re.compile(r"L_([0-9A-F]+)$")
NO_DEST = {"sw", "sb", "sh", "swl", "swr", "sd", "swc1", "sdc1", "beq", "bne", "blez", "bgtz", "bltz",
           "bgez", "beql", "bnel", "b", "j", "jr", "nop", "break", "mult", "multu", "div", "divu",
           "mtc1", "ctc1", "c.eq.s", "c.lt.s", "c.le.s", "bc1t", "bc1f", "mthi", "mtlo", "cache", "sync"}
# Every op that leaves a branch delay slot behind it.
BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "beql", "bnel", "blezl", "bgtzl", "bltzl",
            "bgezl", "b", "bc1t", "bc1f", "bc1tl", "bc1fl"}
ENDS = {"jr", "j"}


def _transfer(ins, state, refs, used_luis):
    """Applies one instruction to the register state: register -> set of (hi, lui vram, indexed)."""
    if ins.op == "lui":
        m = LUI.match(ins.args)
        if m:
            state = dict(state)
            state[m.group(1)] = frozenset({(int(m.group(2), 16) << 16, ins.vram, 0)})
        return state

    if ins.op in ("jal", "jalr"):
        # The delay slot is the next instruction and still sees them; _flow clears after it.
        return state

    m = LOAD_STORE.match(ins.args)
    if m and ins.op in LOADS_STORES:
        value_reg, offset, base = m.groups()
        for hi, lui_vram, indexed in state.get(base, ()):
            refs.add(Ref(ins.vram, lui_vram, (hi + _imm(offset)) & 0xFFFFFFFF, "mem", indexed))
            used_luis.add(lui_vram)
        if ins.op[0] == "l" and value_reg in state:
            state = dict(state)
            del state[value_reg]
        return state

    m = THREE.match(ins.args)
    if m:
        dst, a, b = m.groups()
        state = dict(state)
        if ins.op in ("addiu", "ori") and a in state and not b.startswith("$"):
            imm = _imm(b)
            for hi, lui_vram, indexed in state[a]:
                target = (hi | (imm & 0xFFFF)) if ins.op == "ori" else (hi + imm)
                refs.add(Ref(ins.vram, lui_vram, target & 0xFFFFFFFF, "addr", indexed))
                used_luis.add(lui_vram)
            state.pop(dst, None)
            return state
        if ins.op == "addu" and (a in state) != (b in state):
            state[dst] = frozenset((hi, lv, ins.vram) for hi, lv, _ in state[a if a in state else b])
            return state
        state.pop(dst, None)
        return state

    if ins.op not in NO_DEST:
        first = ins.args.split(",")[0].strip()
        if first in state:
            state = dict(state)
            del state[first]
    return state


def _merge(a, b):
    """Returns the merged state and whether it differs from a. Unknown on one path stays unknown only
    if neither path has a value: a relocation must see every lui that can reach a use."""
    merged = dict(a)
    changed = False
    for reg, values in b.items():
        old = merged.get(reg, frozenset())
        new = old | values
        if new != old:
            merged[reg] = new
            changed = True
    return merged, changed


def resolve(instructions):
    """Forward dataflow over the function's branches: which lui can reach which use."""
    refs = set()
    used_luis = set()
    index = {ins.vram: i for i, ins in enumerate(instructions)}
    count = len(instructions)
    in_states = {}
    work = []

    def push(i, state):
        if i >= count:
            return
        if i not in in_states:
            in_states[i] = dict(state)
            work.append(i)
            return
        merged, changed = _merge(in_states[i], state)
        if changed:
            in_states[i] = merged
            work.append(i)

    def drain():
        while work:
            i = work.pop()
            ins = instructions[i]
            state = _transfer(ins, in_states[i], refs, used_luis)
            if ins.op in BRANCHES or ins.op in ENDS or ins.op in ("jal", "jalr"):
                # The delay slot runs with the branch, then control goes on from after it.
                if i + 1 >= count:
                    continue
                after = _transfer(instructions[i + 1], state, refs, used_luis)
                if ins.op in ("jal", "jalr"):
                    after = {r: v for r, v in after.items() if r in CALLEE_SAVED}
                    push(i + 2, after)
                    continue
                m = BRANCH_TARGET.search(ins.args)
                if m and int(m.group(1), 16) in index:
                    push(index[int(m.group(1), 16)], after)
                if ins.op not in ENDS and ins.op != "b":
                    push(i + 2, after)
                continue
            push(i + 1, state)

    push(0, {})
    drain()
    # Jump table targets have no visible edge; they start from what the table's jr left behind.
    last_state = {}
    for i in range(count):
        if i in in_states:
            last_state = in_states[i]
        elif instructions[i - 1].op not in ENDS or instructions[i - 2].op in ENDS:
            push(i, last_state)
            drain()
        else:
            push(i, last_state)
            drain()
    return sorted(refs, key=lambda r: (r.use_vram, r.lui_vram)), used_luis
