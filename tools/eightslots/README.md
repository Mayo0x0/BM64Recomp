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
| `find_bounds.py` | Finds addresses used as a loop's end |
| `find_limits.py` | Lists constants 3, 4 and 5 in player code (`limits.txt`) |
| `inventory.py` | Lists run-time indexed globals with their stride (`candidates.txt`) |
| `gen_patches.py` | Writes the patches and the moved symbol file; stops on a conflict |
| `build.sh` | Generates, recompiles, forces the C patches to rebuild, builds |
| `testmod/` | Starts a seeded battle of CPU players without the menus and logs its state |
| `run_battle_test.ps1` | Runs it in an isolated folder (`BM64_AUTOSTART`) |
| `check_log.py` | Judges the log by rules that hold in any correct run |
| `crash_at.py` | Names the functions of a crash's stack, using the linker map |
| `test_battle.ps1` | One test: runs it, judges it and prints the stack of a crash |
| `sweep_stages.sh` | The test on all 10 stages |
| `summarize_log.py` | Round flow of a log: deaths, round ends |

The analysis reads `.analysis/`, the recompiler's output for the unpatched config, because the game's own
output shows the patches already applied. To create it, run N64Recomp on a copy of `bm64.us.toml` with
`output_func_path = "tools/eightslots/.analysis"`.

## What was found

- **Runs are not repeatable**, even with the same seed, so builds cannot be compared log by log.
  `check_log.py` checks invariants instead.
- **Hooks rebuild functions from the ROM.** `GameEntry.decompression_routine` is used for nothing else, so
  `src/main/main.cpp` hands the runtime a copy of the ROM with the main section's patches applied. The game
  keeps reading the original. Toml hooks (`HOOKS`) are still lost if a mod hooks that same function.
- **Overlay functions cannot be hooked:** the ROM holds them compressed, at other addresses.
- **An address next to moved data is ambiguous.** The one past an array is usually the next variable
  (0x800AF1D8 is a pool table), and the start of a moved array can be the end of another (0x800BCFE8 ends the
  table at 0x800BCDE8). `new_target` leaves both alone unless `LOOP_BOUNDS` says otherwise.
- **With the minutes setting at 1** the timer drops to 0:59 at once: all Sudden Death, no ghosts. Use 2.
- **Not per player:** the 0x400 tables at 0x800AF1D8, indexed by a global counter.
- **Bash heredocs turn `\n` in Python into a real line break here.** Edit such strings with the Edit tool.

## Status

| Milestone | State |
|---|---|
| Test harness, heap at 4 MB | done |
| Hooks keep the patches | done |
| Per-player data moved, with room for 8 | done (see `moves.txt`) |
| 8 CPU bombers play, die, win rounds on all 10 stages | done, set up by the test mod |
| Ghosts in slots 4 to 7 | spawn and play, but see "Open" |
| Slots 4 to 7 set up by the game itself (`patches/eightslots.c`) | done |
| Human player with 4 extra CPUs through the real menus | done, played by hand once |
| CPUs target slots 4 to 7 | done (`func_80251708`) |
| Own colors for slots 4 to 7 (green, yellow, purple, orange): models and HUD faces | done |
| HUD second row: faces and wins of slots 4 to 7 | done |
| Wins of slots 4 to 7: config block + 0xB4 (its slot flags moved away), result codes 0x19 to 0x1C | done |
| Results screen and match winner screen (overlay `ovl3_7`) with up to 8 bombers | done |
| Player Select page 2 for players 5 to 8 (COM or off), with a page arrow | done |
| CPUs stood still (speeds and difficulty overwritten by slots 6 and 7 via 0x802ACF68) | fixed |
| All 10 battle stages in Stage Select from the start (`VALUE_PATCHES`) | done |
| Team battles with 8 players and up to 4 teams (blue, red, green, yellow): Team Select page 2, team squares, crystals and HUD gems of teams 3 and 4 (recolored red ones), round end, AI, results | done |

Run the test: `build.sh`, build `testmod` with `-DPLAYERS_ADDRESS=0x80700000 -DAI_STATE_ADDRESS=0x80700640`, then
`test_battle.ps1 -Players 8`. `-DPRESET_EXTRA_WINS` sets the wins of slots 4 to 7 to 1 to 4 for a look at the HUD. `-DTEST_BOTS=0` turns the extra slots off (flag at 0x80707F20) for a 4-player
comparison. `sweep_stages.sh` covers all stages.

A mod must not hook a function that has a toml hook in `HOOKS` (func_80245CDC, func_8023CA94, func_8023EB28,
func_8023F26C): the runtime rebuilds it from the ROM without the hook.

## Next

1. **Start positions:** a bot starts one tile from slot `n - 4` and can land in a block on some stages.
2. **Result codes 0x19 to 0x1C** (round won by slot 4 to 7) are known to the results screen only. Other readers
   of 0x802AC5D8 in battle have not been checked one by one.
3. **Team results screen:** earlier wins of teams 3 and 4 get no medals in the list (the screen draws them for 2
   teams), and those of teams 1 and 2 sit where the 2 team layout puts them.
4. **Per-player data found only at run time:** the CPU standstill came from a table the analysis missed
   (0x802ACF68, reached through a base and an index). There may be more like it: a write past an old table shows
   up as a global value changing (Cheat Engine write breakpoint, RDRAM base = the address of the bytes
   `03 00 00 00 02 00 00 00 01 00 00 00` at 0x802A1038, minus 0x2A1038).
5. **The mods:** `sudden_death_ghosts` and `greedy_trap_water` use fixed addresses; check them against `moves.txt`.

## Team battles

Teams live in the config block (+ 0x90, slots 0 to 3) and in `sExtraTeam` (slots 4 to 7), values 0 to 3. The
per-team tables (crystal standing, crystal, HUD gems, gem column x and tile) moved to fixed addresses for 4
(`TEAM_MOVES`). A crystal is an enemy whose kind is its model file: 0x1E blue, 0x1F red; teams 2 and 3 use the
made-up files 0x7F20 and 0x7F21, red crystals whose palette (only the colors the texture uses: vertices follow)
is tinted green or yellow, and the HUD sheets 0x7F30 and 0x7F31 with the red gem tinted. Result codes: 0x16 +
team for teams 0 and 1, 0x1D and 0x1E for teams 2 and 3.
