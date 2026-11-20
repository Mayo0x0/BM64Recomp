// Custom battle levels and the level editor's game side (include/level_layout.h, src/game/level_editor.cpp).
// A level changes a battle stage's grid: its shape (walls, floors, ramps and holes per layer; the frontend rebuilds
// the stage model to match, src/game/level_shape.cpp), which floor tiles get no block, a random one, a soft block or a
// hard one (a soft block's model, taller, that explosions do not break), where the bombers start, which item a soft
// block holds and some rules. gen_patches.py HOOKS call the level_* functions below.
#include "patches.h"
#include "input.h"
#include "../include/level_layout.h"

#define GAME_FLAGS (*(volatile s32*)0x802AC5D0)
#define GAME_FLAG_BATTLE 0x4
#define GAME_FLAG_TIMER_STOPPED 0x80
#define BATTLE ((volatile s32*)0x802AC660) // the battle config block
#define BATTLE_STAGE (BATTLE[0xA0 / 4])
#define BATTLE_MINUTES 0x28
#define BATTLE_SUDDEN 0x38
#define BATTLE_GHOSTS 0x3C
#define EXTRA_SLOTS_OFF (*(volatile s32*)0x80707F20) // patches/eightslots.c

#define WORDS(address) ((volatile u32*)(address))
#define HEADER WORDS(LEVEL_AREA)
#define INFO WORDS(EDITOR_INFO)

// The stage grid (func_8026FC08 loads it): layers, then chunks of 8 x 8 tiles.
#define GRID ((volatile u8*)GRID_ADDRESS)
#define GRID_LAYERS (GRID[0])
#define GRID_CHUNKS_X (*(volatile s32*)(GRID_ADDRESS + 4))
#define GRID_CHUNKS_Z (*(volatile s32*)(GRID_ADDRESS + 8))
#define TILE_KIND 0xF
#define TILE_FLOOR 1
#define TILE_NO_BLOCK 0x10
#define TILE_BLOCKS 0x60   // a block of class 0x20, 0x40 or 0x60
#define TILE_SOFT_BLOCK 0x20
#define TILE_START 0x380   // (slot + 1) << 7 marks slot 0 to 3's start
#define MAX_LAYERS 8
#define MAX_SIDE 64

u32 func_8026D7D8(s32 x, s32 layer, s32 z);           // a tile
void func_8026D620(s32 x, s32 layer, s32 z, u32 v);   // sets a tile
f32 func_8026E2B0(s32 layer);                         // a layer's floor height
void func_8026F068(s32 x, s32 layer, s32 z, s32 cls); // creates a block of a class on a tile
void func_8027D328(void* block);                      // frees a block and clears its tile's block bits
void func_8023DA04(s32 arg);
void func_8023AE6C(s32 overlay, s32 entry, s32 arg);  // goes to an overlay (0x90 + stage: a battle)

// The blocks (func_8027E67C allocates them): 48 records of 0x28.
#define BLOCK_POOL 0x80090490
#define BLOCK_POOL_END 0x80090C10
#define BLOCK_SIZE 0x28
typedef struct Block {
    s16 pad0;
    s16 state;  // 1: free
    u8 pad4[0xC - 0x4];
    u8* model;  // + 0x10 position, + 0x28 scale (3 floats)
    u8 pad10[0x1A - 0x10];
    s16 flags;  // the tile's block bits
    f32 x, layer, z;
} Block;
#define MODEL_SCALE_Y 0x2C
#define HARD_SCALE 1.5f

// Random soft blocks of class 0x20 (func_8027DF9C): how many func_8026F8B0 scatters.
#define RANDOM_BLOCKS (*(volatile s16*)0x802B0194)
s32 func_8027B828(s32 item, s32 x, s32 layer, s32 z); // spawns an item on a tile (as from a block)

// Editor buffers: the wanted kind of every tile (built from the record), and two marker display lists.
#define KIND_MAP ((volatile u8*)0x80731000) // [layer][z][x], MAX_SIDE x MAX_SIDE per layer
#define KIND_FROM_LEVEL 0x80
#define SHAPE_MAP_AT(l, z, x) (((volatile u8*)SHAPE_MAP)[((l) * MAX_SIDE + (z)) * MAX_SIDE + (x)])
#define DRAW_BUFFERS 0x80740000
#define DRAW_BUFFER_SIZE 0x10000

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
extern SlotPlayer g_Players[8];

