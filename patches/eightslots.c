// Eight player slots: slots 4 to 7 are computer or human players in battles (tools/eightslots).
// gen_patches.py calls these from hooks in func_80245CDC and func_8023CA94.
#include "patches.h"
#include "input.h"

#define FIRST_EXTRA_SLOT 4
#define EXTRA_SLOTS 4
#define CONTROL_NONE 0
#define CONTROL_HUMAN 1
#define CONTROL_COMPUTER 2
#define CONTROL_OUT 3
#define TILE_SIZE 100.0f

#define GAME_FLAGS (*(volatile s32*)0x802AC5D0)
#define GAME_FLAG_BATTLE 0x4
#define BATTLE_TYPE (*(volatile s32*)0x802AC660) // 0: last bomber standing, 1: teams
#define BATTLE_TEAMS 1
#define ALIVE_COUNT (*(volatile s32*)0x802AC758)
#define AI_DIFFICULTY (*(volatile s32*)0x802ACFC4) // the computer players' level, read by func_80252D14
#define BATTLE_DIFFICULTIES ((volatile s32*)0x802AC674) // menu level of slots 0 to 3 (config block + 0x14)
#define EXTRA_DIFFICULTIES ((volatile s32*)0x80707F40) // menu level of slots 4 to 7 (page 2); 1 at boot
#define AI_LEVELS ((volatile s32*)0x802A1038)          // menu level -> AI_DIFFICULTY (func_80251804)

typedef struct SlotModel {
    u8 pad[0x10];
    f32 x, y, z;
} SlotModel;

typedef struct SlotActor {
    u8 pad[0x40];
    SlotModel* model;
} SlotActor;

typedef struct SlotPlayer {
    f32 spawn[3];
    u8 pad0C[0x14 - 0xC];
    s32 hearts;
    s32 bombType;
    s32 bombCount;
    s32 firePower;
    s32 controlType;
    SlotActor* actor;
    u8 pad2C[0x94 - 0x2C];
    s32 bombTypeStart;
    s32 firePowerStart;
    u8 pad9C[0xC8 - 0x9C];
} SlotPlayer;

#define STATES_OFFSET 0x68 // GHOST_STATE

extern SlotPlayer g_Players[8];
void func_80251804(s32 slot);    // starts a slot's computer player process
void* func_8026CE28(s32 fileID); // loads a file, or takes it from the file cache

// Nonzero turns the extra slots off; the test mod uses it to compare with the original 4.
#define EXTRA_SLOTS_OFF (*(volatile s32*)0x80707F20)

static s32 extra_slots_wanted(void) {
    return (GAME_FLAGS & GAME_FLAG_BATTLE) && BATTLE_TYPE <= BATTLE_TEAMS && !EXTRA_SLOTS_OFF;
}

// Player models for slots 4 to 7: file ids that the ROM does not have. g_SpawnPlayer asks for
// RECOLORED_MODEL + slot - 4 (gen_patches.py), and the file loader serves a recolored copy of the white bomber.
#define RECOLORED_MODEL 0x7F00
#define WHITE_BOMBER_MODEL 0x4A
#define MODEL_TEXTURE 0x15 // header entry: CI8 texture
#define MODEL_PALETTE 0x1A // header entry: palette of RGBA5551 colors
#define MODEL_HEADER_START 0xC
#define MODEL_HEADER_END 0
#define MODEL_HEADER_MAX 64 // entries; the icon sheet has 16

// Body colors of slots 4 to 7: green, yellow, purple, orange.
static const u8 sExtraColors[EXTRA_SLOTS][3] = {
    { 0x40, 0xD0, 0x40 },
    { 0xF0, 0xD8, 0x20 },
    { 0xA0, 0x50, 0xE0 },
    { 0xFF, 0x80, 0x10 },
};

// The battle HUD's face icons (file 0x130, same format: tile 2 * slot, +8 when out) only exist for slots 0
// to 3, so slots 4 to 7 get recolored copies of the sheet too and are drawn in a second row.
#define RECOLORED_ICONS 0x7F10
#define HUD_ICONS 0x130
#define ICON_ALIVE_TILE 0
#define ICON_OUT_TILE 8
#define HUD_ELEMENTS ((volatile u32*)0x802A07D4) // { tile, x0, y0, x1, y1 } in 10.2, face of slot n is n + 1
#define HUD_ELEMENT_WORDS 5
#define HUD_SECOND_ROW (20 << 2)
#define GHOST_STATE 0x200

static void* sIconSheets[EXTRA_SLOTS];
// Which extra slots play this round: the results screen comes after the battle flag and the players are reset.
static s32 sPlaying[EXTRA_SLOTS];

// Wins by slot: the config block's + 0xA4, grown into + 0xB4 (gen_patches.py moved the flags there).
#define WINS ((volatile s32*)0x802AC704)

// The HUD's wins of slots 0 to 3 are sprite groups 3 to 6: one 10 x 18 CI4 digit each, tile 2 * wins of
// the sheet the HUD keeps, all with the palette of tile 1.
#define HUD_DIGITS (*(u8* volatile*)0x802A075C)
#define HUD_GROUP_X ((volatile s32*)0x802A079C) // pixels
#define FIRST_WINS_GROUP 3
#define DIGIT_PALETTE_TILE 1
#define DIGIT_WIDTH 10
#define DIGIT_HEIGHT 18
#define DIGIT_TOP (22 << 2)
#define MAX_DIGIT 9

static u32* sheet_entry(u8* sheet, s32 tile) {
    return (u32*)(sheet + MODEL_HEADER_START + tile * 12);
}

// Which extra slot a made-up file id belongs to, or -1; *real is the ROM's file to start from.
// Team battles have up to 4 teams: blue (0), red (1), green (2), yellow (3). Teams 2 and 3 get recolored copies
// of the red team's crystal (model file 0x1F) and of the HUD sheet holding the red gem (tile 0x16 of 0x130).
#define TEAMS 4
#define FIRST_NEW_TEAM 2
#define RECOLORED_CRYSTAL 0x7F20
#define RED_CRYSTAL 0x1F
#define RECOLORED_GEMS 0x7F30
#define NEW_TEAM_MARK 100 // recolored_slot() result for a new team's file: NEW_TEAM_MARK + team - FIRST_NEW_TEAM

