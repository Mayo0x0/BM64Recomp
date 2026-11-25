#ifndef __INPUT_H__
#define __INPUT_H__

#include "patch_helpers.h"

typedef enum {
    RECOMP_CAMERA_NORMAL,
    RECOMP_CAMERA_DUALANALOG,
} RecompCameraMode;

extern RecompCameraMode recomp_camera_mode;

DECLARE_FUNC(void, recomp_get_gyro_deltas, float* x, float* y);
DECLARE_FUNC(void, recomp_get_mouse_deltas, float* x, float* y);
DECLARE_FUNC(s32, recomp_get_targeting_mode);
DECLARE_FUNC(void, recomp_get_inverted_axes, s32* x, s32* y);
DECLARE_FUNC(s32, recomp_get_invert_y_axis_mode);
DECLARE_FUNC(s32, recomp_get_radio_comm_box_mode);
DECLARE_FUNC(void, recomp_get_analog_inverted_axes, s32* x, s32* y);
DECLARE_FUNC(void, recomp_get_camera_inputs, float* x, float* y);
DECLARE_FUNC(void, recomp_set_right_analog_suppressed, s32 suppressed);
// Ports 4 to 7 into 4 OSContPad at pads; returns a bit per port that answered (patches/eightslots.c).
DECLARE_FUNC(s32, recomp_get_extra_pads, void* pads);
DECLARE_FUNC(s32, recomp_netplay_sync, void* pads);
DECLARE_FUNC(s32, recomp_netplay_rng_seed, s32 seed);
// Custom levels (patches/level.c): the frontend copies new level data into RDRAM.
DECLARE_FUNC(void, recomp_level_frame);
// The frontend rebuilds the stage model to the grid's shape (src/game/level_shape.cpp).
DECLARE_FUNC(void, recomp_level_shape);
// Players 5 to 8 were chosen in Player Select: types, menu levels and teams (4 words each) for players.json.
DECLARE_FUNC(void, recomp_save_extra_players, s32* values);

#endif