static s32 sSavedRandom = -1;
static s32 sSavedRules[3];
static s32 sRulesSet;       // the config holds a level's rules (sSavedRules are the player's)
static s32 sFrames;
static s32 sEditorStarted;
static u32 sAppliedRevision;
static u32 sStarts[8];      // editor: where each slot starts (drawn)
static u32 sOwnStarts[4];   // where the stage's own starts of slots 0 to 3 are once the shape is applied

static const s32 sRuleOffsets[3] = { BATTLE_MINUTES, BATTLE_SUDDEN, BATTLE_GHOSTS };

static void restore_rules(void) {
    for (s32 i = 0; i < 3; i++) {
        BATTLE[sRuleOffsets[i] / 4] = sSavedRules[i];
    }
    sRulesSet = 0;
}

static volatile u32* level_record(void) {
    if (HEADER[LH_MAGIC] != LEVEL_MAGIC || !(GAME_FLAGS & GAME_FLAG_BATTLE)) {
        return NULL;
    }
    s32 stage = BATTLE_STAGE;
    if (stage < 0 || stage >= LEVEL_STAGES || HEADER[LH_OFFSETS + stage] == 0) {
        return NULL;
    }
    if (HEADER[LH_EDITOR] && (u32)stage != HEADER[LH_EDIT_STAGE]) {
        return NULL;
    }
    return WORDS(LEVEL_AREA + HEADER[LH_OFFSETS + stage]);
}

static s32 editing(void) {
    return HEADER[LH_MAGIC] == LEVEL_MAGIC && HEADER[LH_EDITOR];
}

static s32 grid_width(void) {
    s32 w = GRID_CHUNKS_X * 8;
    return w > MAX_SIDE ? MAX_SIDE : w;
}

static s32 grid_depth(void) {
    s32 d = GRID_CHUNKS_Z * 8;
    return d > MAX_SIDE ? MAX_SIDE : d;
}

static s32 grid_layers(void) {
    s32 n = GRID_LAYERS;
    return n > MAX_LAYERS ? MAX_LAYERS : n;
}

// A tile of the grid as the stage loaded it (copied in level_before_blocks), same layout as the grid.
static u32 original_tile(s32 x, s32 layer, s32 z) {
    u32 chunk = (u32)(z >> 3) * GRID_CHUNKS_X + (u32)(x >> 3);
    return *(volatile u16*)(GRID_ORIGINAL + layer * GRID_LAYER_SIZE + chunk * 0x82 + 0x14 + ((z & 7) * 8 + (x & 7)) * 2);
}

static u32 tile_bits(s32 kind) {
    switch (kind) {
    case TILE_FREE: return TILE_NO_BLOCK;
    case TILE_RANDOM: return 0;
    default: return TILE_SOFT_BLOCK;
    }
}

static s32 kind_of(u32 tile) {
    if (tile & TILE_BLOCKS) {
        return TILE_SOFT;
    }
    return (tile & TILE_NO_BLOCK) ? TILE_FREE : TILE_RANDOM;
}

static volatile u32* record_tiles(volatile u32* rec) {
    return rec + LR_DATA;
}

static volatile u32* record_items(volatile u32* rec) {
    return rec + LR_DATA + rec[LR_TILE_COUNT];
}

static volatile u32* record_shape(volatile u32* rec) {
    return rec + LR_DATA + rec[LR_TILE_COUNT] + rec[LR_ITEM_COUNT];
}

// A tile the shape makes: its kind with the bits the stage uses for it (walls and ramps never get a block, a new
// floor tile gets none at random either).
static u32 shape_tile(s32 kind) {
    return kind == SHAPE_AIR ? 0 : (u32)kind | TILE_NO_BLOCK;
}

// The tile a level's shape leaves before blocks and starts: the stage's own where the kind stays.
static u32 shaped_tile(u32 own, s32 kind) {
    return (s32)(own & TILE_KIND) == kind ? own : shape_tile(kind);
}