static s32 recolored_slot(s32 fileID, s32* real) {
    if (fileID >= RECOLORED_CRYSTAL && fileID < RECOLORED_CRYSTAL + TEAMS - FIRST_NEW_TEAM) {
        *real = RED_CRYSTAL;
        return NEW_TEAM_MARK + fileID - RECOLORED_CRYSTAL;
    }
    if (fileID >= RECOLORED_GEMS && fileID < RECOLORED_GEMS + TEAMS - FIRST_NEW_TEAM) {
        *real = HUD_ICONS;
        return NEW_TEAM_MARK + fileID - RECOLORED_GEMS;
    }
    if (fileID >= RECOLORED_MODEL && fileID < RECOLORED_MODEL + EXTRA_SLOTS) {
        *real = WHITE_BOMBER_MODEL;
        return fileID - RECOLORED_MODEL;
    }
    if (fileID >= RECOLORED_ICONS && fileID < RECOLORED_ICONS + EXTRA_SLOTS) {
        *real = HUD_ICONS;
        return fileID - RECOLORED_ICONS;
    }
    *real = fileID;
    return -1;
}

s32 eightslots_real_file(s32 fileID) {
    s32 real;
    recolored_slot(fileID, &real);
    return real;
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static u16 to_rgba5551(s32 r, s32 g, s32 b) {
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
}

// The white bomber's body is grey and its suit blue; skin, hands and buckle keep their colors.
static void recolor_palette(u16* colors, s32 count, const u8* body) {
    for (s32 i = 0; i < count; i++) {
        u16 c = colors[i];
        s32 r = ((c >> 11) & 31) << 3;
        s32 g = ((c >> 6) & 31) << 3;
        s32 b = ((c >> 1) & 31) << 3;
        s32 high = MAX(r, MAX(g, b));
        s32 low = MIN(r, MIN(g, b));
        if (!(c & 1)) {
            continue; // transparent
        }
        if (high - low < 0x18 && high >= 0x80) { // grey: the body, shaded by its brightness
            colors[i] = to_rgba5551(body[0] * high / 255, body[1] * high / 255, body[2] * high / 255);
        } else if (b > r + 0x40 && b > g + 0x40) { // blue: the suit, a dark shade of the body color
            colors[i] = to_rgba5551(body[0] * b / 640, body[1] * b / 640, body[2] * b / 640);
        }
    }
}

// A new team's color from a red (gem) or magenta (crystal) original: each color keeps its brightness and takes
// the tint of team 2 (green) or 3 (yellow).
static const u8 sNewTeamTints[TEAMS - FIRST_NEW_TEAM][3] = {
    { 0x50, 0xFF, 0x50 },
    { 0xFF, 0xE0, 0x30 },
};

static void recolor_team_palette(u16* colors, s32 count, s32 newTeam) {
    const u8* tint = sNewTeamTints[newTeam];
    for (s32 i = 0; i < count; i++) {
        u16 c = colors[i];
        s32 r = ((c >> 11) & 31) << 3;
        s32 g = ((c >> 6) & 31) << 3;
        s32 b = ((c >> 1) & 31) << 3;
        s32 v = MAX(r, MAX(g, b));
        colors[i] = (to_rgba5551(tint[0] * v / 255, tint[1] * v / 255, tint[2] * v / 255) & ~1) | (c & 1);
    }
}

// The crystal model loads its palette from its display list: G_SETTIMG RGBA16 of segment 2 (the file), then
// G_LOADTLUT with the color count. Every such palette is recolored.
#define CRYSTAL_SCAN 0x2000 // bytes; the red crystal's list ends before 0xA80
#define GBI_SETTIMG_RGBA16 0xFD100000
#define GBI_LOADTLUT 0xF0
#define GBI_FILE_SEGMENT 0x02000000

#define GBI_SETTIMG_CI16 0xFD500000 // a CI texture loaded as 16 bit texels
#define GBI_LOADBLOCK 0xF3

// The colors a CI8 texture uses: its highest index + 1. The file's TLUT load asks for 256, but vertices follow
// the colors in use.
static s32 colors_used(u8* file, u32* cmd, s32 loaded) {
    for (u32 next = 2; next < 32; next += 2) {
        if (cmd[next] != GBI_SETTIMG_CI16 || (cmd[next + 1] & 0xFF000000) != GBI_FILE_SEGMENT) {
            continue;
        }
        for (u32 load = next + 2; load < next + 16; load += 2) {
            if ((cmd[load] >> 24) == GBI_LOADBLOCK) {
                u8* texels = file + (cmd[next + 1] & 0xFFFFFF);
                s32 bytes = (((cmd[load + 1] >> 12) & 0xFFF) + 1) * 2;
                s32 highest = 0;
                for (s32 i = 0; i < bytes; i++) {
                    highest = MAX(highest, texels[i]);
                }
                return MIN(highest + 1, loaded);
            }
        }
    }
    return 0; // no texture found: leave the palette alone
}

static void recolor_crystal(u8* file, s32 newTeam) {
    for (u32 at = 0; at + 16 <= CRYSTAL_SCAN; at += 8) {
        u32* cmd = (u32*)(file + at);
        if (cmd[0] != GBI_SETTIMG_RGBA16 || (cmd[1] & 0xFF000000) != GBI_FILE_SEGMENT) {
            continue;
        }
        // The load follows within a few commands (G_SETTILE, G_RDPLOADSYNC, ...): 4 in the red crystal.
        for (u32 next = 2; next < 16; next += 2) {
            if ((cmd[next] >> 24) == GBI_LOADTLUT) {
                s32 count = colors_used(file, cmd, ((cmd[next + 1] >> 14) & 0x3FF) + 1);
                recolor_team_palette((u16*)(file + (cmd[1] & 0xFFFFFF)), count, newTeam);
                break;
            }
        }
    }
}

void eightslots_after_file_load(s32 fileID, void* data) {
    s32 real;
    s32 slot = recolored_slot(fileID, &real);
    if (slot < 0) {
        return;
    }
    u8* file = data;
    if (slot >= NEW_TEAM_MARK && real == RED_CRYSTAL) {
        recolor_crystal(file, slot - NEW_TEAM_MARK);
        return;
    }
    if (slot >= NEW_TEAM_MARK) {
        for (u32 at = MODEL_HEADER_START; at < MODEL_HEADER_START + MODEL_HEADER_MAX * 12; at += 12) {
            u32 type = *(u32*)(file + at);
            if (type == MODEL_PALETTE) {
                recolor_team_palette((u16*)(file + *(u32*)(file + at + 8)), *(u32*)(file + at + 4), slot - NEW_TEAM_MARK);
            } else if (type == MODEL_HEADER_END) {
                break;
            }
        }
        return;
    }
    const u8* body = sExtraColors[slot];
    // Texture entries differ by format (0x15 for the CI8 models), so only the end of the list stops this.
    for (u32 at = MODEL_HEADER_START; at < MODEL_HEADER_START + MODEL_HEADER_MAX * 12; at += 12) {
        u32 type = *(u32*)(file + at);
        if (type == MODEL_PALETTE) {
            recolor_palette((u16*)(file + *(u32*)(file + at + 8)), *(u32*)(file + at + 4), body);
        } else if (type == MODEL_HEADER_END) {
            break;
        }
    }
}

// Players 5 to 8 as chosen on page 2 of Player Select: CONTROL_HUMAN, CONTROL_COMPUTER or CONTROL_NONE. Off
// until chosen; the frontend keeps the last choice (players.json) and hands it over once (load_extra_players).
static s32 sExtraType[EXTRA_SLOTS] = { CONTROL_NONE, CONTROL_NONE, CONTROL_NONE, CONTROL_NONE };
// Their teams (page 2 of Team Select); players 1 to 4 keep theirs in the config block (+ 0x90).
static s32 sExtraTeam[EXTRA_SLOTS] = { 0, 1, 0, 1 };
#define BLOCK_TEAM(slot) (*(volatile s32*)(0x802AC660 + 0x90 + (slot) * 4))
#define BLOCK_TYPE(slot) (*(volatile s32*)(0x802AC660 + 0x4 + (slot) * 4))

static s32 team_of(s32 slot) {
    return slot < FIRST_EXTRA_SLOT ? BLOCK_TEAM(slot) : sExtraTeam[slot - FIRST_EXTRA_SLOT];
}

// Whether a slot takes part, as chosen in the menu.
static s32 slot_chosen(s32 slot) {
    return slot < FIRST_EXTRA_SLOT ? BLOCK_TYPE(slot) != CONTROL_NONE
                                   : !EXTRA_SLOTS_OFF && sExtraType[slot - FIRST_EXTRA_SLOT] != CONTROL_NONE;
}

// The type the game reads for slots 4 to 7 (hooks in func_8023EB28 and func_8023F26C): human or computer.
s32 eightslots_extra_type(s32 slot) {
    return sExtraType[slot - FIRST_EXTRA_SLOT] == CONTROL_HUMAN ? CONTROL_HUMAN : CONTROL_COMPUTER;
}

// Controllers 5 to 8: the game polls 4 (func_8023A104), the rest come from the frontend's ports 5 to 8 into the
// moved pad array (gen_patches.py CONT_PADS), where the game's own loop takes all 8 into its rings.
#define CONT_PADS 0x80707800
#define PAD_SIZE 6
static s32 sExtraPorts; // a bit per port 5 to 8 that is on

// g_initRandom's seed (gen_patches.py HOOKS): the clock's at boot, in netplay the session's.
s32 eightslots_rng_seed(s32 seed) {
    return recomp_netplay_rng_seed(seed);
}

void level_frame(void);
void level_after_battle_setup(void);

// The last choice of players 5 to 8 (include/level_layout.h LH_EXTRA), once the frontend wrote it.
#define LEVEL_HEADER ((volatile u32*)0x80710000)
#define LH_EXTRA 6
#define LH_EXTRA_TYPES 7
#define EXTRA_MAGIC 0x58385053u
static s32 sExtraLoaded;

static void load_extra_players(void) {
    if (sExtraLoaded || LEVEL_HEADER[LH_EXTRA] != EXTRA_MAGIC) {
        return;
    }
    sExtraLoaded = 1;
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        sExtraType[i] = (s32)LEVEL_HEADER[LH_EXTRA_TYPES + i];
        EXTRA_DIFFICULTIES[i] = (s32)LEVEL_HEADER[LH_EXTRA_TYPES + 4 + i];
        sExtraTeam[i] = (s32)LEVEL_HEADER[LH_EXTRA_TYPES + 8 + i];
    }
}

