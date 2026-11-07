# Eight player slots

Goal: 8 real player slots in Battle (at most 4 humans, the rest CPU). This is a build of its own, not a mod.

## How it works

The game's code is changed by `[[patches.instruction]]` entries, so the ROM stays as it is.
`gen_patches.py` writes them into `bm64.eightslots.toml`, which is the normal config plus those entries.
Arrays too small for 8 players move to free memory by rewriting the `lui` and low half of every
reference. Every patch is checked against the ROM's original instruction.

Memory: the heap was 1 MB at 0x80300000 and is now 4 MB. Moved arrays start at 0x80700000.

## Files

| File | Purpose |
|---|---|
| `mips_refs.py` | Dataflow over each function: which `lui` reaches which use |
| `check_coverage.py` | Lists `lui` without a resolved use; only non-addresses (0x8000) remain |
| `inventory.py` | Lists run-time indexed globals with their stride (`candidates.txt`) |
| `gen_patches.py` | Writes the patches and the moved symbol file; stops on a conflict |
| `build.sh` | Generates, recompiles, forces the C patches to rebuild, builds |
| `testmod/` | Starts a seeded battle of CPU players without the menus and logs its state |
| `run_battle_test.ps1` | Runs it in an isolated folder (`BM64_AUTOSTART`) |
| `check_log.py` | Judges the log by rules that hold in any correct run |
| `crash_at.py` | Names the function behind a crash offset, using the linker map |

The analysis reads `.analysis/`, the recompiler's output for the unpatched config, because the game's own
output shows the patches already applied. To create it, run N64Recomp on a copy of `bm64.us.toml` with
`output_func_path = "tools/eightslots/.analysis"`.

## What was found

- **Runs are not repeatable**, even with the same seed, so builds cannot be compared log by log.
  `check_log.py` checks invariants instead.
- **A mod's hook undoes the patches of the hooked function.** The runtime rebuilds hooked functions from
  `recomp::get_rom()`, the original ROM. Hooks must stay on unpatched functions until this is solved, which
  also breaks existing mods on patched functions. The way out: apply the main section's patches to the ROM
  in memory with `recomp::set_rom_contents` at startup.
- **Overlay functions cannot be hooked at all:** `get_rom()` is the compressed ROM, where overlays are not at
  their decompressed address. Main code is.
- With the minutes setting at 1 the timer drops to 0:59 at once, so the whole battle is Sudden Death and there
  are no ghosts. Use 2 to test ghosts.
- `g_SpawnPlayer` has model cases for slots 0 to 3 only; other slots reuse the model of the slot spawned before.

## Status

- [x] Milestone 0: test harness, heap at 4 MB
- [x] Milestone 1: `g_Players` moved (258 references, 0 conflicts); a full 3-round CPU battle passes

## Next

1. **Patches in the runtime's ROM**, so hooks and the existing mods keep working.
2. **Move the other per-player data**, one group at a time, with a test after each:
   - the block at 0x802AC8A8 to 0x802ACA90 (see `candidates.txt`), 5 or 6 entries each
   - 0x800AF1D8 onwards (stride 0x400)
   - CPU state at 0x800BCFE8 (stride 0x128, 5 entries) and commands at 0x802ACF08 (stride 0x10)
   - 0x802AC714, 0x802AC858 and 0x802AC868, and the battle config's 4-entry arrays at 0x802AC660
3. **Raise the loop bounds** from 4 and 5 to 8 (35 functions with a literal 4 in the first count) and CPU target
   choice (`func_80251708`).
4. **Spawn 8 players:** start positions per stage, models for slots 4 to 7, CPU setup.
5. **Round logic and HUD:** winner search, result screen, 8 faces.
6. Menus, then human players on slots 0 to 3 next to CPUs.