static s32 record_kind(volatile u32* rec, s32 x, s32 layer, s32 z) {
    u32 at = LEVEL_PACK(x, layer, z, 0);
    volatile u32* tiles = record_tiles(rec);
    for (u32 i = 0; i < rec[LR_TILE_COUNT]; i++) {
        if ((tiles[i] & 0xFFFFFF) == at) {
            return LEVEL_VALUE(tiles[i]);
        }
    }
    return -1;
}

static Block* block_at(s32 x, s32 layer, s32 z) {
    for (u32 a = BLOCK_POOL; a < BLOCK_POOL_END; a += BLOCK_SIZE) {
        Block* b = (Block*)a;
        if (b->state != 1 && (s32)b->x == x && (s32)b->layer == layer && (s32)b->z == z) {
            return b;
        }
    }
    return NULL;
}

// The highest floor tile of a column, or -1.
static s32 top_floor(s32 x, s32 z) {
    for (s32 l = grid_layers() - 1; l >= 0; l--) {
        if ((func_8026D7D8(x, l, z) & TILE_KIND) == TILE_FLOOR) {
            return l;
        }
    }
    return -1;
}

// Writes the shape into the grid (a changed tile loses its block and its start mark). The stage's own starts of
// slots 0 to 3 move to the top floor of their column (sOwnStarts; LEVEL_NONE when the column has no floor left).
// The grid must be the stage's own except where the record's shape changes it.
static void apply_shape(volatile u32* rec) {
    s32 layers = grid_layers(), width = grid_width(), depth = grid_depth();
    for (s32 slot = 0; slot < 4; slot++) {
        sOwnStarts[slot] = LEVEL_NONE;
    }
    for (s32 l = 0; l < layers; l++) {
        for (s32 z = 0; z < depth; z++) {
            for (s32 x = 0; x < width; x++) {
                u32 code = original_tile(x, l, z) & TILE_START;
                if (code != 0 && (code >> 7) <= 4) {
                    sOwnStarts[(code >> 7) - 1] = LEVEL_PACK(x, l, z, 0);
                }
            }
        }
    }
    volatile u32* shape = record_shape(rec);
    for (u32 i = 0; i < rec[LR_SHAPE_COUNT]; i++) {
        s32 x = LEVEL_X(shape[i]), l = LEVEL_LAYER(shape[i]), z = LEVEL_Z(shape[i]);
        if (l >= layers || x >= width || z >= depth) {
            continue;
        }
        u32 want = shaped_tile(original_tile(x, l, z), LEVEL_VALUE(shape[i]));
        u32 tile = func_8026D7D8(x, l, z);
        if ((tile & TILE_KIND) == (want & TILE_KIND)) {
            continue;
        }
        Block* b = block_at(x, l, z);
        if (b != NULL) {
            func_8027D328(b);
        }
        func_8026D620(x, l, z, want & ~TILE_START);
    }
    for (s32 slot = 0; slot < 4; slot++) {
        u32 at = sOwnStarts[slot];
        if (at == LEVEL_NONE) {
            continue;
        }
        s32 x = LEVEL_X(at), l = LEVEL_LAYER(at), z = LEVEL_Z(at);
        if ((func_8026D7D8(x, l, z) & TILE_KIND) != TILE_FLOOR) {
            l = top_floor(x, z);
            sOwnStarts[slot] = l < 0 ? LEVEL_NONE : LEVEL_PACK(x, l, z, 0);
        }
    }
}

// Layers and heights for the frontend (picking, the stage mesh).
static void write_heights(void) {
    s32 layers = grid_layers();
    INFO[EI_LAYERS] = (u32)layers;
    for (s32 l = 0; l < MAX_LAYERS; l++) {
        *(volatile f32*)&INFO[EI_HEIGHTS + l] = l < layers ? func_8026E2B0(l) : 0.0f;
    }
}

static void set_hard_look(Block* b, s32 hard) {
    if (b->model != NULL) {
        *(f32*)(b->model + MODEL_SCALE_Y) = hard ? HARD_SCALE : 1.0f;
    }
}