// Players 5 to 8 changed in the menu: the frontend keeps them for the next start.
static void save_extra_players(void) {
    static s32 values[12];
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        values[i] = sExtraType[i];
        values[4 + i] = EXTRA_DIFFICULTIES[i];
        values[8 + i] = sExtraTeam[i];
    }
    recomp_save_extra_players(values);
}
s32 level_extra_start(s32 slot, f32* x, f32* y, f32* z);
Gfx* level_draw(Gfx* gfx);

void eightslots_read_extra_pads(void) {
    sExtraPorts = recomp_get_extra_pads((void*)(CONT_PADS + FIRST_EXTRA_SLOT * PAD_SIZE));
    // Netplay (src/game/netplay.cpp) replaces all 8 pads with the session's and says which slots have a player.
    s32 present = recomp_netplay_sync((void*)CONT_PADS);
    if (present >= 0) {
        sExtraPorts = present >> FIRST_EXTRA_SLOT;
    }
    level_frame(); // custom levels and the level editor (patches/level.c)
    load_extra_players();
}

// Before the room's players spawn: the spawn loop takes every slot with a control type.
// The test mod sets a bit per slot 4 to 7 to have it played by a human without the menu.
#define EXTRA_HUMANS (*(volatile s32*)0x80707F24)

void eightslots_before_spawn(void) {
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        if (EXTRA_HUMANS >> i & 1) {
            sExtraType[i] = CONTROL_HUMAN;
        }
        g_Players[FIRST_EXTRA_SLOT + i].controlType = extra_slots_wanted() ? sExtraType[i] : CONTROL_NONE;
    }
}

// Start positions of slots 4 to 7: the free tile of the arena farthest from everyone placed so far.
// The stage grid: tiles of 100 x 100 on levels whose heights add up (func_8026E2B0).
u32 func_8026D7D8(s32 tileX, s32 level, s32 tileZ); // a tile: kind (low 4 bits) and flags; 0x2010 off the grid
f32 func_8026E2B0(s32 level);                         // a level's height
s32 func_80251AC8(u32 tile);                          // whether the computer players cannot walk on a tile (blocks too)
#define TILE_KIND 0xF
#define TILE_FLOOR 1
#define TILE_RAMP_FIRST 3
#define TILE_RAMP_LAST 6
#define LEVELS 8
#define GRID_BYTES ((volatile u8*)0x800B15D8)
#define GRID_CHUNKS_X (*(volatile s32*)0x800B15DC)
#define GRID_CHUNKS_Z (*(volatile s32*)0x800B15E0)
#define SIDE 64

