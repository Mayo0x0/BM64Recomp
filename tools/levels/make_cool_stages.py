"""Builds a set of big battle stages for the level editor from character maps.
Input: the grid dumps of the stages (tools/eightslots/logs/grid<stage>.out.txt, the test mod built with -DDUMP_GRID).
Usage: make_cool_stages.py <out folder>   (writes one level json per stage and prints a picture of each)

Map characters (one per tile of the arena's inside; a wall is put around it):
  .  floor that may get a random soft block      ,  floor that never gets a block
  o  soft block                                  H  hard block (explosions do not break it)
  X  pillar (a tall wall)                        (space) hole: a bomber falls through
  f b r p s k  soft block with fire / bomb / remote / power / skull / heart in it
  1 to 8  start of that player (free floor)
  W  raised floor (one layer up, free)           Q  raised floor that may get a random block
  O  raised floor with a soft block               P  raised floor with a soft block holding power
  T  floor two layers up (free)
  > < v ^  ramp from the floor up to raised floor, rising toward +x, -x, +z (down in the map), -z
  ) ( ] [  ramp from raised floor up to T, rising toward +x, -x, +z, -z
"""
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, "..", "eightslots", "logs")
ITEMS = {"f": "fire", "b": "bomb", "r": "remote", "p": "power", "s": "skull", "k": "heart"}
RAMP0 = {">": 5, "<": 6, "v": 3, "^": 4}   # SHAPE_RAMP_* on layer 0
RAMP1 = {")": 5, "(": 6, "]": 3, "[": 4}   # on layer 1
DIRS = {5: (1, 0), 6: (-1, 0), 3: (0, 1), 4: (0, -1)}


def load_grid(stage):
    grid = {}
    for line in open(os.path.join(LOGS, f"grid{stage}.out.txt"), encoding="utf-8", errors="replace"):
        m = re.match(r"TEST grid (\d+)\s+(\d+):(.*)", line)
        if m:
            grid[int(m.group(1)), int(m.group(2))] = [int(v, 16) for v in m.group(3).split()]
    return grid


def column_of(ch, tall):
    """The kinds of a map character's column from layer 0 up, and the layer of its floor (None: none)."""
    if ch == "#":
        return [0xF] * tall + [1], None
    if ch == "X":
        return [0xF, 0xF, 1], None
    if ch == " ":
        return [], None
    if ch in "WQOP":
        return [0xF, 1], 1
    if ch == "T":
        return [0xF, 0xF, 1], 2
    if ch in RAMP0:
        return [RAMP0[ch]], None
    if ch in RAMP1:
        return [0xF, RAMP1[ch]], None
    return [1], 0


def top(ch):
    """Height a bomber stands at on this tile (ramps: their low end), None if it cannot stand there."""
    if ch in "#X ":
        return None
    if ch in "WQOP":
        return 1
    if ch == "T":
        return 2
    if ch in RAMP1:
        return 1
    return 0