// Puts the start marks of slots 0 to 3 where the level places them, else on the stage's own (sOwnStarts).
static void apply_starts(volatile u32* rec) {
    s32 layers = grid_layers(), width = grid_width(), depth = grid_depth();
    for (s32 slot = 0; slot < 4; slot++) {
        u32 code = (u32)(slot + 1) << 7;
        u32 want = rec[LR_STARTS + slot];
        u32 own = sOwnStarts[slot];
        for (s32 l = 0; l < layers; l++) {
            for (s32 z = 0; z < depth; z++) {
                for (s32 x = 0; x < width; x++) {
                    u32 tile = func_8026D7D8(x, l, z);
                    if ((tile & TILE_START) == code && (want != LEVEL_NONE || own != LEVEL_PACK(x, l, z, 0))) {
                        func_8026D620(x, l, z, tile & ~TILE_START);
                    }
                }
            }
        }
        sStarts[slot] = LEVEL_NONE;
        if (want == LEVEL_NONE && own != LEVEL_NONE) {
            s32 x = LEVEL_X(own), l = LEVEL_LAYER(own), z = LEVEL_Z(own);
            u32 tile = func_8026D7D8(x, l, z);
            func_8026D620(x, l, z, (tile & ~TILE_START) | code);
            sStarts[slot] = own;
        }
        if (want != LEVEL_NONE) {
            s32 x = LEVEL_X(want), l = LEVEL_LAYER(want), z = LEVEL_Z(want);
            u32 tile = func_8026D7D8(x, l, z);
            if ((tile & TILE_KIND) == TILE_FLOOR) {
                func_8026D620(x, l, z, (tile & ~TILE_START) | code);
            }
            sStarts[slot] = want;
        }
    }
}

// Before func_8026F8B0 creates the blocks of the grid's block bits and scatters the random soft blocks
// (gen_patches.py HOOKS). The level's tiles only get their bits here: the game then makes their blocks.
void level_before_blocks(void) {
    sSavedRandom = -1;
    volatile u32* rec = level_record();
    if (rec == NULL) {
        if (sRulesSet) {
            restore_rules(); // a round on a stage without a level
        }
        return;
    }
    for (u32 i = 0; i < GRID_SIZE; i += 4) {
        *(volatile u32*)(GRID_ORIGINAL + i) = *(volatile u32*)(GRID_ADDRESS + i);
    }
    for (s32 slot = 0; slot < 8; slot++) {
        sStarts[slot] = LEVEL_NONE;
    }
    recomp_printf("LEVEL stage %d: %d tiles, %d items, %d shape%s\n", BATTLE_STAGE, rec[LR_TILE_COUNT],
                  rec[LR_ITEM_COUNT], rec[LR_SHAPE_COUNT], editing() ? " (editor)" : "");
    apply_shape(rec);
    volatile u32* tiles = record_tiles(rec);
    for (u32 i = 0; i < rec[LR_TILE_COUNT]; i++) {
        s32 x = LEVEL_X(tiles[i]), l = LEVEL_LAYER(tiles[i]), z = LEVEL_Z(tiles[i]);
        u32 tile = func_8026D7D8(x, l, z);
        if ((tile & TILE_KIND) == TILE_FLOOR && l < grid_layers()) {
            func_8026D620(x, l, z, (tile & ~(TILE_NO_BLOCK | TILE_BLOCKS)) | tile_bits(LEVEL_VALUE(tiles[i])));
        }
    }
    apply_starts(rec);
    for (s32 slot = 4; slot < 8; slot++) {
        sStarts[slot] = rec[LR_STARTS + slot];
    }
    sSavedRandom = RANDOM_BLOCKS;
    if (editing()) {
        RANDOM_BLOCKS = 0; // the editor shows where they may fall instead
    } else if ((s32)rec[LR_BLOCKS] >= 0) {
        RANDOM_BLOCKS = (s16)rec[LR_BLOCKS];
    }
    // The rules, while the battle lasts (Sudden Death and ghosts are read when the time runs out); level_frame
    // gives the config the player's own back afterwards.
    if (!sRulesSet) {
        for (s32 i = 0; i < 3; i++) {
            sSavedRules[i] = BATTLE[sRuleOffsets[i] / 4];
        }
        sRulesSet = 1;
    }
    for (s32 i = 0; i < 3; i++) {
        s32 rule = (s32)rec[LR_TIME + i];
        BATTLE[sRuleOffsets[i] / 4] = rule >= 0 && !editing() ? rule : sSavedRules[i];
    }
}

