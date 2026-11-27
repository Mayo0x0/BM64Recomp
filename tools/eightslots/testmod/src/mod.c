// Development harness: skips the menus into a seeded battle of computer players and logs its state,
// so that two builds can be compared by their logs. Addresses below are the unrelocated ones the
// config block keeps; PLAYERS is passed at build time because the relocation moves it.
#include "modding.h"
#include "recomputils.h"

#ifndef PLAYERS_ADDRESS
#define PLAYERS_ADDRESS 0x800AEDF0
#endif
#ifndef TEST_STAGE
#define TEST_STAGE 3
#endif
#ifndef TEST_SLOTS
#define TEST_SLOTS 4
#endif
#ifndef TEST_MINUTES
#define TEST_MINUTES 1
#endif
#ifndef TEST_BOTS
#define TEST_BOTS 4 // extra computer players in the slots after the first four
#endif

#define BATTLE ((volatile int*)0x802AC660)
#define GAME_FLAGS (*(volatile int*)0x802AC5D0)
#define ALIVE_COUNT (*(volatile int*)0x802AC758)
#define AI_DIFFICULTY (*(volatile int*)0x802ACFC4)
#define RANDOM_INDEX (*(volatile int*)0x802ACFE0)
#define RANDOM_STATE ((volatile int*)0x802ACFE8)
#define RANDOM_WORDS 55

#ifndef START_FRAME
#define START_FRAME 180
#endif
#ifndef LOG_EVERY
#define LOG_EVERY 60
#endif
#define BATTLE_OVERLAY 0x90
#define START_ENTRY 0x10
#define LEFTOVER_INTRO_FLAGS 0x90 // the attract sequence sets these, and 0x80 stops the battle timer

typedef struct ActorModel {
    char pad[0x10];
    float x, y, z;
} ActorModel;

typedef struct Actor {
    char pad[0x40];
    ActorModel* model;
} Actor;

typedef struct Player {
    float spawn_x, spawn_y, spawn_z;
    char pad0[0x14 - 0xC];
    int hearts;
    int bomb_type;
    int bomb_count;
    int fire_power;
    int control_type;
    Actor* actor;
    char pad2C[0x68 - 0x2C];
    int states;
    char pad6C[0x94 - 0x6C];
    int bomb_type_start;
    int fire_power_start;
    char pad9C[0xC8 - 0x9C];
} Player;

#define PLAYERS ((Player*)PLAYERS_ADDRESS)

void func_8023DA04(int file_loaded);
void func_8023AE6C(int overlay, int entry, int arg);
void func_80251804(int slot); // starts a slot's CPU process

static int frames;
static int logged_tiles;
static int started;

static void seed_random(void) {
    unsigned int value = 0x12345678;
    for (int i = 0; i < RANDOM_WORDS; i++) {
        value = value * 1103515245u + 12345u;
        RANDOM_STATE[i] = (int)(value >> 2);
    }
    RANDOM_INDEX = 0;
}