def build(name, stage, rows, rules, out):
    grid = load_grid(stage)
    layers = max(l for l, _ in grid) + 1
    depth = max(z for _, z in grid) + 1
    width = len(grid[0, 0])
    kind = lambda x, l, z: grid[l, z][x] & 0xF if 0 <= x < width and 0 <= z < depth and (l, z) in grid else 0
    own = lambda x, z: [kind(x, l, z) for l in range(layers)]

    w, d = len(rows[0]), len(rows)
    assert all(len(r) == w for r in rows), f"{name}: rows of different length"
    tall = 3 if any("T" in r for r in rows) else 2  # the outer wall stays above everything one can climb
    assert tall + 1 <= layers, f"{name}: stage {stage} has {layers} layers, needs {tall + 1}"
    # The stage's own arena: where the looks come from, and the centre the new one is put on.
    walls = [(x, z) for z in range(depth) for x in range(width) if kind(x, 0, z) == 0xF]
    wx0, wx1 = min(x for x, _ in walls), max(x for x, _ in walls)
    wz0, wz1 = min(z for _, z in walls), max(z for _, z in walls)
    wall_src = (wx0, (wz0 + wz1) // 2)
    floors = [(x, z) for z in range(wz0, wz1 + 1) for x in range(wx0, wx1 + 1)
              if kind(x, 0, z) == 1 and all(kind(x, l, z) == 0 for l in range(1, layers))]
    cx, cz = (wx0 + wx1) / 2, (wz0 + wz1) / 2
    floor_src = min(floors, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cz) ** 2)
    x0 = round(cx - (w + 2) / 2 + 0.5)
    z0 = round(cz - (d + 2) / 2 + 0.5)
    x0, z0 = max(0, min(width - w - 2, x0)), max(0, min(depth - d - 2, z0))

    full = [["#"] * (w + 2)] + [["#"] + list(r) + ["#"] for r in rows] + [["#"] * (w + 2)]
    at = lambda i, j: full[j][i] if 0 <= i < w + 2 and 0 <= j < d + 2 else "#"

    # Checks: ramps lead from their low side to the right height, starts have room to move.
    for j in range(d + 2):
        for i in range(w + 2):
            ch = at(i, j)
            if ch in RAMP0 or ch in RAMP1:
                low = 0 if ch in RAMP0 else 1
                dx, dz = DIRS[(RAMP0 if ch in RAMP0 else RAMP1)[ch]]
                assert top(at(i + dx, j + dz)) == low + 1, f"{name}: ramp {ch} at {i - 1},{j - 1} leads to {at(i + dx, j + dz)!r}"
                assert top(at(i - dx, j - dz)) == low, f"{name}: ramp {ch} at {i - 1},{j - 1} starts from {at(i - dx, j - dz)!r}"
            if ch in "12345678":
                room = [(i + a, j + b) for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1))]
                free = [p for p in room if at(*p) in ",.<>^v" and at(*p) != "."]
                assert len(free) >= 2 or sum(at(*p) == "," for p in room) >= 2, f"{name}: start {ch} is boxed in"

    shape, looks, tiles, items, starts = [], [], [], [], [None] * 8
    for j in range(d + 2):
        for i in range(w + 2):
            x, z = x0 + i, z0 + j
            ch = at(i, j)
            want, floor = column_of(ch, tall)
            have = own(x, z)
            want_full = want + [0] * (layers - len(want))
            if want_full != have:
                used = max([l + 1 for l, k in enumerate(want_full) if k] + [l + 1 for l, k in enumerate(have) if k] + [1])
                shape.append([x, z, "".join("." if k == 0 else "%X" % k for k in want_full[:used])])
            if want:
                src = wall_src if ch in "#XWQOPT" else floor_src
                if (x, z) != src:
                    looks.append([x, z, src[0], src[1]])
            if floor is not None:
                block = {".": "random", "Q": "random", "o": "soft", "O": "soft", "P": "soft", "H": "hard"}.get(ch, "free")
                if ch in ITEMS:
                    block = "soft"
                    items.append([x, floor, z, ITEMS[ch]])
                if ch == "P":
                    items.append([x, floor, z, "power"])
                tiles.append([x, floor, z, block])
            if ch in "12345678":
                starts[int(ch) - 1] = [x, 0, z]
    assert None not in starts, f"{name}: not all 8 starts are set"
    # A round has room for 48 blocks, set and random together (patches/level.c MAX_BLOCKS); hard blocks count too.
    set_blocks = sum(1 for t in tiles if t[3] in ("soft", "hard"))
    assert set_blocks <= 40, f"{name}: {set_blocks} set blocks leave too few random ones"
    rules = dict(rules, randomBlocks=min(rules["randomBlocks"], 48 - set_blocks))
    assert len(shape) <= 4096 and len(looks) <= 2048 and len(tiles) <= 2048 and len(items) <= 64, name
    level = {"name": name, "stage": stage, "version": 1, "rules": rules, "starts": starts,
             "tiles": tiles, "items": items, "shape": shape, "looks": looks}
    file = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") + ".json"
    json.dump(level, open(os.path.join(out, file), "w"), indent=1)
    print(f"== {name} (stage {stage + 1}, {w}x{d}) -> {file}: shape {len(shape)}, looks {len(looks)}, "
          f"tiles {len(tiles)}, items {len(items)}, set blocks {set_blocks}, random blocks {rules['randomBlocks']}")
    for r in full:
        print("   " + "".join(r))


def rules(minutes, blocks, bombs=-1, fire=-1):
    return {"minutes": minutes, "suddenDeath": 1, "ghosts": 1, "bombs": bombs, "fire": fire, "randomBlocks": blocks}


def grid_map(w, d, fill="."):
    return [[fill] * w for _ in range(d)]


def put(m, x, z, ch):
    m[z][x] = ch


def rect(m, x0, z0, x1, z1, ch):
    for z in range(z0, z1 + 1):
        for x in range(x0, x1 + 1):
            m[z][x] = ch


