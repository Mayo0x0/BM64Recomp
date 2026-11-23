#ifndef __LEVEL_LAYOUT_H__
#define __LEVEL_LAYOUT_H__

// Custom battle levels: what the frontend (src/game/level_editor.cpp) and the game side (patches/level.c) share in
// RDRAM. All fields are 32-bit words, so both sides read them the same way. The frontend writes the level area on the
// game thread (recomp_level_frame); the game writes the editor info for the frontend.
// 0x80708000 to 0x80800000 stays zero in battles (checked with 8 players in a team battle).

#define LEVEL_AREA 0x80710000u      // LevelHeader, then the records
#define LEVEL_AREA_SIZE 0x10000u
#define LEVEL_MAGIC 0x424D4C56u     // "BMLV"
#define LEVEL_STAGES 10             // battle stages 0 to 9

// LevelHeader (words): magic, editor, edit stage, revision, cursor, pad, then a record offset per stage.
#define LH_MAGIC 0
#define LH_EDITOR 1      // 1 in an editor session (src/game/level_editor.cpp starts it)
#define LH_EDIT_STAGE 2
#define LH_REVISION 3    // bumped on every change of the records
#define LH_CURSOR 4      // editor: the packed tile under the mouse, or LEVEL_NONE
#define LH_SYMMETRY 5    // editor: packed mirror tiles drawn with the cursor use this (bits: 1 x, 2 z)
#define LH_OFFSETS 6     // LEVEL_STAGES words: offset of a stage's record from LEVEL_AREA, 0 for none
#define LH_WORDS (LH_OFFSETS + LEVEL_STAGES)

// LevelRecord (words): rules (-1 keeps the game's), 8 start tiles, counts, then the tiles, the items, the shape and
// the looks.
#define LR_TIME 0        // minutes (1 to 5)
#define LR_SUDDEN 1      // Sudden Death 0 / 1
#define LR_GHOSTS 2      // ghosts 0 / 1
#define LR_BOMBS 3       // bombs at the start (1 to 8)
#define LR_FIRE 4        // fire at the start (1 to 8)
#define LR_BLOCKS 5      // soft blocks the game scatters at random (0 to 60)
#define LR_STARTS 6      // 8 packed tiles, LEVEL_NONE = the stage's own (slots 4 to 7: near a partner)
#define LR_TILE_COUNT 14
#define LR_ITEM_COUNT 15
#define LR_SHAPE_COUNT 16
#define LR_LOOK_COUNT 17
#define LR_DATA 18       // tile words, then item words, then shape words, then look words
#define LEVEL_MAX_TILES 2048
#define LEVEL_MAX_ITEMS 64
#define LEVEL_MAX_SHAPE 4096

#define LEVEL_NONE 0xFFFFFFFFu
// A packed tile: x | layer << 8 | z << 16 | value << 24 (value: the tile kind below, or the item).
#define LEVEL_PACK(x, layer, z, value) ((unsigned)(x) | (unsigned)(layer) << 8 | (unsigned)(z) << 16 | (unsigned)(value) << 24)
#define LEVEL_X(p) ((int)((p) & 0xFF))
#define LEVEL_LAYER(p) ((int)(((p) >> 8) & 0xFF))
#define LEVEL_Z(p) ((int)(((p) >> 16) & 0xFF))
#define LEVEL_VALUE(p) ((int)(((p) >> 24) & 0xFF))

// What a floor tile holds.
#define TILE_FREE 0      // never a block (grid flag 0x10)
#define TILE_RANDOM 1    // may get one of the random soft blocks
#define TILE_SOFT 2      // a soft block
#define TILE_HARD 3      // a block that explosions do not break (a soft block's model, taller)
#define TILE_KINDS 4

// The stage's shape: a shape word packs a grid tile's new kind (value) for every layer of a changed column.
#define SHAPE_AIR 0      // nothing: a bomber falls through
#define SHAPE_FLOOR 1
#define SHAPE_RAMP_PZ 3  // a ramp from this layer up to the next, rising toward + z (4: - z, 5: + x, 6: - x)
#define SHAPE_RAMP_NZ 4
#define SHAPE_RAMP_PX 5
#define SHAPE_RAMP_NX 6
#define SHAPE_SOLID 0xF  // a wall (a floor tile on the layer above is its top)
// A look: a column drawn with the top and sides of another column of the stage (x | z << 8 | from x << 16 | from z << 24).
#define LEVEL_MAX_LOOKS 2048
#define LOOK_PACK(x, z, fx, fz) ((unsigned)(x) | (unsigned)(z) << 8 | (unsigned)(fx) << 16 | (unsigned)(fz) << 24)

// Items in a soft block (the game's item numbers for func_8027B828).
#define ITEM_FIRE 0
#define ITEM_BOMB 1
#define ITEM_HEART 2
#define ITEM_POWER 4
#define ITEM_SKULL 5
#define ITEM_REMOTE 6

// Editor info, written by the game every frame of an editor session.
#define EDITOR_INFO 0x80720000u
#define EI_SEQ 0         // frames written
#define EI_READY 1       // 1 once the stage is set up for editing
#define EI_LAYERS 2
#define EI_HEIGHTS 3     // 8 floats: each layer's floor height
#define EI_CAMERA 11     // the camera the markers go with (viewport at + 0, view matrix at + 0x18)
#define EI_SHAPED 12     // 1 while the battle's level changes the stage's shape or looks (any battle)
#define EI_WORDS 13
// Layers and heights are written in every battle with a level (the frontend's stage mesh needs them too).

// Copy of the stage grid as loaded (before the level): the editor's "original" of every tile.
#define GRID_ADDRESS 0x800B15D8u
#define GRID_LAYER_SIZE 0xC34u
#define GRID_SIZE (0x14u + 8u * GRID_LAYER_SIZE)
#define GRID_ORIGINAL 0x8072A000u

// The stage model (func_8026FC08's stage file): its pointer, magic, and header entries of 3 words (type 0: a display
// list at the model + the third word). A level that changes the shape gets a rebuilt copy of those display lists
// (src/game/level_shape.cpp), their first command branches there.
#define STAGE_MODEL_POINTER 0x800A7F54u // the loaded files' table, slot 2 (0x800A0E7C is not it after the menus)
#define STAGE_MODEL_MAGIC 0x36340038u
#define SHAPE_MAP 0x80760000u        // editor: [layer][z][x] shape kind + 1 (0: the stage's), 64 x 64 per layer
#define SHAPE_MESH 0x80768000u       // the rebuilt display lists and their vertices
#define SHAPE_MESH_END 0x80800000u

#endif