static f32 sMiddleX, sMiddleZ;
static s32 sWidth, sDepth, sLayers;

// The middle of the players already standing; ties go to the tile nearer to it.
static void find_middle(void) {
    s32 count = 0;
    sMiddleX = sMiddleZ = 0.0f;
    for (s32 slot = 0; slot < FIRST_EXTRA_SLOT; slot++) {
        if (g_Players[slot].actor != NULL) {
            sMiddleX += g_Players[slot].actor->model->x;
            sMiddleZ += g_Players[slot].actor->model->z;
            count++;
        }
    }
    if (count > 0) {
        sMiddleX /= count;
        sMiddleZ /= count;
    }
}

// The level a height stands on: the highest whose height is not above it.
static s32 level_at(f32 y) {
    s32 level = 0;
    for (s32 l = 1; l < LEVELS; l++) {
        if (func_8026E2B0(l) <= y + 1.0f) {
            level = l;
        }
    }
    return level;
}

static s32 walkable_kind(u32 tile) {
    u32 kind = tile & TILE_KIND;
    return kind == TILE_FLOOR || (kind >= TILE_RAMP_FIRST && kind <= TILE_RAMP_LAST);
}

static u64 sArena[LEVELS][SIDE]; // [level][z], bit x: a floor or ramp tile the players can get to

static s32 in_arena(s32 x, s32 level, s32 z) {
    return sArena[level][z] >> x & 1;
}

static s32 walkable(s32 x, s32 level, s32 z) {
    return x >= 0 && x < sWidth && z >= 0 && z < sDepth && walkable_kind(func_8026D7D8(x, level, z));
}

// From a reached tile one step (dx, dz): the same level, up a ramp that rises that way, or down to the highest floor
// below when the same level has none.
static s32 step_to(s32 x, s32 level, s32 z, s32 dx, s32 dz) {
    static const s8 rises[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } }; // ramp kinds 3 to 6
    s32 nx = x + dx, nz = z + dz;
    u32 kind = func_8026D7D8(x, level, z) & TILE_KIND;
    if (kind >= TILE_RAMP_FIRST && kind <= TILE_RAMP_LAST && rises[kind - TILE_RAMP_FIRST][0] == dx &&
        rises[kind - TILE_RAMP_FIRST][1] == dz && level + 1 < sLayers && walkable(nx, level + 1, nz)) {
        return level + 1;
    }
    for (s32 l = level; l >= 0; l--) {
        if (walkable(nx, l, nz)) {
            return l;
        }
        if (nx < 0 || nx >= sWidth || nz < 0 || nz >= sDepth || (func_8026D7D8(nx, l, nz) & TILE_KIND) != 0) {
            return -1; // a wall (or anything but air) in the way
        }
    }
    return -1;
}

// The arena: the tiles a player of slots 0 to 3 can walk to. Floor on top of the stage's walls stays out.
static void find_arena(void) {
    sLayers = GRID_BYTES[0] > LEVELS ? LEVELS : GRID_BYTES[0];
    sWidth = GRID_CHUNKS_X * 8 > SIDE ? SIDE : GRID_CHUNKS_X * 8;
    sDepth = GRID_CHUNKS_Z * 8 > SIDE ? SIDE : GRID_CHUNKS_Z * 8;
    for (s32 l = 0; l < LEVELS; l++) {
        for (s32 z = 0; z < SIDE; z++) {
            sArena[l][z] = 0;
        }
    }
    for (s32 slot = 0; slot < FIRST_EXTRA_SLOT; slot++) {
        if (g_Players[slot].actor == NULL) {
            continue;
        }
        SlotModel* m = g_Players[slot].actor->model;
        s32 x = (s32)(m->x / TILE_SIZE), z = (s32)(m->z / TILE_SIZE), l = level_at(m->y);
        if (x >= 0 && x < sWidth && z >= 0 && z < sDepth && l < sLayers) {
            sArena[l][z] |= 1ull << x;
        }
    }
    static const s8 sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (s32 changed = 1; changed;) { // sweeps until nothing new: a few dozen at most, once per battle
        changed = 0;
        for (s32 l = 0; l < sLayers; l++) {
            for (s32 z = 0; z < sDepth; z++) {
                for (s32 x = 0; x < sWidth; x++) {
                    if (!in_arena(x, l, z)) {
                        continue;
                    }
                    for (s32 i = 0; i < 4; i++) {
                        s32 to = step_to(x, l, z, sides[i][0], sides[i][1]);
                        if (to >= 0 && !in_arena(x + sides[i][0], to, z + sides[i][1])) {
                            sArena[to][z + sides[i][1]] |= 1ull << (x + sides[i][0]);
                            changed = 1;
                        }
                    }
                }
            }
        }
    }
}

// A tile a bomber can stand on now: floor or a ramp, no block, nothing that hurts.
static s32 tile_clear(s32 x, s32 level, s32 z) {
    if (x < 0 || x >= sWidth || z < 0 || z >= sDepth) {
        return 0;
    }
    u32 tile = func_8026D7D8(x, level, z);
    return walkable_kind(tile) && !func_80251AC8(tile);
}

// Room to drop a bomb and get out of its way: two clear tiles in a row next to it, or two clear sides.
static s32 has_room(s32 x, s32 level, s32 z) {
    static const s8 sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    s32 open = 0;
    for (s32 i = 0; i < 4; i++) {
        if (tile_clear(x + sides[i][0], level, z + sides[i][1])) {
            if (++open >= 2 || tile_clear(x + 2 * sides[i][0], level, z + 2 * sides[i][1])) {
                return 1;
            }
        }
    }
    return 0;
}

