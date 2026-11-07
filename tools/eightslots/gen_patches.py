"""Writes bm64.eightslots.toml: the recompiler config plus this project's instruction patches.

Every patch checks the instruction it replaces against the ROM, so that a wrong address stops the
build here instead of becoming a bug in the game. Run N64Recomp on the written file.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips_refs import load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE_CONFIG = os.path.join(ROOT, "bm64.us.toml")
OUT_CONFIG = os.path.join(ROOT, "bm64.eightslots.toml")
ROM = os.path.join(ROOT, "bm64.decomp.us.z64")
OVERLAYS = os.path.join(HERE, ".analysis", "recomp_overlays.inl")
GENERATED = os.path.join(HERE, "generated")

SECTION = re.compile(r"\.rom_addr = (0x[0-9A-F]+), \.ram_addr = (0x[0-9A-F]+), \.size = (0x[0-9A-F]+), \.funcs = section_\d+_(\w+)_funcs")
FUNCTION_TABLE = re.compile(r"static FuncEntry section_\d+_(\w+)_funcs\[\]")
FUNCTION_ENTRY = re.compile(r"\.func = (\w+),")
SYMBOL_ADDRESS = re.compile(r"vram = 0x([0-9A-Fa-f]+)")

HEAP_SIZE_MB = 4
# Above the heap (0x80300000 + HEAP_SIZE_MB); nothing in the game uses this memory.
RELOCATED_BASE = 0x80700000
PLAYER_SIZE = 0xC8
MAX_PLAYERS = 8

# (first address, end address, new address, capacity at the new address)
MOVES = [
    (0x800AEDF0, 0x800AF1D8, RELOCATED_BASE, MAX_PLAYERS * PLAYER_SIZE),  # g_Players
]


def read_sections():
    """Returns function name -> (section ram, section rom), since overlays share their vram."""
    tables = {}
    placement = {}
    current = None
    with open(OVERLAYS) as f:
        for line in f:
            m = FUNCTION_TABLE.search(line)
            if m:
                current = tables.setdefault(m.group(1), [])
                continue
            m = SECTION.search(line)
            if m:
                placement[m.group(4)] = (int(m.group(2), 16), int(m.group(1), 16))
                continue
            m = FUNCTION_ENTRY.search(line)
            if m and current is not None:
                current.append(m.group(1))
    return {func: placement[name] for name, funcs in tables.items() if name in placement for func in funcs}


class Patcher:
    def __init__(self):
        with open(ROM, "rb") as f:
            self.rom = f.read()
        self.sections = read_sections()
        self.patches = []
        self.taken = {}

    def word_at(self, func, vram):
        ram, rom = self.sections[func]
        return struct.unpack(">I", self.rom[rom + vram - ram:rom + vram - ram + 4])[0]

    def patch(self, func, vram, expected, immediate):
        found = self.word_at(func, vram)
        if found != expected:
            raise ValueError(f"{func} {vram:08X}: expected {expected:08X}, the ROM has {found:08X}")
        self.set_immediate(func, vram, immediate)

    def set_immediate(self, func, vram, immediate):
        """The one way an instruction changes here; two changes of one instruction must agree."""
        value = (self.word_at(func, vram) & 0xFFFF0000) | (immediate & 0xFFFF)
        key = (func, vram)
        if self.taken.get(key, value) != value:
            raise ValueError(f"{func} {vram:08X}: two different patches")
        if key not in self.taken:
            self.taken[key] = value
            self.patches.append((func, vram, value))


def lui(reg, value):
    return 0x3C000000 | (reg << 16) | value


def heap_patches(p):
    """func_80225B00 is sbrk and refuses to grow past 0x80300000 + 1 MB."""
    at = 1
    p.patch("func_80225B00", 0x80225B1C, lui(at, 0x10), HEAP_SIZE_MB * 0x10)


def new_address(address, inclusive_end):
    for start, end, new_start, _ in MOVES:
        if start <= address < end or (inclusive_end and address == end):
            return new_start + address - start
    return None


def high_half(address):
    return (address + 0x8000) >> 16 & 0xFFFF


def connected_groups(refs):
    """A lui can serve several uses and a use can come from several lui: those must move together."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for r in refs:
        parent[find(("lui", r.lui_vram))] = find(("use", r.use_vram))
    groups = {}
    for r in refs:
        groups.setdefault(find(("lui", r.lui_vram)), []).append(r)
    return groups.values()


