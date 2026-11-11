"""Lists the words of the globals that change with 8 players but never with 4 (logs of watch_runs.sh).

Usage: watch_diff.py logs/watch3_0.test.txt ... -- logs/watch3_4.test.txt ...
Before the "--": runs with 4 players; after it: runs with 8. A word must change in every 8 player run.
Each range is shown with the nearest address the code uses below it (the table it likely belongs to).
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def changed_words(path):
    lines = [l for l in open(path, encoding="utf-8-sig", errors="ignore") if l.startswith("TEST changed")]
    words = set()
    for a, b in re.findall(r"([0-9A-F]{8})-([0-9A-F]{8})", lines[-1]):
        words.update(range(int(a, 16), int(b, 16), 4))
    return words


def ranges(words):
    out = []
    for w in sorted(words):
        if out and out[-1][1] == w:
            out[-1][1] = w + 4
        else:
            out.append([w, w + 4])
    return out


def main():
    split = sys.argv.index("--")
    four = set().union(*(changed_words(p) for p in sys.argv[1:split]))
    eight = set.intersection(*(changed_words(p) for p in sys.argv[split + 1:]))
    from mips_refs import load_funcs, resolve
    users = {}
    for name, instructions in load_funcs().items():
        if "secure_call" in name:
            continue
        for r in resolve(instructions)[0]:
            users.setdefault(r.target, set()).add(name)
    known = sorted(users)
    moved = [l.split()[0] for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "moves.txt"))]
    for lo, hi in ranges(eight - four):
        i = bisect.bisect_right(known, lo) - 1
        base = known[i] if i >= 0 else 0
        names = " ".join(sorted(users.get(base, ()))[:4])
        print(f"{lo:08X}-{hi:08X} ({(hi - lo) // 4:3} words)  table {base:08X} +{lo - base:X}: {names}")


if __name__ == "__main__":
    main()