static void start_battle(void) {
    *(volatile int*)0x80707F20 = TEST_BOTS == 0; // patches/eightslots.c: EXTRA_SLOTS_OFF
    for (int i = 0; i < 0x40; i++) {
        BATTLE[i] = 0;
    }
    for (int slot = 0; slot < 4; slot++) {
        BATTLE[1 + slot] = slot < TEST_SLOTS ? 2 : 0; // computer player
        BATTLE[5 + slot] = 2;                         // difficulty
    }
#ifdef TEST_LEVELS // menu level (0 to 2) of each slot, a hex digit per slot from slot 0 up (patches/eightslots.c)
    for (int slot = 0; slot < 8; slot++) {
        int level = (TEST_LEVELS >> (slot * 4)) & 0xF;
        if (slot < 4) {
            BATTLE[5 + slot] = level;
        } else {
            ((volatile int*)0x80707F40)[slot - 4] = level; // EXTRA_DIFFICULTIES
        }
    }
#endif
    BATTLE[0x28 / 4] = TEST_MINUTES; // 1 means Sudden Death at once, with no ghosts
    BATTLE[0x38 / 4] = 1;   // Sudden Death
    BATTLE[0x3C / 4] = 1;   // ghosts
    BATTLE[0xA0 / 4] = TEST_STAGE;
#ifdef TEST_HUMANS // a bit per slot 4 to 7 played by a human: the frontend's ports 5 to 8 (patches/eightslots.c)
    *(volatile int*)0x80707F24 = TEST_HUMANS;
#endif
#ifdef TEST_TEAMS // a team battle: slot n in team n (4 teams); slots 4 to 7 keep their default teams
    BATTLE[0] = 1;
    for (int slot = 0; slot < 4; slot++) {
        BATTLE[0x90 / 4 + slot] = slot;
    }
#endif
#ifdef PRESET_TEAM_WINS // earlier wins of the 4 teams, for the medals on the team results screen
    BATTLE[0xA4 / 4 + 0] = 1;
    BATTLE[0xA4 / 4 + 1] = 2;
    BATTLE[0xA4 / 4 + 2] = 1;
    BATTLE[0xA4 / 4 + 3] = 2;
    BATTLE[0x24 / 4] = 5; // wins that end the match
#endif
#ifdef TEST_WINS_NEEDED
    BATTLE[0x24 / 4] = TEST_WINS_NEEDED; // wins that end the match
#endif
#ifdef PRESET_EXTRA_WINS // shows the HUD digits of slots 4 to 7: 1, 2, 3, 4
    for (int i = 0; i < 4; i++) {
        BATTLE[0xB4 / 4 + i] = i + 1;
    }
#endif
    recomp_printf("TEST start stage=%d slots=%d\n", TEST_STAGE, TEST_SLOTS);
    func_8023DA04(0);
    GAME_FLAGS &= ~LEFTOVER_INTRO_FLAGS;
    seed_random();
    func_8023AE6C(BATTLE_OVERLAY + TEST_STAGE, START_ENTRY, 0);
}