def relocation_patches(p):
    """Moves every reference into MOVES by rewriting its lui and its low half."""
    moved = 0
    report = []
    for name, instructions in load_funcs().items():
        if "secure_call" in name:
            continue
        refs, _ = resolve(instructions)
        if not any(new_address(r.target, r.kind == "addr") is not None for r in refs):
            continue
        for group in connected_groups(refs):
            new = [new_address(r.target, r.kind == "addr") for r in group]
            if all(n is None for n in new):
                continue
            first_lui = min(r.lui_vram for r in group)
            if any(n is None for n in new):
                inside = sorted({r.target for r, n in zip(group, new) if n is not None})
                outside = sorted({r.target for r, n in zip(group, new) if n is None})
                report.append(f"{name} lui {first_lui:08X}: moves {[hex(a) for a in inside][:3]}, keeps {[hex(a) for a in outside][:3]}")
                continue
            highs = {high_half(n) for n in new}
            if len(highs) != 1:
                report.append(f"{name} lui {first_lui:08X}: new addresses need different lui: {[hex(n) for n in new][:4]}")
                continue
            high = highs.pop()
            for r, n in zip(group, new):
                # The ROM must hold what the analysis read.
                if p.word_at(name, r.lui_vram) & 0xFFFF != high_half(r.target):
                    raise ValueError(f"{name} {r.lui_vram:08X}: lui does not match {r.target:08X}")
                if p.word_at(name, r.use_vram) & 0xFFFF != r.target & 0xFFFF:
                    raise ValueError(f"{name} {r.use_vram:08X}: low half does not match {r.target:08X}")
                p.set_immediate(name, r.lui_vram, high)
                p.set_immediate(name, r.use_vram, n)
                moved += 1
    print(f"relocated references: {moved}, conflicts: {len(report)}")
    with open(os.path.join(HERE, "relocation_report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    return len(report)


def scan_data_pointers(rom):
    """Pointers stored in ROM data cannot be found by reading code."""
    found = []
    for i in range(0, len(rom) - 3, 4):
        word = struct.unpack(">I", rom[i:i + 4])[0]
        if new_address(word, False) is not None:
            found.append((i, word))
    print(f"words in the ROM that look like pointers into MOVES: {len(found)}")
    for rom_address, word in found[:12]:
        print(f"  rom {rom_address:08X}: {word:08X}")


def write_data_symbols():
    """The game's C patches find its data by name, so the symbol file moves along."""
    os.makedirs(GENERATED, exist_ok=True)

    def shifted(m):
        new = new_address(int(m.group(1), 16), False)
        return m.group(0) if new is None else f"vram = 0x{new:08X}"

    with open(os.path.join(ROOT, "BM64Syms", "data_dump.toml")) as f:
        text = SYMBOL_ADDRESS.sub(shifted, f.read())
    with open(os.path.join(GENERATED, "data_dump.toml"), "w", newline="\n") as f:
        f.write(text)


def main():
    p = Patcher()
    heap_patches(p)
    conflicts = relocation_patches(p)
    scan_data_pointers(p.rom)
    write_data_symbols()
    if conflicts:
        print("Conflicts stop the build: see relocation_report.txt")
        return 1

    with open(BASE_CONFIG, newline="") as f:
        base = f.read().replace("\r\n", "\n")
    lines = [base.rstrip("\n"), "", "# Generated by tools/eightslots/gen_patches.py, do not edit.", ""]
    for func, vram, value in p.patches:
        lines += ["[[patches.instruction]]", f'func = "{func}"', f"vram = 0x{vram:08X}", f"value = 0x{value:08X}", ""]
    with open(OUT_CONFIG, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"{len(p.patches)} patches written to {os.path.basename(OUT_CONFIG)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