// The arena's clear floor tile with room around it whose nearest bomber is farthest away.
// Returns 0 when there is none.
static s32 place_spread(SlotModel* to, s32 self) {
    f32 best = -1.0f, bestMiddle = 0.0f;
    for (s32 level = 0; level < sLayers; level++) {
        for (s32 z = 0; z < sDepth; z++) {
            for (s32 x = 0; x < sWidth; x++) {
                if (!in_arena(x, level, z) || (func_8026D7D8(x, level, z) & TILE_KIND) != TILE_FLOOR ||
                    !tile_clear(x, level, z) || !has_room(x, level, z)) {
                    continue;
                }
                f32 px = x * TILE_SIZE, pz = z * TILE_SIZE;
                f32 nearest = 1e30f;
                for (s32 slot = 0; slot < self; slot++) {
                    SlotActor* other = g_Players[slot].actor;
                    if (other == NULL || (slot >= FIRST_EXTRA_SLOT && !sPlaying[slot - FIRST_EXTRA_SLOT])) {
                        continue;
                    }
                    f32 dx = other->model->x - px, dz = other->model->z - pz;
                    if (dx * dx + dz * dz < nearest) {
                        nearest = dx * dx + dz * dz;
                    }
                }
                f32 mx = sMiddleX - px, mz = sMiddleZ - pz;
                f32 middle = mx * mx + mz * mz;
                if (nearest > best || (nearest == best && middle < bestMiddle)) {
                    best = nearest;
                    bestMiddle = middle;
                    to->x = px;
                    to->y = func_8026E2B0(level);
                    to->z = pz;
                }
            }
        }
    }
    return best >= 0.0f;
}

// After func_8023CA94 set up slots 0 to 3 for the battle.
// func_8023CA94 also ends here outside a battle (to the results screen); sPlaying stays for that one.
void eightslots_after_battle_setup(void) {
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        sIconSheets[i] = extra_slots_wanted() ? func_8026CE28(RECOLORED_ICONS + i) : NULL;
        if (GAME_FLAGS & GAME_FLAG_BATTLE) {
            sPlaying[i] = 0;
        }
    }
    level_after_battle_setup(); // a custom level's start gear, before slots 4 to 7 copy it
    if (!extra_slots_wanted()) {
        return;
    }
    find_middle();
    find_arena();
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        SlotPlayer* bot = &g_Players[FIRST_EXTRA_SLOT + i];
        // Next to player i, or the first player there is when slot i is empty.
        s32 n = i;
        for (s32 j = 0; j < EXTRA_SLOTS && g_Players[n].actor == NULL; j++) {
            n = j;
        }
        SlotPlayer* near = &g_Players[n];
        if (bot->controlType == CONTROL_NONE || bot->actor == NULL || near->actor == NULL) {
            bot->controlType = CONTROL_NONE;
            continue;
        }
        bot->hearts = near->hearts;
        bot->bombType = near->bombType;
        bot->bombCount = near->bombCount;
        bot->firePower = near->firePower;
        bot->bombTypeStart = near->bombTypeStart;
        bot->firePowerStart = near->firePowerStart;

        SlotModel* to = bot->actor->model;
        if (!level_extra_start(FIRST_EXTRA_SLOT + i, &to->x, &to->y, &to->z) &&
            !place_spread(to, FIRST_EXTRA_SLOT + i)) {
            to->x = near->actor->model->x; // no room anywhere: on the player's own tile
            to->y = near->actor->model->y;
            to->z = near->actor->model->z;
        }
        bot->spawn[0] = to->x; // a ghost comes back here
        bot->spawn[1] = to->y;
        bot->spawn[2] = to->z;

        if (bot->controlType == CONTROL_COMPUTER) {
            s32 difficulty = AI_DIFFICULTY;
            func_80251804(FIRST_EXTRA_SLOT + i);
            AI_DIFFICULTY = difficulty; // the others keep the difficulty chosen in the menu
        }
        ALIVE_COUNT++;
        sPlaying[i] = 1;
    }
}

// Before a computer player's AI step (func_80252D14, gen_patches.py HOOKS): the game keeps one level for all of
// them, set by whichever started last, so each sets its own menu level here.
void eightslots_ai_level(void* ai) {
    s32 slot = *(s32*)((u8*)ai + 0xC);
    s32 menu = slot < FIRST_EXTRA_SLOT ? BATTLE_DIFFICULTIES[slot] : EXTRA_DIFFICULTIES[slot - FIRST_EXTRA_SLOT];
    if (slot >= 0 && slot < 8 && menu >= 0 && menu <= 2) {
        AI_DIFFICULTY = AI_LEVELS[menu];
        ((volatile s32*)0x80707F60)[slot] = AI_DIFFICULTY; // the level each one plays at, for the test mod
    }
}

