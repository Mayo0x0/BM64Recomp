// Settings > General > Quick Start - Battle Mode: the boot goes from the logo straight into the Battle menu.
#include "patches.h"
#include "misc_funcs.h"

void func_8023AE6C(s32 overlay, s32 entry, s32 arg); // goes to an overlay

extern s32 D_802A2C70;

#define OVL_INTRO 0x1B        // the intro and the title screen
#define OVL_LOGO_RETURN 0x1A
#define OVL_BATTLE_MENU 0x23  // what the title menu's Battle leads to

static s32 quick_started;

// Runs when the logo ends (pointer at 0x802A2CE4); the original goes on to the intro or, with D_802A2C70 set, to 0x1A.
RECOMP_PATCH void func_80296A34(void) {
    if (D_802A2C70 != 0) {
        func_8023AE6C(OVL_LOGO_RETURN, 0x10, 4);
    } else if (!quick_started && recomp_get_quick_start_battle()) {
        quick_started = 1; // only at boot: a later pass shows the intro as usual
        func_8023AE6C(OVL_BATTLE_MENU, 0x10, 0);
    } else {
        func_8023AE6C(OVL_INTRO, 0x10, 0);
    }
}
