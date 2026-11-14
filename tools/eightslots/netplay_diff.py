# Compares two RAM dumps of a netplay session (BM64_NETPLAY_DUMP): the ranges of differing words, merged when
# closer than GAP bytes, with the first differing words. Usage: netplay_diff.py <host.bin> <client.bin> [gap]
import struct
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
gap = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
words = len(a) // 4
wa = struct.unpack(f"<{words}I", a)  # stored word-swapped, so a little endian word is the N64's word
wb = struct.unpack(f"<{words}I", b)
diff = [i * 4 for i in range(words) if wa[i] != wb[i]]
ranges = []
for offset in diff:
    if ranges and offset - ranges[-1][1] <= gap:
        ranges[-1][1] = offset
        ranges[-1][2] += 1
    else:
        ranges.append([offset, offset, 1])
print(f"{len(diff)} differing words in {len(ranges)} ranges")
for start, end, count in ranges:
    sample = " ".join(f"{wa[o // 4]:08X}/{wb[o // 4]:08X}" for o in range(start, min(end + 4, start + 16), 4))
    print(f"  {0x80000000 + start:08X}-{0x80000000 + end + 3:08X} {count:6d} words  {sample}")
