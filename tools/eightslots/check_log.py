"""Judges a battle test log by what must hold in any correct run, since runs are not repeatable.

Usage: check_log.py logs/<label>.test.txt [expected player count]. Exit code 1 if a check fails.
"""
import re
import sys

STATE = re.compile(r"TEST f=(\d+) flags=(\w+) alive=(-?\d+) t=(-?\d+):(-?\d+)(.*)")
SLOT = re.compile(r"\| (\d+) c(\d+) (?:h(-?\d+) b(-?\d+) f(-?\d+) s(\w+) (-?\d+),(-?\d+),(-?\d+)|none)")
BATTLE_FLAG = 0x4
ROUND_OVER_FLAG = 0x10


def main():
    path = sys.argv[1]
    expected = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    rows = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            m = STATE.match(line)
            if not m:
                continue
            slots = {}
            for s in SLOT.finditer(m.group(6)):
                slots[int(s.group(1))] = s
            rows.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3)), int(m.group(4)) * 60 + int(m.group(5)), slots))

    battle = [r for r in rows if r[1] & BATTLE_FLAG]
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"{'ok  ' if ok else 'FAIL'} {name} {detail}")

    check("battle ran", len(battle) >= 10, f"{len(battle)} samples")
    if battle:
        first = battle[0]
        check("player count", len(first[4]) == expected and first[2] == expected,
              f"slots={sorted(first[4])} alive={first[2]}")
        check("timer runs", battle[-1][3] < first[3], f"{first[3]}s -> {battle[-1][3]}s")

        moved = set()
        last_pos = {}
        for row in battle:
            for slot, s in row[4].items():
                if s.group(7) is None:
                    continue
                pos = (int(s.group(7)), int(s.group(9)))
                if slot in last_pos and last_pos[slot] != pos:
                    moved.add(slot)
                last_pos[slot] = pos
        check("everyone moved", len(moved) == expected, f"moved={sorted(moved)}")

        # A battle plays several rounds; the count fills up again only when a new round's timer starts.
        counts = [r[2] for r in battle]
        rises = [(a, b) for a, b in zip(battle, battle[1:]) if b[2] > a[2]]
        check("alive count rises only at a new round", all(b[2] == expected and b[3] > a[3] for a, b in rises),
              f"rounds={len(rises) + 1} lowest={min(counts)}")
        check("someone died", min(counts) < expected)
        check("stats sane", all(0 <= int(s.group(3)) <= 9 for r in battle for s in r[4].values() if s.group(3) is not None))

        out = {slot for r in battle for slot, s in r[4].items() if s.group(2) == "3"}
        ghosts = {slot for r in battle for slot, s in r[4].items() if s.group(6) and int(s.group(6), 16) & 0x200}
        print(f"     out={sorted(out)} ghosts={sorted(ghosts)}")

    check("a round ended", any(r[1] & ROUND_OVER_FLAG for r in rows))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
