# Bomberman 64: Recompiled
## What this fork adds
This is a fork of [RevoSucks/BM64Recomp](https://github.com/RevoSucks/BM64Recomp). Download it from the [Releases](https://github.com/Mayo0x0/BM64Recomp/releases) page.

* **Up to 8 players in Battle mode**, humans or computer players. Players 5 to 8 are set up on a second Player Select page (move right past player 4) and play on controller ports 5 to 8. They are off until you turn them on; the game remembers your choice (`players.json`). Without start points from a level they start spread over the stage, each as far from the others as the stage allows. Includes an 8 player results screen and team battles with 4 teams.
* **Online multiplayer (netplay)**: lockstep netplay over UDP, several players per PC, direct or through a relay server. See [Online Multiplayer (Netplay)](#online-multiplayer-netplay).
* **Own level for every computer player**: each CPU plays at the level set for it. The original game used the level of the last started CPU for all of them.
* **Level editor** for the battle stages: change the stage's shape (raise and lower tiles, holes, ramps), where blocks are (soft, hard, none, random), where players 1 to 8 start, which item a block holds and some rules, right in the stage with the mouse. Open it with *Level Editor* in the launcher. See [Level editor](#level-editor).
* **Reset button** at the top of the in-game menu (Esc), also in netplay.
* **Cheat "Unlock all Battle Stages"** (Settings, Cheats): all 10 battle stages in Stage Select. Without it you get the original 6 and the ones your save unlocked. It applies when you next enter the Battle menu.
* **Several controllers**: one controller or keyboard per port, with rumble and controller pak / rumble pak per port (from [RevoSucks/BM64Recomp#17](https://github.com/RevoSucks/BM64Recomp/pull/17) and [#18](https://github.com/RevoSucks/BM64Recomp/pull/18)).
* **Battle mods** in `mods/` (also as `Mods.zip` in the release): *Greedy Trap Rising Water* and *Sudden Death Ghosts*.

How the 8 player slots are made is described in [tools/eightslots/README.md](tools/eightslots/README.md).

### Level editor
*Level Editor* in the launcher lists your levels. A level is based on one of the 10 battle stages; *New level* or *Edit* starts the program again straight into that stage with the editor at the side:

* **Left mouse button** paints the chosen tool onto the floor tile under the cursor (drag to paint more), the **right** one gives a tile back its stage's own state (with a shape tool: the whole column).
* **Shape:** *Higher* and *Lower* move a tile's floor up or down one layer (a raised tile is a wall with a floor on top), *Hole* takes the tile away (bombers fall through), *Ramp* makes the floor a ramp up to the next layer in one of four directions, *Flat floor* turns a ramp back into floor. The cursor picks the highest floor under the mouse, so a raised tile is edited by pointing at its top; the status bar shows the layer. The stage model is rebuilt to match: the changed tiles get new floors and walls with the stage's own textures.
* **Look:** the editor picks the textures of a changed tile from the nearest similar surface of the stage. To choose them yourself, click *Pick look* on any tile of the stage (a wall, a pillar, a floor tile), then paint it onto tiles with *Apply look*: they get that tile's top and sides. This works on unchanged floor tiles too. The right mouse button gives a tile the automatic look back.
* **Tools:** soft block, hard block (a taller rock that explosions do not break), free floor (never a block), floor where a random soft block may fall (dark squares), start points 1 to 8, an item for a soft block (fire, bomb, heart, power bomb, skull, remote), mirroring left/right, front/back or four ways, undo and redo (Ctrl+Z, Ctrl+Y), save (Ctrl+S). The stage is drawn beside the editor's panel; **Tab** (or the panel's *Ausblenden* button) hides the panel and gives the stage the whole window. The **mouse wheel** zooms in and out around the mouse, dragging with the **middle mouse button** moves the zoomed view, **0** or **Home** shows the whole stage again.
* **Rules** per level: minutes, Sudden Death, ghosts, bombs and fire at the start, how many random soft blocks the game scatters.

**Bigger stages:** a level may make a stage larger, out to the edges of the stage's grid. The battle camera then backs away to show all of it. [tools/levels/big_stage1.json](tools/levels/big_stage1.json) is an example: the first stage at twice its width and depth (the arena repeated in four quarters), made with [tools/levels/make_big_stage.py](tools/levels/make_big_stage.py). Copy it into your `levels` folder and set it active.

Levels are files in the `levels` folder of the app folder (next to the exe in portable mode). Set a level *Active* and it replaces its stage: pick that stage in the game and the level is played, one active level per stage. In netplay the host's active levels are sent to everyone with the save.

Limits: changed tiles are drawn as plain blocks with the stage's textures, so decorations on them go and their shading can differ a little from the rest; moving parts of a stage stay as they are. Computer players have not been checked on ramps they would need to reach a raised area. On a bigger stage the ground outside the stage's own model stays black, and the top row can sit under the score display.

---

[Bomberman 64: Recompiled](https://github.com/RevoSucks/BM64Recomp) is a project that uses [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) to **statically recompile** Bomberman 64 into a native port with many new features, enhancements, and extensive mod support. This project uses [RT64](https://github.com/rt64/rt64) as the rendering engine to provide some of these enhancements.

### **This repository and its releases do not contain game assets. The original game is required to build or run this project.**

## Table of Contents
* [System Requirements](#system-requirements)
* [Features](#features)
  * [Plug and Play](#plug-and-play)
  * [Fully Intact N64 Effects](#fully-intact-n64-effects)
  * [Easy-to-Use Menus](#easy-to-use-menus)
  * [High Framerate Support](#high-framerate-support)
  * [Widescreen and Ultrawide Support](#widescreen-and-ultrawide-support)
  * [Online Multiplayer (Netplay)](#online-multiplayer-netplay)
  * [Additional Control Options](#additional-control-options)
  * [Low Input Lag](#low-input-lag)
  * [Instant Load Times](#instant-load-times)
  * [Linux and Steam Deck Support](#linux-and-steam-deck-support)
* [Planned Features](#planned-features)
* [FAQ](#faq)
* [Known Issues](#known-issues)
* [Building](#building)
* [Libraries Used and Projects Referenced](#libraries-used-and-projects-referenced)

## System Requirements
Currently, only a **Windows** exe is provided but other operating systems will be supported soon.<br/>
A GPU supporting Direct3D 12.0 (Shader Model 6), Vulkan 1.2, or Metal Argument Buffers Tier 2 support is required to run this project. The oldest GPUs that should be supported for each vendor are:
* GeForce GT 630
* Radeon HD 7750 (the one from 2012, not to be confused with the RX 7000 series) and newer
* Intel HD 510 (Skylake)
* A Mac with Apple Silicon or an Intel 7th Gen CPU with MacOS 13.0+

On x86-64 PCs, a CPU supporting the SSE4.1 instruction set is also required (Intel Core 2 Penryn series or AMD Bulldozer and newer). ARM64 builds will work on any ARM64 CPU.

If you have issues with crashes on startup, make sure your graphics drivers are fully up to date. 

## Features

#### Plug and Play
Simply provide your copy of the 1.0 North American version of the game in the main menu and start playing! This project will automatically load assets from the provided copy, so there is no need to go through a separate extraction step or build the game yourself. Other versions of the game may be supported in the future.

#### Fully Intact N64 Effects
A lot of care was put into RT64 to make sure all graphical effects were rendered exactly as they did originally on the N64. No workarounds or "hacks" were made to replicate these effects, with the only modifications to them being made for enhancement purposes such as widescreen support.

#### Easy-to-Use Menus
Gameplay settings, graphics settings, input mappings, and audio settings can all be configured with the in-game config menu. The menus can all be used with mouse, controller, or keyboard for maximum convenience.

#### High Framerate Support
Play at any framerate you want thanks to functionality provided by RT64! Game objects and terrain, texture scrolling, screen effects, and most HUD elements are all rendered at high framerates. By default, this project is configured to run at your monitor's refresh rate. You can also play at the original framerate of the game if you prefer. **Changing framerate has no effect on gameplay.**

**Note**: External framerate limiters (such as the NVIDIA Control Panel) are known to potentially cause problems, so if you notice any stuttering then turn them off and use the manual framerate slider in the in-game graphics menu instead.

#### Widescreen and Ultrawide Support
Any aspect ratio is supported, with most effects modded to work correctly in widescreen. The HUD can also be positioned at 16:9 when using ultrawide aspect ratios if preferred.

**Note**: Some animation quirks can be seen at the edges of the screen in certain cutscenes when using very wide aspect ratios.

#### Mod Support
Install community made mods and texture packs! Mods can change any part of the game, including adding completely new features and content. You can install mods by simply dragging the mod files onto the game window before starting the game or by clicking the **Install Mods** button in the mod menu. Mods can be toggled in the mod menu, and some mods can be configured there as well.

If you're interested in making texture packs, check out [the RT64 documentation](https://github.com/rt64/rt64/blob/main/TEXTURE-PACKS.md).

#### Online Multiplayer (Netplay)
Play Battle mode with friends over the internet, up to 8 players in all. Every PC can bring several players: each port that is on in **Setup controls** is one player.

1. One player clicks **Netplay** in the main menu and **Host**. The others enter the host's address and click **Join**.
2. Once everyone is in the lobby, the host clicks **Start game**. All PCs start the game together; the host picks stage and rules in the game's own menus.

The host's save and cheat settings are used for the session; the others' own saves are not touched. Every PC must run the same build with the same mods.

The host's UDP port (7777 by default) has to be reachable: forward it in the router, or play over a VPN such as Tailscale or ZeroTier. Without that, use a relay server: run `BM64NetplayRelay` (built with the game, or from `tools/netplay_relay`) on a machine everyone can reach, enter its address as **Relay server** when hosting, and give the others the room code it shows. With **Visible in the relay's room list: Yes** the room is also listed on the relay: a player who joins with the relay's address and an empty room code sees every listed room (host's name, players, lobby or in game) and joins one with a click. With **No** the room is only joined with its code. The room list needs the relay from this version or later; an older relay still relays, but lists nothing.

Through a relay, every player also tries a direct connection to the host first (UDP hole punching, no port forwarding needed). The lobby shows each player's connection (**Direct** or **Relay**) and ping, and whether hole punching worked for everyone; a player whose router does not allow it stays on the relay. A direct connection that breaks during the game falls back to the relay by itself. Hole punching needs the relay and the game from this version or later. In the game, **F6** shows the ping to every player and their connection (F1 to F4 are taken by the renderer when its developer mode is on).

The **Reset** button at the top of the in-game menu (Esc) restarts the game; in a session it restarts it on every PC, and everyone ends up in the main menu.

The game runs in lockstep: each PC waits for the others' inputs, so the slowest connection sets the pace. Every two seconds the PCs compare a hash of the game state, and a PC that went out of step copies the host's.

#### Additional Control Options
Customize your experience by setting your stick deadzone to your liking, as well as adjusting the Y axis inversion for aiming.

#### Low Input Lag
This project has been optimized to have as little input lag as possible, making the game feel more responsive than ever!

#### Instant Load Times
Saving and loading files, going from place to place, and pausing all happen in the blink of an eye thanks to the game running natively on modern hardware.

#### Linux and Steam Deck Support
A Linux binary is available for playing on most up-to-date distros, including on the Steam Deck.

## FAQ

#### What is static recompilation?
Static recompilation is the process of automatically translating an application from one platform to another. For more details, check out the full description of how this project's recompilation works here: [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp).

#### How is this related to the decompilation project?
Unlike N64 ports in the past, this project is not based on the source code provided by a decompilation of the game. This is because static recompilation bypasses the need for decompiled source code when making a port, allowing ports to be made **without source code**. However, the reverse engineering work done by the decompilation team was invaluable for providing some of the enhancements featured in this project. For this reason, the project uses headers and some functions from the [Decompilation Project](https://github.com/RevoSucks/BM64Recomp) in order to make modifications to the game. Many thanks to the decompilation team for all of the hard work they've done.

#### Where is the savefile stored?
- Windows: `%LOCALAPPDATA%\BM64Recompiled\saves`
- Linux: `~/.config/BM64Recompiled/saves`
- macOS: `~/Library/Application Support/BM64Recompiled/saves`

#### How do I choose a different ROM?
**You don't.** This project is **only** a port of Bomberman 64, and it will only accept one specific ROM: the US version of the N64 release of Bomberman 64. ROMs in formats other than .z64 will be automatically converted, as long as it is the correct ROM. **It is not an emulator and it cannot run any arbitrary ROM.** 

Instead, you can change the game by installing mods. See the [mod support](#mod-support) section for details.

#### Can you run this project as a portable application?
Yes, if you place a file named `portable.txt` in the same folder as the executable then this project will run in portable mode. In portable mode, the save files, config files, and mods are placed in the same folder as the executable.

## Known Issues
* Overlays such as MSI Afterburner and other software such as Wallpaper Engine can cause performance issues with this project that prevent the game from rendering correctly. Disabling such software is recommended.

## Building
Building instructions on how to build this project can be found in the [BUILDING.md](BUILDING.md) file.

## Libraries Used and Projects Referenced
* [RT64](https://github.com/rt64/rt64) for the project's rendering engine
* [RmlUi](https://github.com/mikke89/RmlUi) for building the menus and launcher
* [lunasvg](https://github.com/sammycage/lunasvg) for SVG rendering, used by RmlUi
* [FreeType](https://freetype.org/) for font rendering, used by RmlUi  
* [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) for semaphores and fast, lock-free MPMC queues
* [Gamepad Motion Helpers](https://github.com/JibbSmart/GamepadMotionHelpers) for sensor fusion and calibration algorithms to implement gyro aiming
* [Bomberman 64 Decompilation](https://github.com/Bomberhackers/bm64) for headers and some function definitions, used for making patches or some enhancements
* [Ares emulator](https://github.com/ares-emulator/ares) for RSP vector instruction reference implementations, used in RSP recompilation

# Special Thanks:

* [ZeldaRET](https://github.com/zeldaret) : for teaching me the fundamental knowledge of N64 decompilation.
* [Decompals](https://github.com/decompals) : for helping in the initial disassembly and decompilation of the game.
* [Wiseguy](https://github.com/Mr-Wiseguy) and [DarioSamo](https://github.com/DarioSamo): for creating [N64Recomp](https://github.com/N64Recomp) and [RT64](https://github.com/rt64/rt64), assisting with fantastic technical support along the way.