// At the end of func_8026F8B0: hard blocks look taller.
void level_after_blocks(void) {
    if (sSavedRandom >= 0) {
        RANDOM_BLOCKS = (s16)sSavedRandom;
    }
    volatile u32* rec = level_record();
    if (rec == NULL) {
        return;
    }
    volatile u32* tiles = record_tiles(rec);
    for (u32 i = 0; i < rec[LR_TILE_COUNT]; i++) {
        if (LEVEL_VALUE(tiles[i]) == TILE_HARD) {
            Block* b = block_at(LEVEL_X(tiles[i]), LEVEL_LAYER(tiles[i]), LEVEL_Z(tiles[i]));
            if (b != NULL) {
                set_hard_look(b, 1);
            }
        }
    }
    write_heights();
    recomp_level_shape(); // the frontend rebuilds the stage model to the grid's shape
    if (editing()) {
        sAppliedRevision = ~HEADER[LH_REVISION]; // level_frame builds the kind map
        INFO[EI_READY] = 1;
    }
}

// Before func_8027D894 frees a block an explosion reached (gen_patches.py HOOKS): 1 keeps a hard block.
s32 level_block_is_hard(Block* b) {
    volatile u32* rec = level_record();
    if (rec == NULL) {
        return 0;
    }
    return record_kind(rec, (s32)b->x, (s32)b->layer, (s32)b->z) == TILE_HARD;
}

// A block an explosion broke drops its item (func_8027D398, gen_patches.py HOOKS): the level's item for the
// block's tile instead of a random one. Returns 1 when it dropped one.
s32 level_block_item(Block* b) {
    volatile u32* rec = level_record();
    if (rec == NULL) {
        return 0;
    }
    s32 x = (s32)b->x, l = (s32)b->layer, z = (s32)b->z;
    u32 at = LEVEL_PACK(x, l, z, 0);
    volatile u32* items = record_items(rec);
    for (u32 i = 0; i < rec[LR_ITEM_COUNT]; i++) {
        if ((items[i] & 0xFFFFFF) == at) {
            s32 ok = func_8027B828(LEVEL_VALUE(items[i]), x, l, z);
            recomp_printf("LEVEL item %d at %d,%d,%d: %d\n", LEVEL_VALUE(items[i]), x, l, z, ok);
            return 1;
        }
    }
    return 0;
}

// After the battle setup placed the bombers (patches/eightslots.c): the level's start gear.
void level_after_battle_setup(void) {
    volatile u32* rec = level_record();
    if (rec == NULL || editing()) {
        return;
    }
    for (s32 slot = 0; slot < 8; slot++) {
        SlotPlayer* p = &g_Players[slot];
        if (p->controlType == 0 || p->actor == NULL) {
            continue;
        }
        if ((s32)rec[LR_BOMBS] > 0) {
            p->bombCount = (s32)rec[LR_BOMBS];
        }
        if ((s32)rec[LR_FIRE] > 0) {
            p->firePower = (s32)rec[LR_FIRE];
            p->firePowerStart = (s32)rec[LR_FIRE];
        }
    }
}

// Slots 4 to 7 (patches/eightslots.c): the level's start tile for a slot, or 0 to let eightslots place it.
s32 level_extra_start(s32 slot, f32* x, f32* y, f32* z) {
    volatile u32* rec = level_record();
    if (rec == NULL || rec[LR_STARTS + slot] == LEVEL_NONE) {
        return 0;
    }
    u32 at = rec[LR_STARTS + slot];
    if ((func_8026D7D8(LEVEL_X(at), LEVEL_LAYER(at), LEVEL_Z(at)) & TILE_KIND) != TILE_FLOOR) {
        return 0;
    }
    *x = LEVEL_X(at) * 100.0f;
    *y = func_8026E2B0(LEVEL_LAYER(at));
    *z = LEVEL_Z(at) * 100.0f;
    return 1;
}

// Editor: the tile the level's shape makes of a tile, before blocks and starts (SHAPE_MAP, built by editor_sync).
static u32 base_tile(s32 x, s32 l, s32 z) {
    u32 own = original_tile(x, l, z);
    u8 shape = SHAPE_MAP_AT(l, z, x);
    return shape == 0 ? own : shaped_tile(own, shape - 1);
}