// After the battle HUD (func_80241BB0) drew slots 0 to 3: the faces of slots 4 to 7, under theirs.
void eightslots_draw_hud(Gfx** gfxp) {
    Gfx* gfx = *gfxp;
    gDPPipeSync(gfx++);
    gDPSetTextureLUT(gfx++, G_TT_RGBA16);
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        SlotPlayer* player = &g_Players[FIRST_EXTRA_SLOT + i];
        u8* sheet = sIconSheets[i];
        if (sheet == NULL || player->controlType == CONTROL_NONE) {
            continue;
        }
        s32 out = player->controlType == CONTROL_OUT || (*(s32*)((u8*)player + STATES_OFFSET) & GHOST_STATE);
        u32* tile = sheet_entry(sheet, out ? ICON_OUT_TILE : ICON_ALIVE_TILE);
        u8* texture = sheet + tile[2];
        u8* palette = sheet + tile[5];
        volatile u32* place = HUD_ELEMENTS + (1 + i) * HUD_ELEMENT_WORDS;

        gDPLoadTLUT_pal256(gfx++, palette);
        // The icons are CI4, unlike the CI8 model textures.
        gDPLoadTextureBlock_4b(gfx++, texture, G_IM_FMT_CI, 16, 16, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSPTextureRectangle(gfx++, place[1], place[2] + HUD_SECOND_ROW, place[3], place[4] + HUD_SECOND_ROW,
                            G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    }

    u8* digits = HUD_DIGITS;
    if (digits != NULL) {
        gDPPipeSync(gfx++);
        gDPLoadTLUT_pal16(gfx++, 0, digits + sheet_entry(digits, DIGIT_PALETTE_TILE)[2]);
        for (s32 i = 0; i < EXTRA_SLOTS; i++) {
            if (sIconSheets[i] == NULL || g_Players[FIRST_EXTRA_SLOT + i].controlType == CONTROL_NONE) {
                continue;
            }
            u8* texture = digits + sheet_entry(digits, MIN(WINS[FIRST_EXTRA_SLOT + i], MAX_DIGIT) * 2)[2];
            s32 x = HUD_GROUP_X[FIRST_WINS_GROUP + i] << 2;
            // Loaded by tile: a 10 texel CI4 row is 5 bytes, not a whole TMEM line.
            gDPLoadTextureTile_4b(gfx++, texture, G_IM_FMT_CI, DIGIT_WIDTH, DIGIT_HEIGHT, 0, 0, DIGIT_WIDTH - 1,
                                  DIGIT_HEIGHT - 1, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
                                  G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPTextureRectangle(gfx++, x, DIGIT_TOP + HUD_SECOND_ROW, x + (DIGIT_WIDTH << 2),
                                DIGIT_TOP + (DIGIT_HEIGHT << 2) + HUD_SECOND_ROW, G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
        }
    }
    gDPPipeSync(gfx++);
    gDPSetTextureLUT(gfx++, G_TT_NONE);
    gfx = level_draw(gfx); // the level editor's markers (patches/level.c)
    *gfxp = gfx;
}

// Where the game clears the wins of slots 0 to 3: a new match, and the battle menu.
void eightslots_reset_wins(void) {
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        WINS[FIRST_EXTRA_SLOT + i] = 0;
    }
}

// The round results screen (func_ovl3_7_8004400C) lists the slots that played; slots 4 to 7 follow 0 to 3.
// gen_patches.py moved the list to RESULTS_LIST with room for 8.
#define RESULTS_LIST ((volatile s32*)0x80707CD0)
#define RESULTS_COUNT (*(volatile s32*)0x80046F1C)

void eightslots_results_participants(void) {
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        if (sPlaying[i]) {
            RESULTS_LIST[RESULTS_COUNT++] = FIRST_EXTRA_SLOT + i;
        }
    }
}

// The round results stack the bombers and their medals in one column, 720 / count apart, sized for 4:
// more get smaller.
#define RESULTS_ROOM 4

// Medals hang this far above their bomber's row (170 against 90), at the middle of a full size bomber.
#define MEDAL_LIFT 80.0f

typedef struct ResultsModel {
    u8 pad[0x14];
    f32 y;
    u8 pad18[0x28 - 0x18];
    f32 scale[3];
} ResultsModel;

void eightslots_results_scale(ResultsModel* model) {
    s32 count = RESULTS_COUNT;
    if (count <= RESULTS_ROOM || model == NULL) {
        return;
    }
    for (s32 i = 0; i < 3; i++) {
        model->scale[i] *= (f32)RESULTS_ROOM / count;
    }
}

// A medal of an earlier win: smaller, and as far above its smaller bomber.
void eightslots_results_medal(ResultsModel* model) {
    s32 count = RESULTS_COUNT;
    if (count <= RESULTS_ROOM || model == NULL) {
        return;
    }
    eightslots_results_scale(model);
    model->y -= MEDAL_LIFT * (1.0f - (f32)RESULTS_ROOM / count);
}

// Player Select (battle menu, overlay ovl3_3) has 4 columns. Page 2 shows players 5 to 8 in them: the menu
// edits the config block through MENU_BLOCK, which then points at a copy holding players 5 to 8 in the
// slots of 1 to 4. The columns' bombers get the recolored models.
#define MENU_BLOCK (*(u8* volatile*)0x8004A324)
#define MENU_COLUMN (*(volatile s32*)0x8004A328)
#define MENU_COLUMNS 4
#define MENU_MODEL(column) (*(u8* volatile*)(0x80048EAC + (column) * 0x14))
#define MENU_MODEL_IDLE 0x40 // flag of the bombers in the columns not chosen
#define MENU_MODEL_PARTS 12
#define MENU_SOUND_MOVE 0
#define BLOCK_SIZE 0x110
#define BLOCK_TYPES 0x4
#define BLOCK_DIFFICULTIES 0x14
#define BLOCK_CUSTOM 0x40
#define BLOCK_TEAMS 0x90
#define TEAM_COLUMN (*(volatile s32*)0x8004A330) // Team Select's cursor
#define MENU_HIGHLIGHT_X (*(volatile s32*)0x8004A410) // the column highlight's x: column * 50
#define MENU_COLUMN_WIDTH 50

void g_AssignModelParts(void* model, s32 fileID, s32 part, s32 slot);
void g_PlayGlobalSound(s32 id);

static u32 sPageBlock[BLOCK_SIZE / 4];
static u8* sRealBlock;
static s32 sPage;
#define sExtraDifficulty EXTRA_DIFFICULTIES

static s32* block_word(u8* block, s32 offset, s32 column) {
    return (s32*)(block + offset + column * 4);
}

static void turn_page(s32 page, s32 column, volatile s32* cursor) {
    if (page) {
        sRealBlock = MENU_BLOCK;
        for (s32 i = 0; i < BLOCK_SIZE / 4; i++) {
            sPageBlock[i] = ((u32*)sRealBlock)[i];
        }
        for (s32 c = 0; c < MENU_COLUMNS; c++) {
            *block_word((u8*)sPageBlock, BLOCK_TYPES, c) = sExtraType[c];
            *block_word((u8*)sPageBlock, BLOCK_DIFFICULTIES, c) = sExtraDifficulty[c];
            *block_word((u8*)sPageBlock, BLOCK_CUSTOM, c) = 0;
            *block_word((u8*)sPageBlock, BLOCK_TEAMS, c) = sExtraTeam[c];
        }
        MENU_BLOCK = (u8*)sPageBlock;
    } else {
        for (s32 c = 0; c < MENU_COLUMNS; c++) {
            sExtraType[c] = *block_word((u8*)sPageBlock, BLOCK_TYPES, c);
            sExtraDifficulty[c] = *block_word((u8*)sPageBlock, BLOCK_DIFFICULTIES, c);
            sExtraTeam[c] = *block_word((u8*)sPageBlock, BLOCK_TEAMS, c);
        }
        MENU_BLOCK = sRealBlock;
        save_extra_players();
    }
    sPage = page;
    for (s32 c = 0; c < MENU_COLUMNS; c++) {
        s32 file = page ? RECOLORED_MODEL + c : WHITE_BOMBER_MODEL + c;
        for (s32 part = 0; part < MENU_MODEL_PARTS; part++) {
            g_AssignModelParts(MENU_MODEL(c), file, part, part);
        }
    }
    *(volatile u32*)(MENU_MODEL(*cursor) + 0xC) |= MENU_MODEL_IDLE;
    *cursor = column;
    *(volatile u32*)(MENU_MODEL(column) + 0xC) &= ~MENU_MODEL_IDLE;
    MENU_HIGHLIGHT_X = column * MENU_COLUMN_WIDTH;
    g_PlayGlobalSound(MENU_SOUND_MOVE);
}

// Cursor right on column 4 of page 1, or left on column 1 of page 2. Returns 1 when the page turned, so that
// the hook redoes the custom bombers.
s32 eightslots_menu_right(void) {
    if (sPage || MENU_COLUMN != MENU_COLUMNS - 1) {
        return 0;
    }
    turn_page(1, 0, &MENU_COLUMN);
    return 1;
}

s32 eightslots_menu_left(void) {
    if (!sPage || MENU_COLUMN != 0) {
        return 0;
    }
    turn_page(0, MENU_COLUMNS - 1, &MENU_COLUMN);
    return 1;
}

// Start or B leave Player Select: the rest of the menu edits the real block.
s32 eightslots_menu_leave(void) {
    if (!sPage) {
        return 0;
    }
    turn_page(0, MENU_COLUMN, &MENU_COLUMN);
    return 1;
}

// Page 2 turns the type in one order, off, COM, MAN, whichever way the stick went: the game skips MAN by the
// controllers of ports 1 to 4, not those of the players shown. MAN only with that player's port on (5 to 8).
s32 eightslots_menu_type(s32 type) {
    s32 current = *block_word((u8*)sPageBlock, BLOCK_TYPES, MENU_COLUMN);
    if (!sPage || type == current) {
        return type;
    }
    if (current == CONTROL_NONE) {
        return CONTROL_COMPUTER;
    }
    if (current == CONTROL_COMPUTER && (sExtraPorts >> MENU_COLUMN & 1)) {
        return CONTROL_HUMAN;
    }
    return CONTROL_NONE;
}

// For the menu's "at least 2 players" check.
s32 eightslots_menu_extra_count(void) {
    s32 count = 0;
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        count += sExtraType[i] != CONTROL_NONE;
    }
    return count;
}