static void log_state(void) {
    recomp_printf("TEST f=%d flags=%X alive=%d t=%d:%d diff=%d", frames, GAME_FLAGS, ALIVE_COUNT, BATTLE[0x2C / 4], BATTLE[0x30 / 4], AI_DIFFICULTY); recomp_printf(" spd=%d,%d,%d", (int)(*(volatile float*)0x802ACFB0), (int)(*(volatile float*)0x802ACFB4), (int)(*(volatile float*)0x802ACFB8));
    for (int slot = 0; slot < 8; slot++) {
        Player* p = &PLAYERS[slot];
        if (slot >= 5 && PLAYERS_ADDRESS == 0x800AEDF0) {
            break; // only 5 entries exist before the relocation
        }
        if (p->control_type == 0) {
            continue;
        }
        if (p->actor != 0) {
            ActorModel* m = p->actor->model;
            recomp_printf(" | %d c%d h%d b%d f%d s%X %d,%d,%d", slot, p->control_type, p->hearts, p->bomb_count, p->fire_power,
                          p->states, (int)(m->x * 10.0f), (int)(m->y * 10.0f), (int)(m->z * 10.0f));
        } else {
            recomp_printf(" | %d c%d none", slot, p->control_type);
        }
    }
    recomp_printf(" | wins"); // by slot, + 0xB4 on for slots 4 to 7
    for (int slot = 0; slot < 8; slot++) {
        recomp_printf(" %d", BATTLE[0xA4 / 4 + slot]);
    }
    recomp_printf(" code=%X", *(volatile int*)0x802AC5D8);
    recomp_printf(" teams=%d%d%d%d", ((volatile int*)0x80707D00)[0], ((volatile int*)0x80707D00)[1],
                  ((volatile int*)0x80707D00)[2], ((volatile int*)0x80707D00)[3]);
    recomp_printf("\n");
#ifdef TEST_HUMANS
    // Port 5's pad as read (gen_patches.py CONT_PADS + 4 * 6) and its ring position (CONT_RING_AT + 4 * 4).
    recomp_printf("TEST pad5 f=%d %08X %04X ring=%d paused=%d\n", frames, *(volatile unsigned int*)0x80707818,
                  *(volatile unsigned short*)0x8070781C, *(volatile int*)0x80707B50, *(volatile int*)0x802A0B80);
    // Every pad's buttons and stick x (gamepad_test.py holds each pad's stick at its own x).
    recomp_printf("TEST pads f=%d", frames);
    for (int i = 0; i < 8; i++) {
        volatile unsigned char* pad = (volatile unsigned char*)(0x80707800 + i * 6);
        recomp_printf(" %04X/%d", *(volatile unsigned short*)pad, (signed char)pad[2]);
    }
    recomp_printf("\n");
#endif
#ifdef AI_STATE_ADDRESS
    // Computer players: state (+0x4) and target slot (+0x10) of each slot's AI record.
    // The level each computer player last played at (patches/eightslots.c: eightslots_ai_level).
    volatile int* levels = (volatile int*)0x80707F60;
    recomp_printf("TEST ai f=%d levels %d%d%d%d%d%d%d%d", frames, levels[0], levels[1], levels[2], levels[3], levels[4],
                  levels[5], levels[6], levels[7]);
    for (int slot = 0; slot < 8; slot++) {
        volatile int* ai = (volatile int*)(AI_STATE_ADDRESS + slot * 0x128);
        volatile float* own = (volatile float*)ai[0x18 / 4]; // the CPU's actor: it waits while + 0x6C is not 0
        // The level func_80252D14 copied into the first entry of the movement macro (+0x98, entries at + 0x24).
        volatile int* macro = (volatile int*)ai[0x98 / 4];
        volatile int* entries = macro != 0 ? (volatile int*)macro[9] : 0;
        recomp_printf(" | %d s%d t%d m%d f%X w%d v%d L%d", slot, ai[1], ai[4], ai[5], ai[0], ai[0x88 / 4],
                      own != 0 ? (int)(own[0x6C / 4] * 100.0f) : -999, entries != 0 ? entries[0] : -1);
    }
    recomp_printf("\n");
#ifdef DUMP_AI_SLOT
    volatile int* rec = (volatile int*)(AI_STATE_ADDRESS + DUMP_AI_SLOT * 0x128);
    recomp_printf("TEST rec f=%d", frames);
    for (int w = 0; w < 0x128 / 4; w++) {
        recomp_printf(" %X", rec[w]);
    }
    recomp_printf("\n");
    // The movement macro (+0x98): counts, positions and timers of its 3 tracks, then each track's entries.
    volatile int* macro = (volatile int*)rec[0x98 / 4];
    if (macro != 0) {
        recomp_printf("TEST macro f=%d n %d %d %d i %d %d %d t %d %d %d", frames, macro[0], macro[1], macro[2],
                      macro[3], macro[4], macro[5], macro[6], macro[7], macro[8]);
        volatile int* first = (volatile int*)macro[9];
        if (first != 0 && macro[0] > 0 && macro[0] < 64) {
            recomp_printf(" | t0");
            for (int e = 0; e < macro[0]; e++) {
                recomp_printf(" %d", first[e * 4]);
            }
        }
        recomp_printf("\n");
    }
#endif
#endif
}

// The game sets up slots 4 to 7 itself now (patches/eightslots.c). Hooks here must stay off the functions
// that carry toml hooks: a mod hook rebuilds the function without them.

#ifdef DUMP_PLAYER_MODELS
static void dump_player_models(void);
#endif

unsigned int func_8026D7D8(int x, int level, int z); // the stage tile at a grid place
float func_8026E2B0(int level);                         // a level's height

static int level_at(float y) {
    int level = 0;
    for (int l = 1; l < 8; l++) {
        if (func_8026E2B0(l) <= y + 1.0f) {
            level = l;
        }
    }
    return level;
}