// Editor: brings the grid's shape, blocks and starts in line with the record after a change.
static void editor_sync(volatile u32* rec) {
    s32 layers = grid_layers(), width = grid_width(), depth = grid_depth();
    for (s32 l = 0; l < layers; l++) {
        for (s32 z = 0; z < depth; z++) {
            for (s32 x = 0; x < width; x++) {
                KIND_MAP[(l * MAX_SIDE + z) * MAX_SIDE + x] = 0xFF;
                SHAPE_MAP_AT(l, z, x) = 0;
            }
        }
    }
    // The shape: tiles an earlier version changed go back to the stage's own, then the record's shape is applied.
    volatile u32* shape = record_shape(rec);
    for (u32 i = 0; i < rec[LR_SHAPE_COUNT]; i++) {
        s32 x = LEVEL_X(shape[i]), l = LEVEL_LAYER(shape[i]), z = LEVEL_Z(shape[i]);
        if (l < layers && x < width && z < depth) {
            SHAPE_MAP_AT(l, z, x) = (u8)(LEVEL_VALUE(shape[i]) + 1);
        }
    }
    for (s32 l = 0; l < layers; l++) {
        for (s32 z = 0; z < depth; z++) {
            for (s32 x = 0; x < width; x++) {
                u32 own = original_tile(x, l, z);
                u32 tile = func_8026D7D8(x, l, z);
                if ((tile & TILE_KIND) != (own & TILE_KIND) && SHAPE_MAP_AT(l, z, x) == 0) {
                    Block* b = block_at(x, l, z);
                    if (b != NULL) {
                        func_8027D328(b);
                    }
                    func_8026D620(x, l, z, own & ~TILE_START);
                }
            }
        }
    }
    apply_shape(rec);
    volatile u32* tiles = record_tiles(rec);
    for (u32 i = 0; i < rec[LR_TILE_COUNT]; i++) {
        s32 x = LEVEL_X(tiles[i]), l = LEVEL_LAYER(tiles[i]), z = LEVEL_Z(tiles[i]);
        if (l < layers && x < width && z < depth) {
            KIND_MAP[(l * MAX_SIDE + z) * MAX_SIDE + x] = (u8)(LEVEL_VALUE(tiles[i]) | KIND_FROM_LEVEL);
        }
    }
    for (s32 l = 0; l < layers; l++) {
        for (s32 z = 0; z < depth; z++) {
            for (s32 x = 0; x < width; x++) {
                u32 own = base_tile(x, l, z);
                if ((own & TILE_KIND) != TILE_FLOOR) {
                    continue;
                }
                volatile u8* entry = &KIND_MAP[(l * MAX_SIDE + z) * MAX_SIDE + x];
                if (*entry == 0xFF) {
                    *entry = (u8)kind_of(own);
                }
                s32 want = *entry & ~KIND_FROM_LEVEL;
                Block* b = block_at(x, l, z);
                u32 tile = func_8026D7D8(x, l, z);
                if (want == TILE_SOFT || want == TILE_HARD) {
                    if (b == NULL) {
                        func_8026D620(x, l, z, tile & ~(TILE_NO_BLOCK | TILE_BLOCKS));
                        func_8026F068(x, l, z, TILE_SOFT_BLOCK);
                        b = block_at(x, l, z);
                    }
                    tile = func_8026D7D8(x, l, z);
                    func_8026D620(x, l, z, (tile & ~(TILE_NO_BLOCK | TILE_BLOCKS)) | TILE_SOFT_BLOCK);
                    if (b != NULL) {
                        set_hard_look(b, want == TILE_HARD);
                    }
                } else {
                    if (b != NULL) {
                        func_8027D328(b);
                        tile = func_8026D7D8(x, l, z);
                    }
                    func_8026D620(x, l, z, (tile & ~(TILE_NO_BLOCK | TILE_BLOCKS)) | tile_bits(want));
                }
            }
        }
    }
    apply_starts(rec);
    for (s32 slot = 4; slot < 8; slot++) {
        sStarts[slot] = rec[LR_STARTS + slot];
    }
    recomp_level_shape();
}

