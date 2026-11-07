#include "modding.h"
#include "recomputils.h"

// Bomberman 64 (US) battle state. The decomp has no names for these yet.
#define GAME_FLAGS      (*(volatile int*)0x802AC5D0)
#define BATTLE_TYPE     (*(volatile int*)0x802AC660) // 0 = the mode with ghosts
#define MINUTES_LEFT    (*(volatile int*)0x802AC68C) // 0 from the start of Sudden Death
#define GHOSTS_ENABLED  (*(volatile int*)0x802AC69C)
#define ROUND_DECIDED   (*(volatile int*)0x802AC854) // set when time runs out

#define GAME_FLAG_BATTLE 0x4
#define GAME_FLAG_NO_GHOST 0x10

static int saved_minutes;
static int minutes_replaced;

// The death process (func_8023F26C) calls func_8025C590, then spawns a ghost only if
// GHOSTS_ENABLED, MINUTES_LEFT > 0 and !(GAME_FLAGS & 0x10). The test is in the middle of a
// large function that mods cannot hook, so MINUTES_LEFT is replaced just for that test.
RECOMP_HOOK_RETURN("func_8025C590") void sdg_before_ghost_test(void) {
    if ((GAME_FLAGS & GAME_FLAG_BATTLE) && !(GAME_FLAGS & GAME_FLAG_NO_GHOST) && BATTLE_TYPE == 0
        && GHOSTS_ENABLED != 0 && MINUTES_LEFT <= 0 && ROUND_DECIDED == 0) {
        saved_minutes = MINUTES_LEFT;
        minutes_replaced = 1;
        MINUTES_LEFT = 1;
    }
}

// func_80243E90 is the first call in the ghost branch. These conditions match that branch, and
// nothing yields in between, so the battle timer never sees the replaced value.
RECOMP_HOOK_RETURN("func_80243E90") void sdg_after_ghost_test(void) {
    if (minutes_replaced) {
        minutes_replaced = 0;
        MINUTES_LEFT = saved_minutes;
        recomp_printf("sudden_death_ghosts: ghost spawned during Sudden Death\n");
    }
}