// Right after the start: the tile under every bomber (kind in the low 4 bits, 0x60 blocked).
static void log_start_tiles(void) {
    recomp_printf("TEST tiles");
    for (int slot = 0; slot < 8; slot++) {
        Player* p = &PLAYERS[slot];
        if (p->control_type == 0 || p->actor == 0) {
            continue;
        }
        ActorModel* m = p->actor->model;
        recomp_printf(" | %d %X @%d,%d,%d", slot, func_8026D7D8((int)(m->x / 100.0f), level_at(m->y), (int)(m->z / 100.0f)), (int)m->x, (int)m->y,
                      (int)m->z);
    }
    recomp_printf("\n");
}

#ifdef WATCH_CHANGES
// Which words of the game's globals ever change after the battle starts. A run with 4 players and one with 8
// differ in the words only slots 4 to 7 write: a table with room for 4 shows up there (tools/eightslots/
// watch_diff.py). Two ranges: data and bss before the heap, and the main code's bss.
static const unsigned int watch_ranges[][2] = {{0x80090000, 0x800C0000}, {0x802A0000, 0x802B4000}};
#define WATCH_WORDS ((0x800C0000 - 0x80090000 + 0x802B4000 - 0x802A0000) / 4)
static unsigned int watch_copy[WATCH_WORDS];
static unsigned char watch_changed[WATCH_WORDS / 8];

static void watch(int snapshot) {
    int w = 0;
    for (int r = 0; r < 2; r++) {
        for (unsigned int a = watch_ranges[r][0]; a < watch_ranges[r][1]; a += 4, w++) {
            unsigned int v = *(volatile unsigned int*)a;
            if (!snapshot && v != watch_copy[w]) {
                watch_changed[w >> 3] |= 1 << (w & 7);
            }
            watch_copy[w] = v;
        }
    }
}

// Changed words as ranges of addresses.
static void watch_log(void) {
    int w = 0;
    recomp_printf("TEST changed f=%d", frames);
    for (int r = 0; r < 2; r++) {
        unsigned int start = 0;
        for (unsigned int a = watch_ranges[r][0]; a <= watch_ranges[r][1]; a += 4, w++) {
            int on = a < watch_ranges[r][1] && (watch_changed[w >> 3] >> (w & 7) & 1);
            if (on && start == 0) {
                start = a;
            } else if (!on && start != 0) {
                recomp_printf(" %X-%X", start, a);
                start = 0;
            }
        }
        w--; // the extra step past the range's end
    }
    recomp_printf("\n");
}
#endif

#ifdef TRACE_OVERLAYS // every overlay switch and the title menu's table (overlay, entry, arg per choice)
RECOMP_HOOK("func_8023AE6C") void test_trace_overlay(int overlay, int entry, int arg) {
    recomp_printf("TEST overlay f=%d %X %X %X choice=%d table", frames, overlay, entry, arg, *(volatile int*)0x802B03A4);
    for (int i = 0; i < 15; i++) {
        recomp_printf(" %X", ((volatile int*)0x802A2C74)[i]);
    }
    recomp_printf("\n");
}

// Presses pad 1 buttons at set frames: writes them into the newest entry of the pad's ring (+0 held, +2 pressed).
static const int press_at[][2] = { PRESS_SCRIPT };
RECOMP_HOOK_RETURN("func_8023A104") void test_press(void) {
    for (unsigned i = 0; i < sizeof(press_at) / sizeof(press_at[0]); i++) {
        if (press_at[i][0] == frames) {
            int idx = *(volatile int*)0x80707B40;
            volatile unsigned short* e = (volatile unsigned short*)(0x80707840 + idx * 6);
            e[0] = (unsigned short)press_at[i][1];
            e[1] = (unsigned short)press_at[i][1];
            if (press_at[i][1] == 0x10000) { // the stick held down
                ((volatile signed char*)e)[5] = -80;
                *(volatile signed char*)0x80707803 = -80;
            }
            *(volatile unsigned short*)0x80707800 = (unsigned short)press_at[i][1];
            recomp_printf("TEST press f=%d %X\n", frames, press_at[i][1]);
        }
    }
}
#endif