// Page arrow of Player Select: a blinking red triangle under bomber 4 pointing right (page 1, players 5 to 8
// that way), or under bomber 1 pointing left (page 2). Drawn with the column highlight
// (func_ovl3_3_80046918), which leaves the combiner on the primitive color; the frame and the glass tubes are
// drawn later, so it stays inside the green field, in the gap between the bombers and their texts.
// Screen coordinates of the 320 x 240 menu.
#define ARROW_RIGHT_TIP 262
#define ARROW_LEFT_TIP 58
#define ARROW_MIDDLE 151
#define ARROW_LENGTH 9
#define ARROW_BLINK 16 // frames

static s32 sMenuFrames;

// Team Select runs eightslots_team_screen() every frame; this counts down when it stops.
static s32 sTeamScreen;

// Team colors: blue, red, green, yellow.
static const u8 sTeamColors[TEAMS][3] = {
    { 0x30, 0x60, 0xFF },
    { 0xF0, 0x20, 0x20 },
    { 0x40, 0xD0, 0x40 },
    { 0xF0, 0xD8, 0x20 },
};
// Team Select shows teams 0 and 2 at the top, 1 and 3 at the bottom; a square right of each bomber's head shows
// its team's color (the texts and the VS bar are drawn later, over anything lower).
#define TEAM_MARK_X 100 // column 1's, then + MENU_COLUMN_WIDTH
#define TEAM_MARK_TOP 76
#define TEAM_MARK_BOTTOM 152
#define TEAM_MARK_SIZE 8

void eightslots_team_screen(void) {
    sTeamScreen = 2;
}

static Gfx* draw_team_marks(Gfx* gfx) {
    u8* block = MENU_BLOCK;
    for (s32 c = 0; c < MENU_COLUMNS; c++) {
        if (*block_word(block, BLOCK_TYPES, c) == CONTROL_NONE) {
            continue;
        }
        s32 team = *block_word(block, BLOCK_TEAMS, c) & (TEAMS - 1);
        s32 x = TEAM_MARK_X + c * MENU_COLUMN_WIDTH;
        s32 y = (team & 1) ? TEAM_MARK_BOTTOM : TEAM_MARK_TOP;
        gDPPipeSync(gfx++);
        gDPSetPrimColor(gfx++, 0, 0, sTeamColors[team][0], sTeamColors[team][1], sTeamColors[team][2], 0xFF);
        gDPFillRectangle(gfx++, x, y, x + TEAM_MARK_SIZE, y + TEAM_MARK_SIZE);
    }
    return gfx;
}

Gfx* eightslots_menu_draw(Gfx* gfx) {
    if (BATTLE_TYPE > BATTLE_TEAMS || EXTRA_SLOTS_OFF) {
        return gfx;
    }
    if (sTeamScreen > 0) {
        sTeamScreen--;
        gfx = draw_team_marks(gfx);
    }
    if (++sMenuFrames & ARROW_BLINK) {
        return gfx;
    }
    gDPPipeSync(gfx++);
    gDPSetPrimColor(gfx++, 0, 0, 0xF0, 0x20, 0x20, 0xFF);
    for (s32 i = 0; i < ARROW_LENGTH; i++) {
        s32 x = sPage ? ARROW_LEFT_TIP + i : ARROW_RIGHT_TIP - i;
        gDPFillRectangle(gfx++, x, ARROW_MIDDLE - i, x + 1, ARROW_MIDDLE + i + 1);
    }
    return gfx;
}

// Team Select (same menu, cursor TEAM_COLUMN): page 2, four teams.
static s32 column_present(s32 column) {
    return *block_word(MENU_BLOCK, BLOCK_TYPES, column) != CONTROL_NONE;
}

// Right with no player further right on page 1: page 2 at its first player. Returns 1 when the page turned.
s32 eightslots_team_right(void) {
    if (sPage || EXTRA_SLOTS_OFF) {
        return 0;
    }
    for (s32 c = TEAM_COLUMN + 1; c < MENU_COLUMNS; c++) {
        if (column_present(c)) {
            return 0;
        }
    }
    for (s32 c = 0; c < MENU_COLUMNS; c++) {
        if (sExtraType[c] != CONTROL_NONE) {
            turn_page(1, c, &TEAM_COLUMN);
            return 1;
        }
    }
    return 0;
}

static s32 last_present_on_page_1(void) {
    for (s32 c = MENU_COLUMNS - 1; c > 0; c--) {
        if (*block_word(sRealBlock, BLOCK_TYPES, c) != CONTROL_NONE) {
            return c;
        }
    }
    return 0;
}

