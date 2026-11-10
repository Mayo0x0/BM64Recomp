// Eight player slots: slots 4 to 7 are computer players in single battles (tools/eightslots).
// gen_patches.py calls these from hooks in func_80245CDC and func_8023CA94.
#include "patches.h"

#define FIRST_EXTRA_SLOT 4
#define EXTRA_SLOTS 4
#define CONTROL_NONE 0
#define CONTROL_COMPUTER 2
#define CONTROL_OUT 3
#define TILE_SIZE 100.0f
#define ARENA_MIDDLE_X 2300.0f
#define ARENA_MIDDLE_Z 2300.0f // shortcut: guessed like X, check it with spawn points per stage

#define GAME_FLAGS (*(volatile s32*)0x802AC5D0)
#define GAME_FLAG_BATTLE 0x4
#define BATTLE_TYPE (*(volatile s32*)0x802AC660) // 0: last bomber standing, 1: teams
#define ALIVE_COUNT (*(volatile s32*)0x802AC758)
#define AI_DIFFICULTY (*(volatile s32*)0x802ACFC4) // shared by all computer players

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
    return (GAME_FLAGS & GAME_FLAG_BATTLE) && BATTLE_TYPE == 0 && !EXTRA_SLOTS_OFF;
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
static s32 recolored_slot(s32 fileID, s32* real) {
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

void eightslots_after_file_load(s32 fileID, void* data) {
    s32 real;
    s32 slot = recolored_slot(fileID, &real);
    if (slot < 0) {
        return;
    }
    const u8* body = sExtraColors[slot];
    u8* file = data;
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

// Players 5 to 8 as chosen on page 2 of Player Select: CONTROL_COMPUTER or CONTROL_NONE.
static s32 sExtraType[EXTRA_SLOTS] = { CONTROL_COMPUTER, CONTROL_COMPUTER, CONTROL_COMPUTER, CONTROL_COMPUTER };

// Before the room's players spawn: the spawn loop takes every slot with a control type.
void eightslots_before_spawn(void) {
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        g_Players[FIRST_EXTRA_SLOT + i].controlType = extra_slots_wanted() ? sExtraType[i] : CONTROL_NONE;
    }
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
    if (!extra_slots_wanted()) {
        return;
    }
    s32 uses[EXTRA_SLOTS] = { 0 };
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

        // shortcut: one tile from slot i towards the middle, which can be inside a block on some stages;
        // replace with spawn points per stage.
        // A second bot next to the same player goes the other way, along z.
        SlotModel* from = near->actor->model;
        SlotModel* to = bot->actor->model;
        to->x = from->x;
        to->y = from->y;
        to->z = from->z;
        if (uses[n]++ == 0) {
            to->x += from->x > ARENA_MIDDLE_X ? -TILE_SIZE : TILE_SIZE;
        } else {
            to->z += from->z > ARENA_MIDDLE_Z ? -TILE_SIZE : TILE_SIZE;
        }
        bot->spawn[0] = to->x; // a ghost comes back here
        bot->spawn[1] = to->y;
        bot->spawn[2] = to->z;

        s32 difficulty = AI_DIFFICULTY;
        func_80251804(FIRST_EXTRA_SLOT + i);
        AI_DIFFICULTY = difficulty; // the others keep the difficulty chosen in the menu
        ALIVE_COUNT++;
        sPlaying[i] = 1;
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

void g_AssignModelParts(void* model, s32 fileID, s32 part, s32 slot);
void g_PlayGlobalSound(s32 id);

static u32 sPageBlock[BLOCK_SIZE / 4];
static u8* sRealBlock;
static s32 sPage;
static s32 sExtraDifficulty[EXTRA_SLOTS] = { 1, 1, 1, 1 };

static s32* block_word(u8* block, s32 offset, s32 column) {
    return (s32*)(block + offset + column * 4);
}

static void turn_page(s32 page, s32 column) {
    if (page) {
        sRealBlock = MENU_BLOCK;
        for (s32 i = 0; i < BLOCK_SIZE / 4; i++) {
            sPageBlock[i] = ((u32*)sRealBlock)[i];
        }
        for (s32 c = 0; c < MENU_COLUMNS; c++) {
            *block_word((u8*)sPageBlock, BLOCK_TYPES, c) = sExtraType[c];
            *block_word((u8*)sPageBlock, BLOCK_DIFFICULTIES, c) = sExtraDifficulty[c];
            *block_word((u8*)sPageBlock, BLOCK_CUSTOM, c) = 0;
        }
        MENU_BLOCK = (u8*)sPageBlock;
    } else {
        for (s32 c = 0; c < MENU_COLUMNS; c++) {
            sExtraType[c] = *block_word((u8*)sPageBlock, BLOCK_TYPES, c);
            sExtraDifficulty[c] = *block_word((u8*)sPageBlock, BLOCK_DIFFICULTIES, c);
        }
        MENU_BLOCK = sRealBlock;
    }
    sPage = page;
    for (s32 c = 0; c < MENU_COLUMNS; c++) {
        s32 file = page ? RECOLORED_MODEL + c : WHITE_BOMBER_MODEL + c;
        for (s32 part = 0; part < MENU_MODEL_PARTS; part++) {
            g_AssignModelParts(MENU_MODEL(c), file, part, part);
        }
    }
    *(volatile u32*)(MENU_MODEL(MENU_COLUMN) + 0xC) |= MENU_MODEL_IDLE;
    MENU_COLUMN = column;
    *(volatile u32*)(MENU_MODEL(column) + 0xC) &= ~MENU_MODEL_IDLE;
    g_PlayGlobalSound(MENU_SOUND_MOVE);
}

// Cursor right on column 4 of page 1, or left on column 1 of page 2. Returns 1 when the page turned, so that
// the hook redoes the custom bombers.
s32 eightslots_menu_right(void) {
    if (sPage || MENU_COLUMN != MENU_COLUMNS - 1) {
        return 0;
    }
    turn_page(1, 0);
    return 1;
}

s32 eightslots_menu_left(void) {
    if (!sPage || MENU_COLUMN != 0) {
        return 0;
    }
    turn_page(0, MENU_COLUMNS - 1);
    return 1;
}

// Start or B leave Player Select: the rest of the menu edits the real block.
s32 eightslots_menu_leave(void) {
    if (!sPage) {
        return 0;
    }
    turn_page(0, MENU_COLUMN);
    return 1;
}

// Page 2 has no human players: MAN (type 1) is skipped, from off to COM and back.
s32 eightslots_menu_type(s32 type) {
    if (!sPage || type != 1) {
        return type;
    }
    return *block_word((u8*)sPageBlock, BLOCK_TYPES, MENU_COLUMN) == 0 ? CONTROL_COMPUTER : 0;
}

// For the menu's "at least 2 players" check.
s32 eightslots_menu_extra_count(void) {
    s32 count = 0;
    for (s32 i = 0; i < EXTRA_SLOTS; i++) {
        count += sExtraType[i] == CONTROL_COMPUTER;
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

Gfx* eightslots_menu_draw(Gfx* gfx) {
    if (BATTLE_TYPE != 0 || EXTRA_SLOTS_OFF || (++sMenuFrames & ARROW_BLINK)) {
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