// Once per game frame.
RECOMP_HOOK("func_8023A208") void test_every_frame(void) {
    frames++;
#ifdef LOG_HIGHMEM
    if (frames % 600 == 0) { void test_log_highmem(int); test_log_highmem(frames); }
#endif
#ifdef WATCH_CHANGES
    if (started) {
        watch(0);
        if (frames % WATCH_CHANGES == 0) {
            watch_log();
        }
    }
#endif
#ifdef TEST_TIME_JUMP // at that frame of the battle the clock jumps to 1:03 (Sudden Death at 1:00 with ghosts about)
    if (started && frames == TEST_TIME_JUMP) {
        BATTLE[0x2C / 4] = 1;
        BATTLE[0x30 / 4] = 3;
    }
#endif
    if (!started && frames == START_FRAME) {
        started = 1;
        frames = 0;
#ifdef WATCH_CHANGES
        watch(1);
#endif
        start_battle();
    } else if (started && !logged_tiles && PLAYERS[4].actor != 0 && PLAYERS[4].control_type != 0) {
        logged_tiles = 1; // the first frame the bombers stand, before they move
        log_start_tiles();
#ifdef DUMP_GRID // the whole stage grid at 0x800B15D8: header, then per layer its header and every tile
        {
            volatile unsigned char* g = (volatile unsigned char*)0x800B15D8;
            int sx = *(volatile int*)0x800B15DC * 8, sz = *(volatile int*)0x800B15E0 * 8;
            recomp_printf("TEST grid head");
            for (int i = 0; i < 0x14; i++) recomp_printf(" %02X", g[i]);
            recomp_printf("\n");
            for (int l = 0; l < g[0]; l++) {
                recomp_printf("TEST grid layer %d", l);
                for (int i = 0; i < 0x14; i++) recomp_printf(" %02X", g[0x14 + l * 0xC34 + i]);
                recomp_printf(" h=%d\n", (int)func_8026E2B0(l));
                for (int z = 0; z < sz; z++) {
                    recomp_printf("TEST grid %d %2d:", l, z);
                    for (int x = 0; x < sx; x++) recomp_printf(" %04X", func_8026D7D8(x, l, z) & 0xFFFF);
                    recomp_printf("\n");
                }
            }
        }
#endif
#ifdef TILE_ROW_Z // one row of the stage's tiles, to see what blocks and walls return
        recomp_printf("TEST row z=%d:", TILE_ROW_Z);
        for (int x = 14; x <= 32; x++) {
            recomp_printf(" %X", func_8026D7D8(x, 0, TILE_ROW_Z / 100));
        }
        recomp_printf("\n");
#endif
    } else if (started && frames % LOG_EVERY == 0) {
        log_state();
#ifdef LOG_LEVEL_TILES
        { void test_log_level_tiles(int); test_log_level_tiles(frames); }
#endif
#ifdef LOG_RIDE_TABLE
        // The table of lifted and ridden objects (moved from 0x802AFCE8, 0x30 per entry): + 0xC is the object.
        recomp_printf("TEST ride f=%d", frames);
        for (int i = 0; i < 16; i++) {
            recomp_printf(" %X", *(volatile int*)(0x80701000 + i * 0x30 + 0xC));
        }
        recomp_printf("\n");
#endif
#ifdef LOG_ENEMIES
        // The enemy pool (16 records of 0xA4 at 0x800BA900): actor (+0x14), kind index (+0x84) and the
        // kind table at 0x800BA8E8 (type, model file).
        recomp_printf("TEST enemies f=%d kinds", frames);
        for (int i = 0; i < 6; i++) {
            recomp_printf(" %X/%X", *(volatile short*)(0x800BA8E8 + i * 4), *(volatile short*)(0x800BA8EA + i * 4));
        }
        for (int i = 0; i < 16; i++) {
            volatile int* rec = (volatile int*)(0x800BA900 + i * 0xA4);
            if (rec[0x14 / 4] != 0) recomp_printf(" | %d a=%X k=%d", i, rec[0x14 / 4], rec[0x84 / 4]);
        }
        recomp_printf("\n");
#endif
    }
#ifdef DUMP_PLAYER_MODELS
    if (started && frames == 120) {
        dump_player_models();
    }
#endif
}