// The editor session: a battle on the stage being edited with two idle bombers, the timer stopped.
static void editor_start_battle(void) {
    s32 stage = (s32)HEADER[LH_EDIT_STAGE];
    EXTRA_SLOTS_OFF = 1;
    for (s32 i = 0; i < 0x40; i++) {
        BATTLE[i] = 0;
    }
    BATTLE[1] = 1; // humans; nobody plays them while the editor takes the input
    BATTLE[2] = 1;
    for (s32 slot = 0; slot < 4; slot++) {
        BATTLE[5 + slot] = 2;
    }
    BATTLE[BATTLE_MINUTES / 4] = 5;
    BATTLE_STAGE = stage;
    func_8023DA04(0);
    GAME_FLAGS &= ~0x90; // the attract sequence's flags
    func_8023AE6C(0x90 + stage, 0x10, 0);
}

// Every frame, at the controller poll (patches/eightslots.c).
void level_frame(void) {
    sFrames++;
    recomp_level_frame(); // the frontend copies new level data in
    if (sRulesSet && !(GAME_FLAGS & GAME_FLAG_BATTLE)) {
        restore_rules();
    }
    if (!editing()) {
        return;
    }
    for (s32 i = 0; i < 8 * 6; i += 2) {
        *(volatile u16*)(0x80707800 + i) = 0; // the bombers stand still (patches/eightslots.c CONT_PADS)
    }
    if (!sEditorStarted && sFrames >= 180) {
        sEditorStarted = 1;
        editor_start_battle();
    }
    INFO[EI_SEQ] = (u32)sFrames;
    volatile u32* rec = level_record();
    if (rec == NULL || !INFO[EI_READY]) {
        return;
    }
    GAME_FLAGS |= GAME_FLAG_TIMER_STOPPED;
    write_heights();
    if (sAppliedRevision != HEADER[LH_REVISION]) {
        sAppliedRevision = HEADER[LH_REVISION];
        editor_sync(rec);
    }
}

// Marker drawing (editor): flat squares a little above the floor of a tile.
typedef struct Marker {
    Gfx* gfx;
    Vtx* vtx;
    s32 count; // vertices in the batch
} Marker;

static void flush_markers(Marker* m) {
    if (m->count == 0) {
        return;
    }
    Vtx* first = m->vtx - m->count;
    gSPVertex(m->gfx++, first, m->count, 0);
    for (s32 i = 0; i < m->count; i += 4) {
        gSP2Triangles(m->gfx++, i, i + 1, i + 2, 0, i, i + 2, i + 3, 0);
    }
    m->count = 0;
}

static void add_square(Marker* m, s32 x, s32 layer, s32 z, s32 inset, u32 rgba) {
    f32 y = func_8026E2B0(layer) + 3.0f;
    s32 half = 50 - inset;
    static const s8 corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (s32 i = 0; i < 4; i++) {
        Vtx* v = m->vtx++;
        v->v.ob[0] = (s16)(x * 100 + corners[i][0] * half);
        v->v.ob[1] = (s16)y;
        v->v.ob[2] = (s16)(z * 100 + corners[i][1] * half);
        v->v.flag = 0;
        v->v.tc[0] = 0;
        v->v.tc[1] = 0;
        v->v.cn[0] = (u8)(rgba >> 24);
        v->v.cn[1] = (u8)(rgba >> 16);
        v->v.cn[2] = (u8)(rgba >> 8);
        v->v.cn[3] = (u8)rgba;
    }
    m->count += 4;
    if (m->count == 16) {
        flush_markers(m);
    }
}

static const u32 sSlotColors[8] = {
    0xFFFFFFC0, 0x303030C0, 0xFF3030C0, 0x3060FFC0, 0x30C040C0, 0xFFE030C0, 0xA040E0C0, 0xFF9020C0,
};

static u32 item_color(s32 item) {
    switch (item) {
    case ITEM_FIRE: return 0xFF8000E0;
    case ITEM_BOMB: return 0x2040FFE0;
    case ITEM_HEART: return 0xFF60C0E0;
    case ITEM_POWER: return 0xE00000E0;
    case ITEM_SKULL: return 0x909090E0;
    default: return 0x8030C0E0;
    }
}

