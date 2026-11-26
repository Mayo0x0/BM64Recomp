"""Builds a level that makes a battle stage 2 x 2 times as big: the arena's inside repeated in four quadrants, a wall
around it. Input: a grid dump of the stage (the test mod built with -DDUMP_GRID, tools/eightslots/stage_starts.sh).
Usage: make_big_stage.py <grid log> <stage> <out.json> [name]"""
import json
import re
import sys

log, stage, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
name = sys.argv[4] if len(sys.argv) > 4 else "Big"

grid = {}  # (layer, z) -> [tile per x]
for line in open(log, encoding="utf-8", errors="replace"):
    m = re.match(r"TEST grid (\d+)\s+(\d+):(.*)", line)
    if m:
        grid[int(m.group(1)), int(m.group(2))] = [int(v, 16) for v in m.group(3).split()]
layers = max(l for l, _ in grid) + 1
depth = max(z for _, z in grid) + 1
width = len(grid[0, 0])


def tile(x, l, z):
    return grid[l, z][x] if 0 <= x < width and 0 <= z < depth else 0


def kind(x, l, z):
    return tile(x, l, z) & 0xF


# The arena's inside: floor on layer 0 inside the bounding box of the walls on layer 0.
walls = [(x, z) for z in range(depth) for x in range(width) if kind(x, 0, z) == 0xF]
wx0, wx1 = min(x for x, _ in walls), max(x for x, _ in walls)
wz0, wz1 = min(z for _, z in walls), max(z for _, z in walls)
ix0, iz0, iw, id_ = wx0 + 1, wz0 + 1, wx1 - wx0 - 1, wz1 - wz0 - 1
nw, nd = 2 * iw, 2 * id_  # the new inside
assert nw + 2 <= width and nd + 2 <= depth, "the grid has no room for it"
cx, cz = (wx0 + wx1) / 2, (wz0 + wz1) / 2
nx0 = max(1, min(width - nw - 1, round(cx - nw / 2 + 0.5)))
nz0 = max(1, min(depth - nd - 1, round(cz - nd / 2 + 0.5)))
print(f"arena {iw}x{id_} at {ix0},{iz0} -> {nw}x{nd} at {nx0},{nz0}; grid {width}x{depth}x{layers}")


def column(x, z):
    return [kind(x, l, z) for l in range(layers)]


def text(kinds, own):
    # Layers left out keep the stage's own: air is written up to the highest layer either column uses.
    used = max([l + 1 for l, k in enumerate(kinds) if k] + [l + 1 for l, k in enumerate(own) if k] + [1])
    kinds = kinds[:used]
    return "".join("." if k == 0 else "%X" % k for k in kinds)


wall_src = (wx0, (wz0 + wz1) // 2)  # a wall tile of the stage's own for the new wall's look
shape, looks, tiles = [], [], []
for z in range(nz0 - 1, nz0 + nd + 1):
    for x in range(nx0 - 1, nx0 + nw + 1):
        inside = nx0 <= x < nx0 + nw and nz0 <= z < nz0 + nd
        if inside:
            sx, sz = ix0 + (x - nx0) % iw, iz0 + (z - nz0) % id_
            want = column(sx, sz)
        else:
            sx, sz = wall_src
            want = column(sx, sz)
        if want != column(x, z):
            shape.append([x, z, text(want, column(x, z))])
        if (x, z) != (sx, sz):
            looks.append([x, z, sx, sz])
        for l in range(layers):  # blocks: as on the source tile (no block / random block may fall)
            if want[l] == 1 and kind(x, l, z) != 1 or (want[l] == 1 and (tile(sx, l, sz) & 0x10) != (tile(x, l, z) & 0x10)):
                tiles.append([x, l, z, "free" if tile(sx, l, sz) & 0x10 else "random"])


def start(slot):  # the stage's own start of slots 0 to 3, in the quadrant of its corner
    for z in range(depth):
        for x in range(width):
            for l in range(layers):
                if (tile(x, l, z) >> 7 & 7) == slot + 1:
                    qx = 0 if x - ix0 < iw / 2 else 1
                    qz = 0 if z - iz0 < id_ / 2 else 1
                    return [nx0 + qx * iw + x - ix0, l, nz0 + qz * id_ + z - iz0]
    return None


level = {
    "name": name, "stage": stage, "version": 1,
    "rules": {"bombs": -1, "fire": -1, "ghosts": -1, "minutes": 3, "randomBlocks": 44, "suddenDeath": -1},
    "starts": [start(s) for s in range(4)] + [None] * 4,
    "tiles": tiles, "items": [], "shape": shape, "looks": looks,
}
print("starts", level["starts"], "shape", len(shape), "looks", len(looks), "tiles", len(tiles))
json.dump(level, open(out, "w"), indent=1)