#ifdef TRACE_FILE_LOADS
// The file loader's decompressor: arguments are the open file, the buffer and its size. The hook shares
// its registers with the hooked function, so the call here must not leave the arguments changed.
#ifdef TRACE_HITS
// Every hit a player takes (func_80248148: slot, kind; hearts - 1, at 0 the player is out).
RECOMP_HOOK("func_80248148") void test_trace_hit(int slot, int kind, int arg) {
    Player* p = &PLAYERS[slot & 7];
    recomp_printf("TEST hit f=%d t=%d:%d slot=%d kind=%d arg=%X states=%X c=%d h=%d\n", frames, BATTLE[0x2C / 4],
                  BATTLE[0x30 / 4], slot, kind, arg, p->states, p->control_type, p->hearts);
}
#endif
RECOMP_HOOK("func_80292BD0") void test_trace_decompress(int file, int buffer, int size) {
    recomp_printf("TEST decompress f=%d file=%X buffer=%X size=%X\n", frames, file, buffer, size);
    register int a0 __asm__("$4") = file;
    register int a1 __asm__("$5") = buffer;
    register int a2 __asm__("$6") = size;
    __asm__ volatile("" : : "r"(a0), "r"(a1), "r"(a2));
}
#endif

#ifdef DUMP_PLAYER_MODELS
// Prints the loaded player model files 0x4A to 0x4D as hex, from the game's file table (0x800A7F30:
// 100 entries of { file id, buffer, ?, uses }).
#define FILE_TABLE ((volatile int*)0x800A7F30)
#define DUMP_BYTES 0x8000
static int dumped;

static void dump_player_models(void) {
    for (int i = 0; i < 100 && dumped != 0xF; i++) {
        int id = FILE_TABLE[i * 4];
        if (id < 0x4A || id > 0x4D || (dumped & (1 << (id - 0x4A)))) {
            continue;
        }
        dumped |= 1 << (id - 0x4A);
        const unsigned char* data = (const unsigned char*)FILE_TABLE[i * 4 + 1];
        for (int at = 0; at < DUMP_BYTES; at += 32) {
            recomp_printf("DUMP %X %05X", id, at);
            for (int b = 0; b < 32; b++) {
                recomp_printf(" %02X", data[at + b]);
            }
            recomp_printf("\n");
        }
    }
}
#endif



#ifdef LOG_HIGHMEM
// Which words from 0x80708000 to 0x80800000 are not zero (to find room for new data).
void test_log_highmem(int frames) {
    recomp_printf("TEST highmem f=%d", frames);
    unsigned int start = 0;
    for (unsigned int a = 0x80708000; a <= 0x80800000; a += 4) {
        int on = a < 0x80800000 && *(volatile unsigned int*)a != 0;
        if (on && start == 0) start = a;
        else if (!on && start != 0) { recomp_printf(" %X-%X", start, a); start = 0; }
    }
    recomp_printf("\n");
}
#endif

#ifdef LOG_LEVEL_TILES
// Tiles of the level test (stage 0, tools/eightslots README): hard blocks, soft ones with items.
static const int level_tiles[][3] = { {21,0,13}, {25,0,13}, {21,0,17}, {25,0,17}, {22,0,13}, {24,0,17},
                                      {19,0,11}, {27,0,19}, {19,0,19}, {27,0,11}, {23,0,12}, {23,0,18}, {17,0,10}, {29,0,20} };
void test_log_level_tiles(int frames) {
    recomp_printf("TEST level f=%d", frames);
    for (int i = 0; i < (int)(sizeof(level_tiles) / sizeof(level_tiles[0])); i++)
        recomp_printf(" %X", func_8026D7D8(level_tiles[i][0], level_tiles[i][1], level_tiles[i][2]));
    recomp_printf(" | items %X %X\n", *(volatile int*)0x802AFE80, *(volatile int*)0x802AFE84);
}
#endif