// The battle HUD (patches/eightslots.c) ends with this: in the editor, the markers, drawn with camera 0's view
// (func_8022787C sets it up the same way: projection, then the camera's view matrix multiplied in).
#define CAMERA0 0x800BD5B0
#define PROJECTION_MTX 0x802A53D8
Gfx* level_draw(Gfx* gfx) {
    static s32 sDrawnFrame = -1;
    volatile u32* rec = editing() ? level_record() : NULL;
    if (rec == NULL || !INFO[EI_READY] || sAppliedRevision != HEADER[LH_REVISION] || sDrawnFrame == sFrames) {
        return gfx;
    }
    sDrawnFrame = sFrames;
    INFO[EI_CAMERA] = CAMERA0;
    u8* buffer = (u8*)(DRAW_BUFFERS + (sFrames & 1) * DRAW_BUFFER_SIZE);
    // The identity in the RSP's fixed point: integer halves of rows 0 to 3, then the fractions (all 0).
    Mtx* identity = (Mtx*)buffer;
    static const s32 ident[16] = { 0x10000, 0, 1, 0, 0, 0x10000, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (s32 i = 0; i < 16; i++) {
        ((s32*)identity)[i] = ident[i];
    }
    Gfx* list = (Gfx*)(buffer + sizeof(Mtx));
    Marker m;
    m.gfx = list;
    m.vtx = (Vtx*)(buffer + 0x4000);
    m.count = 0;

    gDPPipeSync(m.gfx++);
    gSPViewport(m.gfx++, (Vp*)CAMERA0);
    gSPMatrix(m.gfx++, (Mtx*)PROJECTION_MTX, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(m.gfx++, (Mtx*)(CAMERA0 + 0x18), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
    gDPSetCycleType(m.gfx++, G_CYC_1CYCLE);
    gDPSetRenderMode(m.gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(m.gfx++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture(m.gfx++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(m.gfx++, G_LIGHTING | G_CULL_BOTH | G_FOG | G_TEXTURE_GEN | G_ZBUFFER);
    gSPSetGeometryMode(m.gfx++, G_SHADE | G_SHADING_SMOOTH);
    gSPMatrix(m.gfx++, identity, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

    // Tiles that may get a random soft block: faint.
    s32 layers = grid_layers(), width = grid_width(), depth = grid_depth();
    for (s32 l = 0; l < layers; l++) {
        for (s32 z = 0; z < depth; z++) {
            for (s32 x = 0; x < width; x++) {
                u8 entry = KIND_MAP[(l * MAX_SIDE + z) * MAX_SIDE + x];
                if (entry != 0xFF && (entry & ~KIND_FROM_LEVEL) == TILE_RANDOM &&
                    (base_tile(x, l, z) & TILE_KIND) == TILE_FLOOR) {
                    add_square(&m, x, l, z, 30, 0x00000060);
                }
            }
        }
    }
    volatile u32* items = record_items(rec);
    for (u32 i = 0; i < rec[LR_ITEM_COUNT]; i++) {
        add_square(&m, LEVEL_X(items[i]), LEVEL_LAYER(items[i]), LEVEL_Z(items[i]), 15, item_color(LEVEL_VALUE(items[i])));
    }
    for (s32 slot = 0; slot < 8; slot++) {
        if (sStarts[slot] != LEVEL_NONE) {
            add_square(&m, LEVEL_X(sStarts[slot]), LEVEL_LAYER(sStarts[slot]), LEVEL_Z(sStarts[slot]), 8, sSlotColors[slot]);
        }
    }
    u32 cursor = HEADER[LH_CURSOR];
    if (cursor != LEVEL_NONE && LEVEL_LAYER(cursor) < layers) {
        add_square(&m, LEVEL_X(cursor), LEVEL_LAYER(cursor), LEVEL_Z(cursor), 0, 0xFFE000A0);
    }
    flush_markers(&m);
    gDPPipeSync(m.gfx++);
    gSPSetGeometryMode(m.gfx++, G_ZBUFFER);
    gSPEndDisplayList(m.gfx++);

    gSPDisplayList(gfx++, list);
    return gfx;
}
