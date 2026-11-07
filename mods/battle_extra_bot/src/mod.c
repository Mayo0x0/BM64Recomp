#include "modding.h"
#include "recomputils.h"

// Bomberman 64 (US) state. The decomp has no names for these yet.
#define GAME_FLAGS       (*(volatile int*)0x802AC5D0)
#define DEATH_BLOCKED    (*(volatile int*)0x802A01B0) // func_8023A870 ignores deaths with bit 0x2
#define BATTLE_TYPE      (*(volatile int*)0x802AC660) // 0 = last bomber standing
#define GHOSTS_ENABLED   (*(volatile int*)0x802AC69C)
#define ALIVE_COUNT      (*(volatile int*)0x802AC758) // a round ends below 2
#define AI_DIFFICULTY    (*(volatile int*)0x802ACFC4) // shared by all CPUs, set by func_80251804

#define GAME_FLAG_NO_DEATH 0x1
#define GAME_FLAG_BATTLE 0x4

#define BOT_SLOT 4                    // the fifth player slot, which battles leave unused
#define HUMAN_SLOTS 4
#define CONTROL_NONE 0
#define CONTROL_COMPUTER 2
#define CONTROL_OUT 3
#define TILE_SIZE 100.0f
#define PROCESS_PRIORITY 0x110

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
    int hearts;        // 0x14
    int bomb_type;     // 0x18
    int bomb_count;    // 0x1C
    int fire_power;    // 0x20
    int control_type;  // 0x24
    Actor* actor;      // 0x28
    char pad2C[0x94 - 0x2C];
    int bomb_type_start;  // 0x94
    int fire_power_start; // 0x98
    char pad9C[0xC8 - 0x9C];
} Player;

#define PLAYERS ((Player*)0x800AEDF0)

void func_80251804(int slot);   // starts a slot's CPU process
void HuPrcCreateChecked(void* func, int arg, void* stack, int stack_size, unsigned short priority);
void HuPrcSleep1(void);

// An entry hook shares its registers with the hooked function, which still needs its argument in $a0.
#define KEEP_ARGUMENT(arg) __asm__ volatile("" : : "r"(arg))

static int saved_ghosts;
static int ghosts_replaced;

static int is_bot_battle(void) {
    return (GAME_FLAGS & GAME_FLAG_BATTLE) && BATTLE_TYPE == 0;
}

static void restore_ghosts(void) {
    if (ghosts_replaced) {
        ghosts_replaced = 0;
        GHOSTS_ENABLED = saved_ghosts;
    }
}

// Puts GHOSTS_ENABLED back after the bot's death process has read it. That process does not yield
// between the replacement and the read, so this can not run in between.
static void bot_process(void) {
    while (1) {
        restore_ghosts();
        HuPrcSleep1();
    }
}

// The room's player init spawns every slot with a control type, slot 4 included. g_SpawnPlayer has no
// model case for slot 4 and keeps the model of the slot it spawned before.
RECOMP_HOOK("func_80245CDC") void bot_before_player_spawn(void) {
    restore_ghosts(); // a scene change can end the death process before bot_process runs
    if (is_bot_battle()) {
        PLAYERS[BOT_SLOT].control_type = CONTROL_COMPUTER;
    }
}

// func_8023CA94 sets up slots 0 to 3 for the battle, after the spawn.
RECOMP_HOOK_RETURN("func_8023CA94") void bot_after_battle_setup(void) {
    if (!is_bot_battle() || PLAYERS[BOT_SLOT].control_type != CONTROL_COMPUTER) {
        return;
    }

    Player* bot = &PLAYERS[BOT_SLOT];
    Player* neighbour = 0;
    for (int i = HUMAN_SLOTS - 1; i >= 0 && neighbour == 0; i--) {
        if (PLAYERS[i].control_type != CONTROL_NONE && PLAYERS[i].actor != 0) {
            neighbour = &PLAYERS[i];
        }
    }
    if (neighbour == 0 || bot->actor == 0) {
        bot->control_type = CONTROL_NONE;
        return;
    }

    bot->hearts = neighbour->hearts;
    bot->bomb_type = neighbour->bomb_type;
    bot->bomb_count = neighbour->bomb_count;
    bot->fire_power = neighbour->fire_power;
    bot->bomb_type_start = neighbour->bomb_type_start;
    bot->fire_power_start = neighbour->fire_power_start;

    // shortcut: one tile towards the arena's centre from another player, can land in a block on some
    // stages, replace with a checked tile search when the bots get their own spawn points.
    ActorModel* from = neighbour->actor->model;
    ActorModel* to = bot->actor->model;
    to->x = from->x + (from->x > 0.0f ? -TILE_SIZE : TILE_SIZE);
    to->y = from->y;
    to->z = from->z;

    int difficulty = AI_DIFFICULTY;
    func_80251804(BOT_SLOT);
    AI_DIFFICULTY = difficulty; // keeps the other CPUs at the difficulty chosen in the menu

    HuPrcCreateChecked(bot_process, 0, 0, 0, PROCESS_PRIORITY);
    recomp_printf("battle_extra_bot: spawned at %d %d\n", (int)to->x, (int)to->z);
}

// The round ends on ALIVE_COUNT and its winner search only knows slots 0 to 3, so the bot stays out
// of the count. The death process (func_8023F26C) lowers it for every death, this makes up for that.
RECOMP_HOOK("func_8023A870") void bot_on_death(int slot) {
    if (slot == BOT_SLOT && !(DEATH_BLOCKED & 0x2) && !(GAME_FLAGS & GAME_FLAG_NO_DEATH)) {
        ALIVE_COUNT++;
    }
    KEEP_ARGUMENT(slot);
}

// func_8023F26C calls this right before its ghost test. A ghost respawn reads the slot's player type
// from a 4-entry array, which would make slot 4 a human on a pad that does not exist.
RECOMP_HOOK("func_8025C590") void bot_before_ghost_test(int slot) {
    if (slot == BOT_SLOT && !ghosts_replaced) {
        saved_ghosts = GHOSTS_ENABLED;
        ghosts_replaced = 1;
        GHOSTS_ENABLED = 0;
    }
    KEEP_ARGUMENT(slot);
}