def mirror4(m, x, z, ch):
    """Puts ch at (x, z) and its three mirror images (left/right, front/back)."""
    w, d = len(m[0]), len(m)
    for xx in {x, w - 1 - x}:
        for zz in {z, d - 1 - z}:
            m[zz][xx] = ch


def text(m):
    return ["".join(r) for r in m]


def crossroads():
    """Pillars as in the original stages; in the middle a raised cross with a ramp on each side and prizes on top."""
    w, d = 27, 21
    m = grid_map(w, d)
    for z in range(1, d, 2):
        for x in range(1, w, 2):
            put(m, x, z, "X")
    rect(m, 9, 7, 17, 13, ",")                 # room around the cross
    rect(m, 11, 8, 15, 12, "W")
    rect(m, 10, 9, 16, 11, "W")
    put(m, 9, 10, ">"); put(m, 17, 10, "<"); put(m, 13, 7, "v"); put(m, 13, 13, "^")
    for x, z in ((11, 8), (15, 8), (11, 12), (15, 12)):
        put(m, x, z, "O")
    put(m, 13, 10, "P")
    for x, z, ch in ((4, 2, "f"), (8, 4, "b"), (10, 2, "r"), (4, 8, "o"), (6, 10, "k")):
        mirror4(m, x, z, ch)
    starts = {(0, 0): "1", (w - 1, 0): "2", (0, d - 1): "3", (w - 1, d - 1): "4",
              (13, 0): "5", (13, d - 1): "6", (0, 10): "7", (w - 1, 10): "8"}
    for (x, z), ch in starts.items():
        for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2)):
            if 0 <= x + a < w and 0 <= z + b < d and m[z + b][x + a] not in "X":
                m[z + b][x + a] = ","
        put(m, x, z, ch)
    return text(m)



def clear_starts(m, starts):
    w, d = len(m[0]), len(m)
    for (x, z), ch in starts.items():
        for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2), (1, 1), (-1, 1), (1, -1), (-1, -1)):
            if 0 <= x + a < w and 0 <= z + b < d and m[z + b][x + a] not in "X ><v^WQOPT":
                m[z + b][x + a] = ","
        put(m, x, z, ch)


def labyrinth():
    """A maze of pillars with extra openings; soft blocks with items in its corridors."""
    w, d = 29, 23
    rnd = random.Random(64)
    m = grid_map(w, d, "X")
    cells = [(x, z) for z in range(0, d, 2) for x in range(0, w, 2)]
    for x, z in cells:
        put(m, x, z, ".")
    seen, stack = {(0, 0)}, [(0, 0)]
    while stack:                                # a random maze over the even tiles
        x, z = stack[-1]
        nexts = [(x + a, z + b) for a, b in ((2, 0), (-2, 0), (0, 2), (0, -2))
                 if 0 <= x + a < w and 0 <= z + b < d and (x + a, z + b) not in seen]
        if not nexts:
            stack.pop()
            continue
        nx, nz = rnd.choice(nexts)
        put(m, (x + nx) // 2, (z + nz) // 2, ".")
        seen.add((nx, nz))
        stack.append((nx, nz))
    for z in range(d):                          # open about a third of the other walls: loops to escape bombs
        for x in range(w):
            if m[z][x] == "X" and (x % 2) != (z % 2) and rnd.random() < 0.35:
                put(m, x, z, "o" if rnd.random() < 0.12 else ".")
    rect(m, 12, 9, 16, 13, ",")                # a room in the middle with prizes
    for x, z, ch in ((13, 10, "p"), (15, 10, "r"), (13, 12, "b"), (15, 12, "f")):
        put(m, x, z, ch)
    for x, z in ((14, 8), (14, 14), (11, 11), (17, 11)):
        put(m, x, z, ".")                        # doors to the room
    floor = [(x, z) for z in range(d) for x in range(w) if m[z][x] == "."]
    for x, z in rnd.sample(floor, 10):
        put(m, x, z, rnd.choice("ffbbkso"))
    starts = {(0, 0): "1", (w - 1, 0): "2", (0, d - 1): "3", (w - 1, d - 1): "4",
              (14, 0): "5", (14, d - 1): "6", (0, 10): "7", (w - 1, 12): "8"}
    clear_starts(m, starts)
    return text(m)




if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else HERE
    build("Big Crossroads", 0, crossroads(), rules(4, 40), out)
    build("Big Labyrinth", 7, labyrinth(), rules(5, 30), out)
