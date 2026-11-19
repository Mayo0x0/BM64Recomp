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
| `netplay_test.ps1` | Netplay on one PC: a host and clients with the test mod over localhost or a relay, with logs |
| `netplay_diff.py` | Compares two RAM dumps of a session (`BM64_NETPLAY_DUMP`) range by range |
| `netplay_ui.ps1` | Starts the game without mods in its own folder, for trying the Netplay page by hand |
| `netplay_menus.ps1` | Netplay through the real menus: the host's keys go to a battle, both players walk and bomb |
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
| Own level per computer player (the game keeps one in 0x802ACFC4, set by whichever started last; `eightslots_ai_level` sets each one's before its AI step, slots 5 to 8 from page 2 at 0x80707F40) | done |
| All 10 battle stages in Stage Select from the start (`VALUE_PATCHES`) | done |
| Start positions: a free floor tile near the partner (stage grid, `place_near`), checked on all stages (`stage_starts.sh`) | done |
| Team results: medals of earlier wins for all 4 teams, beside each team | done |
| Team battles with 8 players and up to 4 teams (blue, red, green, yellow): Team Select page 2, team squares, crystals and HUD gems of teams 3 and 4 (recolored red ones), round end, AI, results | done |
| Hits by thrown objects for slots 5 to 7: the per-player hit counters at 0x800BA8A0 (7 halfwords, room for 5) wrote into the enemy kind table (0x800BA8E8) and the first enemy record, so team crystals got wrong types and models and team battles crashed in `func_80285F8C`; moved (`HIT_COUNTS`), and `func_802797BC` finds slots 5 to 7 | fixed |
| Lift and ride table (`RIDE_TABLE`): the clears at stage start (`func_802714C4`, `func_802777D0`) only reached entries 0 to 4 | fixed |

Run the test: `build.sh`, build `testmod` with `-DPLAYERS_ADDRESS=0x80700000 -DAI_STATE_ADDRESS=0x80700640`, then
`test_battle.ps1 -Players 8`. `-DPRESET_EXTRA_WINS` sets the wins of slots 4 to 7 to 1 to 4 for a look at the HUD. `-DLOG_RIDE_TABLE` logs the lifted objects, `-DLOG_ENEMIES` the enemy pool and kind table, `-DLOG_EVERY=n` how often. `-DTEST_BOTS=0` turns the extra slots off (flag at 0x80707F20) for a 4-player
comparison. `sweep_stages.sh` covers all stages.

A mod must not hook a function that has a toml hook in `HOOKS` (func_80245CDC, func_8023CA94, func_8023EB28,
func_8023F26C, func_8023A104, g_initRandom, func_80252D14, g_getRandomNumber, g_nextRandomDouble): the runtime rebuilds it from the ROM without the hook.

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

## Netplay

`src/game/netplay.cpp` (lockstep over UDP), `src/ui/ui_netplay.cpp` with `assets/netplay.rml` (the page),
`tools/netplay_relay/relay.cpp` (relay server). Every PC runs the whole game; frame n is the n-th controller poll
(`func_8023A104`, hooked by `eightslots_read_extra_pads`). There each PC sends its local pads for frame n + delay
(3) to the host, the host sends back the 8 pads of frame n, and all write them into the game.

What made two PCs differ, found by dumping RAM at the same frames on both (`BM64_NETPLAY_DUMP=f1,f2,...`,
`BM64_NETPLAY_NORESYNC`) and comparing (`netplay_diff.py`):

- **The random seed.** The main function seeds `g_initRandom` with the clock (`0x80297E38`). The table at
  0x802ACFEC then differs, and the first random stage setup (frame ~215) goes different ways. In a session every PC
  takes the session id as seed (`HOOKS`: `g_initRandom` -> `eightslots_rng_seed`). After that the game state stays
  the same through whole battles.
- **Not game state, left out of checks and resyncs** (`SYNCED`, `UNSYNCED_WORDS` and `graphics_pointer` in
  netplay.cpp): libultra's thread structs and the scheduler (times, retrace counts), frame and depth buffers and
  display lists (0x800C0000-0x8021FFFF), the heap (models and animations follow the retrace counter 0x802A8954),
  sound (0x80098000-0x800A5FFF, 0x80220000-0x80224FFF), a one-frame flag at 0x80064434, the per-player array moved
  from the display task page (0x80701000), the float cache of `func_8024D108` (0x807019C0) and a word at 0x800BD7C4
  that looks like a sound handle.
- **Pointers into graphics memory.** The render thread `Gfx_Render` flips between two display list buffers
  (0x8014C8D0 + index * 0x27000) as frames get drawn, so which one a frame fills depends on timing, and objects keep
  pointers into them. Any word pointing into 0x800C0000-0x8021FFFF counts as 0 in checks.
- **Starting.** `recomp::start_game` before the UI has run once crashes the VI thread (the runtime is not set up),
  but a window in the background may not run its UI loop: the network thread starts the game once the UI has run.
- **Retraces only pace the main loop.** `Gfx_EndRender` waits for one retrace per frame (`func_80237F44`), and
  `ThreadProc_RunQueues` counts them; no game logic was found to depend on the count.

A check hashes 1/60 of the synced pages each frame, so one hash covers 60 frames and costs ~0.2 ms a frame (a full
hash every 60 frames cost 10 to 50 ms and was a visible hitch). A client whose hash differs asks for a resync: at a
frame the host chooses, the host keeps a copy of RAM and the client fetches the pages whose hash differs. Result:
CPU battles with the test mod and battles through the real menus (`netplay_menus.ps1`, 2 PCs on one machine) ran
with no resync in most sessions; about 1 check in 200 still fails for a moment and its resync finds nothing to copy. Test: `netplay_test.ps1 -Folder <dir> -HostPlayers 4 -HoldAt 24` with the test
mod built with `-DTEST_HUMANS=1` (the client's key moves player 5); add `-Relay 127.0.0.1:7778` with
`BM64NetplayRelay` running. `-Jitter 30` makes the client wait up to 30 ms before each frame, like a slow link
(`BM64_NETPLAY_JITTER`); with it, on Pyramid with 8 players and dumps of both PCs, the synced state stayed the same
and only flag bits in the excluded object pool and the float cache differed.
Team battles with 8 players and `-Jitter 30` (Pyramid): a page at 0x800A6000 differs for a moment now and then (the
object pool reaches past the excluded range; a bit 0x40000000 in its objects differs between PCs), and in about 1
session of 3 the PCs really part: once the host took one random number more (frame 1523), once positions, heap
pointers and the random index differed and the client crashed after the resync, which copies the host's heap
pointers although the heap is not synced. Not found yet: what takes the extra random number. `BM64_NETPLAY_TRACERNG`
writes every random number (frame, sp, a0, index) to `netplay_rng.txt` in the game's folder, to compare two PCs. `BM64_NETPLAY_CHECKLOG=1` logs the checks and frames slower than 8 ms (`=pages`: every
page's hash in each check, to compare the two logs). A resync logs the first word of each page that differs.

Not done: rumble and controller paks in a session (pads report no pak), IPv6, a client cannot join a running game,
and the check is a sampled hash (a difference outside `SYNCED` goes unnoticed).
