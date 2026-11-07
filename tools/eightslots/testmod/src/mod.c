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

#define BATTLE ((volatile int*)0x802AC660)
#define GAME_FLAGS (*(volatile int*)0x802AC5D0)
#define ALIVE_COUNT (*(volatile int*)0x802AC758)
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
    char pad0[0x14];
    int hearts;
    int bomb_type;
    int bomb_count;
    int fire_power;
    int control_type;
    Actor* actor;
    char pad2C[0x68 - 0x2C];
    int states;
    char pad6C[0xC8 - 0x6C];
} Player;

#define PLAYERS ((Player*)PLAYERS_ADDRESS)

void func_8023DA04(int file_loaded);
void func_8023AE6C(int overlay, int entry, int arg);

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
    for (int i = 0; i < 0x40; i++) {
        BATTLE[i] = 0;
    }
    for (int slot = 0; slot < 4; slot++) {
        BATTLE[1 + slot] = slot < TEST_SLOTS ? 2 : 0; // computer player
        BATTLE[5 + slot] = 2;                         // difficulty
    }
    BATTLE[0x28 / 4] = 1;   // minutes: Sudden Death comes after one
    BATTLE[0x38 / 4] = 1;   // Sudden Death
    BATTLE[0x3C / 4] = 1;   // ghosts
    BATTLE[0xA0 / 4] = TEST_STAGE;
    recomp_printf("TEST start stage=%d slots=%d\n", TEST_STAGE, TEST_SLOTS);
    func_8023DA04(0);
    GAME_FLAGS &= ~LEFTOVER_INTRO_FLAGS;
    seed_random();
    func_8023AE6C(BATTLE_OVERLAY + TEST_STAGE, START_ENTRY, 0);
}

static void log_state(void) {
    recomp_printf("TEST f=%d flags=%X alive=%d t=%d:%d", frames, GAME_FLAGS, ALIVE_COUNT, BATTLE[0x2C / 4], BATTLE[0x30 / 4]);
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
    recomp_printf("\n");
}

// Hooks only functions without patches: the runtime rebuilds a hooked function from the original ROM,
// which would undo gen_patches.py for it.
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
