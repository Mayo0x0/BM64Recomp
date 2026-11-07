#include "modding.h"
#include "recomputils.h"

// Bomberman 64 (US) state. The decomp has no names for these yet.
#define GAME_FLAGS       (*(volatile int*)0x802AC5D0)
#define BATTLE_TYPE      (*(volatile int*)0x802AC660) // 0 = the mode with stage hazards
#define MINUTES_LEFT     (*(volatile int*)0x802AC68C) // 0 from the start of Sudden Death
#define SUDDEN_DEATH_ON  (*(volatile int*)0x802AC698)
#define ROUND_DECIDED    (*(volatile int*)0x802AC854)
#define PLAYER_STATE(i)  (*(volatile int*)(0x800AEE14 + (i) * 0xC8)) // 0 = off, 3 = out

#define LOADED_STAGE_FILE (*(volatile int*)0x802AFC50) // set by func_8026FC08

#define GAME_FLAG_BATTLE 0x4
#define GAME_FLAG_ROUND_OVER 0x10
#define GREEDY_TRAP_STAGE_FILE 0x21C  // a story room uses this file too, hence the battle check

#define WATER_MODEL_FILE 0x207        // Pyramid's water
#define WATER_ALPHA 0x99              // as Pyramid
#define WATER_MODEL_Y_OFFSET 200.0f   // the stages draw the model this far above the death height
// shortcut: reaches the floor after 45 s of the 60 s Sudden Death, tune after playtesting.
#define FRAMES_UNTIL_FLOOR 2700.0f
#define PROCESS_PRIORITY 0x110        // as the stages' own Sudden Death processes
#define MAX_PLAYERS 4

typedef struct Actor {
    char pad[0x40];
    struct ActorModel* model;
} Actor;

typedef struct ActorModel {
    char pad0[0x14];
    float y;
    char pad1[0x36 - 0x18];
    short alpha;
} ActorModel;

float func_8026E8B8(void);                // water (death) height
void func_8026ECE8(float height);
Actor* func_802443F4(int slot);           // a player's actor
Actor* func_802310F4(int file, int flags, float x, float y, float z);
void func_80231408(Actor* actor, int flags);
void HuPrcCreateChecked(void* func, int arg, void* stack, int stack_size, unsigned short priority);
void HuPrcSleep1(void);
void HuPrcEnd(void);

// Rising is measured against the players, since a stage's floor height is not available here.
static float highest_player_y(float fallback) {
    float highest = fallback;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (PLAYER_STATE(i) != 0 && PLAYER_STATE(i) != 3) {
            Actor* player = func_802443F4(i);
            if (player != 0 && player->model->y > highest) {
                highest = player->model->y;
            }
        }
    }
    return highest;
}

// A port of the Sudden Death process of Pyramid and Up & Down (func_ovl14_3_8004350C). The model is
// created only now, since Greedy Trap has no water before Sudden Death.
static void water_process(void) {
    while (MINUTES_LEFT > 0) {
        HuPrcSleep1();
    }

    float start = func_8026E8B8();
    float floor_y = highest_player_y(start);
    float rise_per_frame = (floor_y - start) / FRAMES_UNTIL_FLOOR;
    recomp_printf("greedy_trap_water: start=%f floor=%f\n", start, floor_y);

    Actor* water = func_802310F4(WATER_MODEL_FILE, 0xD0, 0.0f, start + WATER_MODEL_Y_OFFSET, 0.0f);
    func_80231408(water, 0x1000000);
    func_80231408(water, 0x80000);
    water->model->alpha = WATER_ALPHA;

    float height = start;
    while (!(GAME_FLAGS & GAME_FLAG_ROUND_OVER) && ROUND_DECIDED == 0) {
        func_8026ECE8(height);
        water->model->y = height + WATER_MODEL_Y_OFFSET;
        HuPrcSleep1();
        height += rise_per_frame;
    }
    HuPrcEnd();
}

// func_8026FF64 loads a stage's collision file. The runtime cannot regenerate overlay functions
// for hooks, so this main-code function picks out Greedy Trap instead of the stage's own setup.
RECOMP_HOOK_RETURN("func_8026FF64") void gtw_after_stage_load(void) {
    if (LOADED_STAGE_FILE == GREEDY_TRAP_STAGE_FILE && (GAME_FLAGS & GAME_FLAG_BATTLE)
        && SUDDEN_DEATH_ON == 1 && BATTLE_TYPE == 0) {
        HuPrcCreateChecked(water_process, 0, 0, 0, PROCESS_PRIORITY);
    }
}