// Left with no player further left on page 2: back to page 1's last player.
s32 eightslots_team_left(void) {
    if (!sPage) {
        return 0;
    }
    for (s32 c = TEAM_COLUMN - 1; c >= 0; c--) {
        if (column_present(c)) {
            return 0;
        }
    }
    turn_page(0, last_present_on_page_1(), &TEAM_COLUMN);
    return 1;
}

// A or B leave Team Select.
s32 eightslots_team_leave(void) {
    if (!sPage) {
        return 0;
    }
    turn_page(0, last_present_on_page_1(), &TEAM_COLUMN);
    return 1;
}

// Up and down step through the 4 teams (the game only knew 0 and 1).
void eightslots_team_cycle(s32 step) {
    s32* team = block_word(MENU_BLOCK, BLOCK_TEAMS, TEAM_COLUMN);
    *team = (*team + step + TEAMS) % TEAMS;
    g_PlayGlobalSound(MENU_SOUND_MOVE);
}

// A starts the battle with at least 2 teams among all players chosen.
s32 eightslots_team_ok(void) {
    s32 seen = 0;
    for (s32 slot = 0; slot < FIRST_EXTRA_SLOT + EXTRA_SLOTS; slot++) {
        if (slot_chosen(slot)) {
            seen |= 1 << (team_of(slot) & (TEAMS - 1));
        }
    }
    return seen != 0 && (seen & (seen - 1)) != 0;
}

// Battle: per-team data moved to room for 4 (gen_patches.py: TEAM_MOVES).
#define TEAM_STANDING ((volatile s32*)0x80707D00) // the team's crystal still stands
#define TEAM_CRYSTAL ((volatile s32*)0x80707D10)  // its enemy record; the first word is the crystal's actor
#define TEAM_PROCESS 0x80242FB4                   // places a team's crystal and HUD gems (argument: team + 1)
#define PROCESS_STACK 0x130

s32 HuPrcCreate(u32 function, s32 arg, s32 a2, s32 a3, s32 stack); // starts a process (func_80238A10)

static s32 team_in_play(s32 team) {
    for (s32 slot = 0; slot < FIRST_EXTRA_SLOT + EXTRA_SLOTS; slot++) {
        if (slot_chosen(slot) && team_of(slot) == team) {
            return 1;
        }
    }
    return 0;
}

// After func_8023CA94 set up teams 0 and 1 (in play and their crystal processes).
void eightslots_team_setup(void) {
    if (BATTLE_TYPE != BATTLE_TEAMS || !(GAME_FLAGS & GAME_FLAG_BATTLE)) {
        return;
    }
    for (s32 team = 0; team < TEAMS; team++) {
        TEAM_STANDING[team] = team_in_play(team);
        if (team >= FIRST_NEW_TEAM) {
            TEAM_CRYSTAL[team] = 0;
            if (TEAM_STANDING[team]) {
                HuPrcCreate(TEAM_PROCESS, team + 1, 0, 0, PROCESS_STACK);
            }
        }
    }
}

s32 eightslots_team_has_member(s32 team) {
    return team_in_play(team);
}

s32 eightslots_teams_left(void) {
    s32 left = 0;
    for (s32 team = 0; team < TEAMS; team++) {
        left += TEAM_STANDING[team] != 0;
    }
    return left;
}

// The round's winning team, for result code 0x16 + team (teams 2 and 3: 0x1D and 0x1E).
s32 eightslots_winning_team(void) {
    for (s32 team = 0; team < TEAMS; team++) {
        if (TEAM_STANDING[team]) {
            return team;
        }
    }
    return 0;
}

// A computer player sees an actor: the enemy team whose standing crystal it is, else any other team.
s32 eightslots_ai_enemy_team(s32 slot, u32 actor) {
    s32 own = team_of(slot);
    s32 other = own == 0 ? 1 : 0;
    for (s32 team = 0; team < TEAMS; team++) {
        if (team == own) {
            continue;
        }
        u32 crystal = TEAM_CRYSTAL[team];
        if (TEAM_STANDING[team] && crystal != 0 && *(u32*)crystal == actor) {
            return team;
        }
    }
    return other;
}

s32 eightslots_same_team(s32 a, s32 b) {
    return team_of(a) == team_of(b);
}

// Team results (func_ovl3_7_8004400C): everyone who played, by team. The screen leaves a gap after the first team
// (its count is returned); with up to 4 teams there is none: the count of all.
s32 eightslots_results_teams(void) {
    RESULTS_COUNT = 0;
    for (s32 team = 0; team < TEAMS; team++) {
        for (s32 slot = 0; slot < FIRST_EXTRA_SLOT + EXTRA_SLOTS; slot++) {
            s32 played = slot < FIRST_EXTRA_SLOT ? BLOCK_TYPE(slot) != CONTROL_NONE : sPlaying[slot - FIRST_EXTRA_SLOT];
            if (!played || team_of(slot) != team) {
                continue;
            }
            RESULTS_LIST[RESULTS_COUNT++] = slot;
        }
    }
    return RESULTS_COUNT;
}

s32 eightslots_results_team_row(s32 team);

// A medal of a team's earlier win (func_ovl3_7_8004400C, team results): beside the team's first member, smaller
// when more than 4 bombers stand there. Same rows as the medal flying in (gen_patches.py, 0x80044960).
#define RESULTS_TOP 90.0f
#define RESULTS_COLUMN 720.0f
#define RESULTS_TEAM_SPACING 640.0f

void eightslots_team_medal(ResultsModel* model, s32 team) {
    s32 count = RESULTS_COUNT;
    if (count <= 0 || model == NULL) {
        return;
    }
    f32 scale = count > RESULTS_ROOM ? (f32)RESULTS_ROOM / count : 1.0f;
    s32 row = eightslots_results_team_row(team);
    eightslots_results_scale(model);
    model->y = RESULTS_TOP + MEDAL_LIFT * scale - RESULTS_COLUMN / 2 / count - row * RESULTS_TEAM_SPACING / count;
}

// The row of a team's first member on the team results screen, where its medal lands.
s32 eightslots_results_team_row(s32 team) {
    for (s32 row = 0; row < RESULTS_COUNT; row++) {
        if (team_of(RESULTS_LIST[row]) == team) {
            return row;
        }
    }
    return 0;
}
