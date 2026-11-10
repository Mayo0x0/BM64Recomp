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

#define START_FRAME 180
#define LOG_EVERY 60
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
    BATTLE[0x28 / 4] = TEST_MINUTES; // 1 means Sudden Death at once, with no ghosts
    BATTLE[0x38 / 4] = 1;   // Sudden Death
    BATTLE[0x3C / 4] = 1;   // ghosts
    BATTLE[0xA0 / 4] = TEST_STAGE;
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
    recomp_printf("\n");
#ifdef AI_STATE_ADDRESS
    // Computer players: state (+0x4) and target slot (+0x10) of each slot's AI record.
    recomp_printf("TEST ai f=%d", frames);
    for (int slot = 0; slot < 8; slot++) {
        volatile int* ai = (volatile int*)(AI_STATE_ADDRESS + slot * 0x128);
        volatile float* own = (volatile float*)ai[0x18 / 4]; // the CPU's actor: it waits while + 0x6C is not 0
        recomp_printf(" | %d s%d t%d m%d f%X w%d v%d", slot, ai[1], ai[4], ai[5], ai[0], ai[0x88 / 4],
                      own != 0 ? (int)(own[0x6C / 4] * 100.0f) : -999);
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

// Once per game frame.
RECOMP_HOOK("func_8023A208") void test_every_frame(void) {
    frames++;
    if (!started && frames == START_FRAME) {
        started = 1;
        frames = 0;
        start_battle();
    } else if (started && frames % LOG_EVERY == 0) {
        log_state();
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
