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
| `late_index.py` | Tables indexed after their address is formed (`addiu t0, base` then `addu`), which `inventory.py` misses |
| `watch_runs.sh` | Battles with 4 and 8 players, logging which words of the globals change (`WATCH_CHANGES`) |
| `watch_diff.py` | Words that change in every 8 player run but in no 4 player run, with the table below each |
| `human_test.ps1` | Player 5 as a human on port 5: holds a key bound to its stick and logs the pad |
| `gamepad_test.py` | Plugs in 8 virtual gamepads (ViGEmBus, `pip install vgamepad`), each stick at its own x |

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
| Human players 5 to 8 on the frontend's ports 5 to 8 (MAN on page 2 when the port is on) | done, played by hand once |
| CPUs stood still (speeds and difficulty overwritten by slots 6 and 7 via 0x802ACF68) | fixed |
| All 10 battle stages in Stage Select from the start (`VALUE_PATCHES`) | done |
| Start positions: a free floor tile near the partner (stage grid, `place_near`), checked on all stages (`stage_starts.sh`) | done |
| Team results: medals of earlier wins for all 4 teams, beside each team | done |
| Team battles with 8 players and up to 4 teams (blue, red, green, yellow): Team Select page 2, team squares, crystals and HUD gems of teams 3 and 4 (recolored red ones), round end, AI, results | done |

Run the test: `build.sh`, build `testmod` with `-DPLAYERS_ADDRESS=0x80700000 -DAI_STATE_ADDRESS=0x80700640`, then
`test_battle.ps1 -Players 8`. `-DPRESET_EXTRA_WINS` sets the wins of slots 4 to 7 to 1 to 4 for a look at the HUD. `-DTEST_BOTS=0` turns the extra slots off (flag at 0x80707F20) for a 4-player
comparison. `sweep_stages.sh` covers all stages.

A mod must not hook a function that has a toml hook in `HOOKS` (func_80245CDC, func_8023CA94, func_8023EB28,
func_8023F26C): the runtime rebuilds it from the ROM without the hook.

## Checked

- **Result codes 0x19 to 0x1C:** in battle only `func_80242994` writes 0x802AC5D8 (the others write fixed codes of
  other modes). It is read by `func_8023DB0C`, which hands it to the results overlay (ovl3_7, mapped back there), and
  by `func_8023C374`, which compares it with 0x10 to 0x13 in modes 4 and 5 only. The copy at 0x802AC5E0 is only
  written. Rounds won by slots 5 and 6 (0x1A, 0x1B) went on normally.
- **Tables with room for 4:** `late_index.py` finds the pattern that hid 0x802ACF68; its 4 entry tables in main
  code are indexed by other things (controller paks at 0x802AFF70, timer digits at 0x802AC770, an area at
  0x802ACEC8, 0x802B03C0 by a global). `watch_runs.sh` on all 10 stages and team battles on stages 3 and 6: every
  word that changes only with 8 players is in a pool (files 0x800A7F30, 0x800BCDE8 and 0x80090080, objects
  0x800BB550, heap 0x800A0DDC and 0x800A3F00, enemies 0x800BA8E8 for the crystals of teams 3 and 4), the wins of
  slots 4 to 7 (+ 0xB4 of the config block) or the HUD table the second row borrows (0x802A07D4). In team battles
  the alive count drops from 8 to 0 at once, so `check_log.py`'s "someone died" fails there with 4 players too.
- **The mods:** `greedy_trap_water` reads the players through `g_Players`; `sudden_death_ghosts` uses no moved
  address. `battle_extra_bot` was removed: the build does its job and it hooked functions with toml hooks.

For a value that still goes wrong at run time: a Cheat Engine write breakpoint on it finds the writer (RDRAM base =
the address of the bytes `03 00 00 00 02 00 00 00 01 00 00 00` at 0x802A1038, minus 0x2A1038).

## Human players 5 to 8

The game polls 4 controllers (`osContGetReadData`) into 0x802AC410, keeps a ring of 16 states per controller at
0x802AC428 and each ring's position at 0x802AC5A8; a human player (type 1) reads the controller of its slot. These
three arrays moved to room for 8 (`CONT_PADS`, `CONT_RINGS`, `CONT_RING_AT`; their loop ends in `LOOP_BOUNDS`), and
a hook after the poll fills pads 5 to 8 from `recomp_get_extra_pads`, which reads the frontend's ports 5 to 8
(`recomp::max_ports` is 8; Settings > Controls has P1 to P8). The types the game reads for slots 4 to 7 come from
`eightslots_extra_type`. Page 2 of Player Select turns a type off, COM, MAN, MAN only with that player's port on.
`human_test.ps1` binds port 5's stick right to M, holds it in a battle and logs the pad (test mod `-DTEST_HUMANS=1`).
8 gamepads: 8 virtual PS4 pads (`gamepad_test.py 60 ds4`, all ports set to Controller) are given ports 1 to 8
and each one's stick and A reach its player (test mod `-DTEST_HUMANS=15` logs every pad). 8 virtual Xbox 360 pads
are given ports 1 to 8 too, but SDL reads 0 from them through RawInput, outside the game as well; with RawInput off
SDL sees only 4 (XInput). Real Xbox pads were not tested. Start of players 5 to 8 pauses too (`func_8023AEDC` asks 8 controllers). Not done: rumble and controller paks of ports 5
to 8; the custom row shows on page 2 but is not used (slots 4 to 7 keep their colored bombers).

## Team battles

Teams live in the config block (+ 0x90, slots 0 to 3) and in `sExtraTeam` (slots 4 to 7), values 0 to 3. The
per-team tables (crystal standing, crystal, HUD gems, gem column x and tile) moved to fixed addresses for 4
(`TEAM_MOVES`). A crystal is an enemy whose kind is its model file: 0x1E blue, 0x1F red; teams 2 and 3 use the
made-up files 0x7F20 and 0x7F21, red crystals whose palette (only the colors the texture uses: vertices follow)
is tinted green or yellow, and the HUD sheets 0x7F30 and 0x7F31 with the red gem tinted. Result codes: 0x16 +
team for teams 0 and 1, 0x1D and 0x1E for teams 2 and 3.

## Stage grid

`func_8026D7D8(tile x, level, tile z)` returns a tile: kind in the low 4 bits (1 floor, 0 air, 0xF solid), bits
above for bombers standing there; off the grid 0x2010. Tiles are 100 wide (x = tile * 100), a level's height is
`func_8026E2B0(level)`. `func_80251AC8(tile)` is the computer players' "cannot walk here". The test mod logs the tile
under every bomber at the start; `stage_starts.sh` does that for all 10 stages.
