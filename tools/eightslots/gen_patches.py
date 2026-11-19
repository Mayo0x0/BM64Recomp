"""Writes bm64.eightslots.toml: the recompiler config plus this project's instruction patches.

Every patch checks the instruction it replaces against the ROM, so that a wrong address stops the
build here instead of becoming a bug in the game. Run N64Recomp on the written file.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_bounds import loop_bounds
from mips_refs import load_funcs, resolve

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE_CONFIG = os.path.join(ROOT, "bm64.us.toml")
OUT_CONFIG = os.path.join(ROOT, "bm64.eightslots.toml")
ROM = os.path.join(ROOT, "bm64.decomp.us.z64")
OVERLAYS = os.path.join(HERE, ".analysis", "recomp_overlays.inl")
GENERATED = os.path.join(HERE, "generated")

SECTION = re.compile(r"\.rom_addr = (0x[0-9A-F]+), \.ram_addr = (0x[0-9A-F]+), \.size = (0x[0-9A-F]+), \.funcs = section_\d+_(\w+)_funcs")
FUNCTION_TABLE = re.compile(r"static FuncEntry section_\d+_(\w+)_funcs\[\]")
FUNCTION_ENTRY = re.compile(r"\.func = (\w+),")
SYMBOL_ADDRESS = re.compile(r"vram = 0x([0-9A-Fa-f]+)")

HEAP_SIZE_MB = 4
# Above the heap (0x80300000 + HEAP_SIZE_MB); nothing in the game uses this memory.
RELOCATED_BASE = 0x80700000
PLAYER_SIZE = 0xC8
MAX_PLAYERS = 8

AI_STATE_SIZE = 0x128
COMMAND_SIZE = 0x10
# The block's arrays have up to 6 entries, of which the use of the last is not known yet: room for 8 + spare.
BLOCK_ARRAY_SIZE = 16 * 4
# Everything used in this range moves, so that a lui shared by its members stays valid.
SLOT_BLOCK = (0x802AC8A8, 0x802ACA90)

SLOT_LIMITS = [
    ("func_80245CDC", 0x80245D40, 5),  # spawns the room's players
    ("func_8024FFF0", 0x80250020, 5),  # updates the players every frame
    ("func_80277B54", 0x80277BF4, 5),  # resets a field of every player
    ("func_80278F68", 0x80278F94, 5),  # resets a field of every player
    ("func_802441AC", 0x802441D8, 5),  # finds the slot of an actor
    # An object hitting a bomber: finds the bomber's slot; slots 5 to 7 were not found and the hit did nothing.
    ("func_802797BC", 0x802797E8, 5), ("func_802797BC", 0x802797F8, 5), ("func_802797BC", 0x80279804, 5),
    ("func_8027BB48", 0x8027BB68, 5),  # clears HIT_COUNTS
    ("func_80242DA8", 0x80242E50, 4),  # finds the round's winner
    ("func_80242994", 0x80242A1C, 4),  # round end: stops every player
    ("func_80242994", 0x80242A6C, 4),  # round end: counts who is left, 1 is a win
    ("func_80242994", 0x80242C34, 4),  # round end: finds the winner (its result code: RESULT_CODE_HOOK)
    ("func_ovl3_7_800435FC", 0x800437D0, 4),  # match winner screen: whose wins reached the target
    ("func_8023AEDC", 0x8023AEFC, 4),  # buttons of all controllers, for the battle pause (func_8023BBCC)
]
# g_getRandomNumber(n) returns 0 to n: the highest slot.
HIGHEST_SLOT_LIMITS = [
    ("func_80251708", 0x80251720, 3),  # a computer player's target; it draws again for an empty slot
]

HIT_COUNTS = (0x800BA8A0, 0x800BA8E8)

# Moving objects and the actors riding them. func_80273370 looks an actor up here and the caller uses the
# result without a check, so a player missing from the 5 entries crashes the game.
RIDE_TABLE = (0x802AFCE8, 0x802AFDD8)
RIDE_ENTRY_SIZE = 0x30
RIDE_TABLE_ENTRIES = 16
RIDE_TABLE_LIMITS = [(func, vram, 5) for func, vram in [
    ("func_80270770", 0x802707A0), ("func_80270770", 0x80270800),
    # Clear the table when a stage starts; entries 5 to 15 kept the last round's objects, so a new bomb or
    # crystal at the same address counted as held by a player and nobody could lift it.
    ("func_802714C4", 0x80271508), ("func_802777D0", 0x80277810),
    ("func_80271178", 0x8027118C),
    ("func_80270B4C", 0x80270B8C),
    ("func_80277B54", 0x80277BBC),
    ("func_80271BB8", 0x80271BD8), ("func_80271BB8", 0x80271E18),
    ("func_802741D8", 0x802741F4), ("func_802741D8", 0x80274804),
    ("func_802731A4", 0x802731C4), ("func_802731A4", 0x802731E8),
    ("func_80273370", 0x802733B0),
    ("func_802702BC", 0x802702CC),
    ("func_802702EC", 0x80270300),
    ("func_80270C24", 0x80270C58),
    ("func_80270714", 0x80270728),
    ("func_80274994", 0x80274A64),
    ("func_80273F30", 0x80273F6C),
    ("func_80273E2C", 0x80273E48), ("func_80273E2C", 0x80273E58), ("func_80273E2C", 0x80273EA4),
    ("func_802727D8", 0x80272D80), ("func_802727D8", 0x80272D8C), ("func_802727D8", 0x80272D9C),
    ("func_802727D8", 0x80272DE8),
]]

def call_with_result(name, args, result):
    """Hook text calling a patch function with a0 = args; `result` is C using `r` (its v0) on the restored ctx."""
    return ("{ int32_t r; void " + name + "(uint8_t* rdram, recomp_context* ctx); recomp_context saved = *ctx; "
            + args + " " + name + "(rdram, ctx); r = (int32_t)ctx->r2; *ctx = saved; " + result + " }")


CALL_PATCH = ("{{ void {0}(uint8_t* rdram, recomp_context* ctx); recomp_context saved = *ctx; "
              "{0}(rdram, ctx); *ctx = saved; }}")

# The battle config has player types for 4 slots, and overlays read it by fixed offsets, so it cannot grow:
# for later slots, the value read is really the next field. eightslots_extra_type gives theirs: human (1) or
# computer (2). The text runs before the branch that tests the value.
# Registers: s2 = r18, t7 = r15, v1 = r3, t8 = r24, t9 = r25.
def extra_type(slot, register):
    return "if ((int32_t)" + slot + " >= 4) " + call_with_result(
        "eightslots_extra_type", "ctx->r4 = " + slot + ";", "ctx->" + register + " = r;")


HOOKS = [
    # Netplay: the game seeds its random numbers from the clock at boot; in a session every PC takes the same seed.
    ("g_initRandom", 0x8025E6A4, call_with_result("eightslots_rng_seed", "", "ctx->r4 = r;")),
    # Netplay, BM64_NETPLAY_TRACERNG: logs who takes each random number (src/game/recomp_api.cpp).
    ("g_getRandomNumber", 0x80234248, "{ void recomp_netplay_trace_rng(uint8_t* rdram, recomp_context* ctx); "
     "recomp_netplay_trace_rng(rdram, ctx); }"),
    ("g_nextRandomDouble", 0x8025E63C, "{ void recomp_netplay_trace_rng(uint8_t* rdram, recomp_context* ctx); "
     "recomp_netplay_trace_rng(rdram, ctx); }"),
    ("func_8023EB28", 0x8023EB44, extra_type("ctx->r18", "r15")),
    ("func_8023EB28", 0x8023EBD0, extra_type("ctx->r18", "r3")),
    ("func_8023F26C", 0x8023F694, extra_type("MEM_W(0xAC, ctx->r29)", "r24")),
    ("func_8023F26C", 0x8023F870, extra_type("MEM_W(0xAC, ctx->r29)", "r25")),
    # After the game read its 4 controllers: ports 5 to 8 (CONT_PADS).
    ("func_8023A104", 0x8023A158, CALL_PATCH.format("eightslots_read_extra_pads")),
    # patches/eightslots.c, with the registers kept for the hooked code.
    ("func_80245CDC", 0x80245CDC, CALL_PATCH.format("eightslots_before_spawn")),
    ("func_8023CA94", 0x8023CC3C, CALL_PATCH.format("eightslots_after_battle_setup")),
    # Every computer player plays at its own level (patches/eightslots.c); s0 = r16 is its AI state.
    ("func_80252D14", 0x80252D14, call_with_result("eightslots_ai_level", "ctx->r4 = ctx->r16;", "")),
    # Round result code (0x802AC5D8): 0x11 + slot for slots 0 to 3, but 0x15 to 0x18 already mean a draw
    # and team results, so slots 4 to 7 get 0x19 to 0x1C. Only the results screen (ovl3_7) learns them.
    # s0 = r16 is the winner's slot, 8 for none; t7 = r15 is the code it stores.
    ("func_80242994", 0x80242CB4, "{ int32_t s = (int32_t)ctx->r16; ctx->r15 = s < 4 ? 0x11 + s : s < 8 ? 0x15 + s : 0x15; }"),
    # The overlay's entry number is the code too (0x11 to 0x14 start the round results).
    ("func_ovl3_7_80043010", 0x80043014, "if ((int32_t)ctx->r4 >= 0x19 && (int32_t)ctx->r4 <= 0x1C) ctx->r4 = 0x11; "
     "else if ((int32_t)ctx->r4 >= 0x1D && (int32_t)ctx->r4 <= 0x1E) ctx->r4 = 0x16;"),
    ("func_ovl3_7_80043010", 0x800430B8, "if ((int32_t)ctx->r2 >= 0x19 && (int32_t)ctx->r2 <= 0x1C) ctx->r2 = 0x11; "
     "else if ((int32_t)ctx->r2 >= 0x1D && (int32_t)ctx->r2 <= 0x1E) ctx->r2 = 0x16;"),
    ("func_ovl3_7_80043010", 0x80043240, "if ((int32_t)ctx->r2 >= 0x19 && (int32_t)ctx->r2 <= 0x1C) ctx->r2 = 0x11; "
     "else if ((int32_t)ctx->r2 >= 0x1D && (int32_t)ctx->r2 <= 0x1E) ctx->r2 = 0x16;"),
    ("func_ovl3_7_8004400C", 0x8004473C, "if ((int32_t)ctx->r23 >= 0x19 && (int32_t)ctx->r23 <= 0x1C) ctx->r23 -= 4;"),
    # Results screens: slots 4 to 7 join the list, use their recolored models, and have no custom
    # bomber (their type and custom flag would be the next fields of the config block).
    ("func_ovl3_7_8004400C", 0x80044150, CALL_PATCH.format("eightslots_results_participants")),
    # After a bomber of the round results got its place; s0 = r16 points at its model.
    ("func_ovl3_7_8004400C", 0x800443DC, "{ void eightslots_results_scale(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = MEM_W(0, ctx->r16); eightslots_results_scale(rdram, ctx); *ctx = saved; }"),
    # The medals (earlier wins, then the one flying in): v0 = r2 is the new model. The new one flies to
    # f26, which hangs 80 above the row like eightslots_results_medal().
    ("func_ovl3_7_8004400C", 0x80044558, "{ void eightslots_results_medal(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = ctx->r2; eightslots_results_medal(rdram, ctx); *ctx = saved; }"),
    # Team results (config block + 0 is 1): the bombers stand in one column, 640 / n apart from 90 - 360 / n down
    # (eightslots_results_teams), so the medal goes to the winning team's (s7 = r23) first row.
    ("func_ovl3_7_8004400C", 0x80044960, "{ int32_t n = MEM_W(0, (gpr)(int32_t)0x80046F1C); "
     "float scale = n > 4 ? 4.0f / n : 1.0f; "
     "if (MEM_W(0, (gpr)(int32_t)0x802AC660) == 1 && n > 0) { "
     + call_with_result("eightslots_results_team_row", "ctx->r4 = ctx->r23;",
                        "ctx->f26.fl = 90.0f + 80.0f * scale - 360.0f / n - r * 640.0f / n;")
     + " } else if (n > 4) ctx->f26.fl -= 80.0f * (1.0f - scale); }"),
    ("func_ovl3_7_8004400C", 0x80044828, "{ void eightslots_results_scale(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = ctx->r2; eightslots_results_scale(rdram, ctx); *ctx = saved; }"),
    ("func_ovl3_7_8004400C", 0x800443F0, "if ((int32_t)ctx->r5 >= 4) ctx->r5 += 0x7F00 - 4 - 0x4A;"),
    ("func_ovl3_7_800435FC", 0x800439DC, "if ((int32_t)ctx->r5 >= 4) ctx->r5 += 0x7F00 - 4 - 0x4A;"),
    ("func_ovl3_7_8004400C", 0x8004442C, "if ((int32_t)ctx->r2 >= 4) ctx->r14 = 0;"),
    ("func_ovl3_7_800435FC", 0x80043A18, "if ((int32_t)ctx->r2 >= 4) ctx->r15 = 0;"),
    ("func_ovl3_7_8004400C", 0x80044A84, "if ((int32_t)ctx->r6 >= 4) ctx->r14 = 0;"),
    # Wins of slots 4 to 7 (config block + 0xB4) are cleared where the game clears those of 0 to 3.
    ("func_ovl3_3_8004386C", 0x80046244, CALL_PATCH.format("eightslots_reset_wins")),
    ("func_8023DB0C", 0x8023DC84, CALL_PATCH.format("eightslots_reset_wins")),
    # After the HUD stored its display list pointer; a0 gets that pointer's address (sp + 0x11C).
    ("func_80241BB0", 0x80242238, "{ void eightslots_draw_hud(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = MEM_W(0x11C, ctx->r29); eightslots_draw_hud(rdram, ctx); *ctx = saved; }"),
]

# Player Select, page 2 (patches/eightslots.c): after a page turn, each column's custom bomber is redone by
# the menu's func_ovl3_3_8004368C (s2 = r18: the column's model, s1 = r17: custom parts or 0), as on entry.
MENU_REFRESH = (
    "for (int c = 0; c < 4; c++) { gpr b = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)0x8004A324); "
    "ctx->r18 = (gpr)(int32_t)MEM_W(c * 4, (gpr)(int32_t)0x8004A400); "
    "ctx->r17 = (MEM_W(4 + c * 4, b) == 1 && MEM_W(0x40 + c * 4, b) == 1) ? b + 0x50 + c * 16 : 0; "
    "func_ovl3_3_8004368C(rdram, ctx); }")


# Team Select places each column's bomber (team 1 lower) and its type text only on entry (func_ovl3_3_8004386C
# 0x800451BC); after a page turn all 4 columns are placed again the same way, with teams 2 and 3 like 0 and 1.
TEAM_REFRESH = (
    "for (int c = 0; c < 4; c++) { gpr b = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)0x8004A324); "
    "int32_t lower = MEM_W(0x90 + c * 4, b) & 1; union { float f; uint32_t u; } v; "
    "gpr model = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)(0x80048EAC + c * 0x14)); "
    "gpr text = (gpr)(int32_t)(0x80048DA0 + c * 0x18); "
    "v.f = lower * -70.0f; MEM_W(0x14, model) = v.u; "
    "MEM_W(0x38, (gpr)(int32_t)MEM_W(0x14, text)) = MEM_W(4 + c * 4, b) * 2; "
    "v.f = lower * 76.0f + 116.0f; MEM_W(4, text) = v.u; "
    "ctx->r4 = text; ctx->f12.fl = 1.0f; func_ovl3_3_80043170(rdram, ctx); }")


def menu_turn(name):
    return ("{ void " + name + "(uint8_t* rdram, recomp_context* ctx); recomp_context saved = *ctx; "
            + name + "(rdram, ctx); if (ctx->r2) { " + MENU_REFRESH + " } *ctx = saved; }")


MENU = "func_ovl3_3_8004386C"
HOOKS += [
    (MENU, 0x80044340, menu_turn("eightslots_menu_right")),  # right pressed, the column is in t6
    (MENU, 0x800442A8, menu_turn("eightslots_menu_left")),  # left pressed
    (MENU, 0x80044190, menu_turn("eightslots_menu_leave")),  # Start
    (MENU, 0x80044224, menu_turn("eightslots_menu_leave")),  # B
    # The new type (fp = r30) before it is stored.
    (MENU, 0x80044498, "{ int32_t eightslots_menu_type_result; void eightslots_menu_type(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = ctx->r30; eightslots_menu_type(rdram, ctx); "
     "eightslots_menu_type_result = (int32_t)ctx->r2; *ctx = saved; ctx->r30 = (gpr)eightslots_menu_type_result; }"),
    # The column highlight's draw function, before its last store: v0 = r2 is the display list's end.
    ("func_ovl3_3_80046918", 0x800469A8, "{ gpr end; void eightslots_menu_draw(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = ctx->r2; eightslots_menu_draw(rdram, ctx); end = ctx->r2; "
     "*ctx = saved; ctx->r2 = end; }"),
    # "At least 2 players" (s7 = r23 counted slots 1 to 4) also counts the computer players of page 2.
    (MENU, 0x800441BC, "{ int32_t n; void eightslots_menu_extra_count(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; eightslots_menu_extra_count(rdram, ctx); n = (int32_t)ctx->r2; *ctx = saved; "
     "ctx->r23 += n; }"),
]

# Team battles with up to 4 teams (patches/eightslots.c). Per-team data for 2 teams moves to fixed places with
# room for 4, so that the C code knows them; the last two are read-only and filled at boot (required_patches.c).
TEAMS = 4
TEAM_MOVES = [
    (0x802AC760, 0x802AC768, 0x80707D00, TEAMS * 4),  # the team's crystal still stands
    (0x802AC820, 0x802AC828, 0x80707D10, TEAMS * 4),  # the team's crystal (enemy record)
    (0x802AC828, 0x802AC850, 0x80707D20, TEAMS * 0x14),  # its 5 HUD gems
    (0x802A0748, 0x802A0750, 0x80707D70, TEAMS * 4),  # x of the team's gem column
    (0x802A0750, 0x802A0758, 0x80707D80, TEAMS * 4),  # tile of its gems in the HUD sheet
]


TEAM_SELECT = "func_ovl3_3_8004386C"
HOOKS += [
    # Battle setup: teams 2 and 3, and which teams are in play.
    ("func_8023CA94", 0x8023CC1C, CALL_PATCH.format("eightslots_team_setup")),
    # The team process (s5 = r21: team): crystal model and gem sheet of teams 2 and 3, members in all 8 slots.
    ("func_80242FB4", 0x8024302C, "if ((int32_t)ctx->r21 >= 2) { MEM_H(0, (gpr)(int32_t)(0x800BA8E8 + ctx->r21 * 4)) = 0x19; "
     "MEM_H(2, (gpr)(int32_t)(0x800BA8E8 + ctx->r21 * 4)) = 0x7F20 + (int32_t)ctx->r21 - 2; }"),
    ("func_80242FB4", 0x80243080, "if ((int32_t)ctx->r21 >= 2) ctx->r4 = 0x7F30 + (int32_t)ctx->r21 - 2;"),
    ("func_80242FB4", 0x8024312C, call_with_result("eightslots_team_has_member", "ctx->r4 = ctx->r21;", "ctx->r24 = r;")),
    ("func_80242FB4", 0x8024313C, "if (ctx->r24) ctx->r25 = ctx->r21;"),
    # Round end: over when at most one team's crystal stands (the game checked teams 0 and 1).
    ("func_80242DA8", 0x80242EF8, call_with_result("eightslots_teams_left", "", "ctx->r15 = r > 1;")),
    ("func_80242DA8", 0x80242F08, "ctx->r24 = 1;"),
    ("func_80242DA8", 0x80242F20, call_with_result("eightslots_teams_left", "", "ctx->r25 = r > 1;")),
    ("func_80242DA8", 0x80242F30, call_with_result("eightslots_teams_left", "", "ctx->r14 = r > 1;")),
    ("func_80242994", 0x80242AA8, call_with_result("eightslots_teams_left", "", "ctx->r25 = r > 0;")),
    ("func_80242994", 0x80242AB8, call_with_result("eightslots_teams_left", "", "ctx->r14 = r > 0;")),
    ("func_80242994", 0x80242D74, call_with_result("eightslots_winning_team", "", "ctx->r15 = r < 2 ? 0x16 + r : 0x1B + r;")),
    # Computer players: the crystal they see belongs to any enemy team (s3 = r19); targets by team in 8 slots.
    ("func_802553D4", 0x8025B838, call_with_result("eightslots_ai_enemy_team", "ctx->r4 = MEM_W(0xC, ctx->r30); ctx->r5 = ctx->r17;",
                                                   "ctx->r19 = r;")),
    ("func_80251708", 0x80251790, call_with_result("eightslots_same_team", "ctx->r4 = ctx->r17; ctx->r5 = ctx->r19;",
                                                   "ctx->r25 = 0; ctx->r24 = r ? 0 : 1;")),
    # Team results: teams 2 and 3 (codes 0x1D, 0x1E), and everyone who played, by team.
    # Each earlier team medal (v0 = r2) next to its team (s7 = r23), where the 2 team layout put it in a group.
    ("func_ovl3_7_8004400C", 0x800446F0, "{ void eightslots_team_medal(uint8_t* rdram, recomp_context* ctx); "
     "recomp_context saved = *ctx; ctx->r4 = ctx->r2; ctx->r5 = ctx->r23; eightslots_team_medal(rdram, ctx); *ctx = saved; }"),
    ("func_ovl3_7_8004400C", 0x800447D4, "if ((int32_t)ctx->r23 >= 7) ctx->r23 -= 5;"),
    ("func_ovl3_7_8004400C", 0x800442AC, call_with_result("eightslots_results_teams", "", "ctx->r19 = r;")),
    # Team Select: 4 teams on up and down, teams 0 and 2 at the top, page 2, and the start check for 8 players.
    (TEAM_SELECT, 0x80045514, CALL_PATCH.format("eightslots_team_cycle").replace("recomp_context saved = *ctx; ",
                                                                                 "recomp_context saved = *ctx; ctx->r4 = (gpr)-1; ")
     + " ctx->r15 = 0;"),
    (TEAM_SELECT, 0x80045578, CALL_PATCH.format("eightslots_team_cycle").replace("recomp_context saved = *ctx; ",
                                                                                 "recomp_context saved = *ctx; ctx->r4 = 1; ")
     + " ctx->r14 = 1;"),
    (TEAM_SELECT, 0x80045210, "ctx->r24 &= 1;"),
    (TEAM_SELECT, 0x80045268, "ctx->r25 &= 1;"),
    (TEAM_SELECT, 0x8004565C, "ctx->r24 &= 1;"),
    (TEAM_SELECT, 0x800456B8, "ctx->r24 &= 1;"),
    (TEAM_SELECT, 0x80045640, CALL_PATCH.format("eightslots_team_screen")),
    (TEAM_SELECT, 0x80045410, call_with_result("eightslots_team_right", "", "if (r) { recomp_context kept = *ctx; " + MENU_REFRESH + TEAM_REFRESH + " *ctx = kept; ctx->r25 = 3; }")),
    (TEAM_SELECT, 0x80045328, call_with_result("eightslots_team_left", "", "if (r) { recomp_context kept = *ctx; " + MENU_REFRESH + TEAM_REFRESH + " *ctx = kept; ctx->r14 = 0; }")),
    (TEAM_SELECT, 0x800455B8, call_with_result("eightslots_team_leave", "", "if (r) { recomp_context kept = *ctx; " + MENU_REFRESH + TEAM_REFRESH + " *ctx = kept; }")),
    (TEAM_SELECT, 0x800455F8, call_with_result("eightslots_team_ok", "", "ctx->r23 = r; ctx->r19 = r;")),
    (TEAM_SELECT, 0x80045630, call_with_result("eightslots_team_leave", "", "if (r) { recomp_context kept = *ctx; " + MENU_REFRESH + TEAM_REFRESH + " *ctx = kept; }")),
]

# {0, 1, 2, 3}: read-only data, so its new place is filled at boot in patches/required_patches.c.
SLOT_NUMBERS = 0x80707F00

G_PLAYERS = 0x800AEDF0
PLAYER_SLOTS_IN_USE = MAX_PLAYERS
# The battle HUD indexes g_Players by its element number, which starts 1 or 3 before the player's slot,
# so its base lies before the array: (function, base) -> how many players before g_Players.
BASES_BEFORE_ARRAY = {
    ("func_80241BB0", 0x800AED4C): 1,
    ("func_80241BB0", 0x800AEBBC): 3,
}
# Controllers (func_8023A104 polls them every frame): the pads read (OSContPad, 6 bytes), a ring of the last 16
# states per controller (0x60) and each ring's position. Room for 8: ports 5 to 8 come from
# recomp_get_extra_pads (patches/eightslots.c), and a human player reads the controller of its slot.
CONT_PADS = (0x802AC410, 0x802AC428, 0x80707800, MAX_PLAYERS * 6)
CONT_RINGS = (0x802AC428, 0x802AC5A8, 0x80707840, MAX_PLAYERS * 0x60)
CONT_RING_AT = (0x802AC5A8, 0x802AC5B8, 0x80707B40, MAX_PLAYERS * 4)

# Pointer loops that end at an array's end: (function, addiu vram) -> (the array, its size for 8).
LOOP_BOUNDS = {
    ("func_80245868", 0x802458A0): (G_PLAYERS, MAX_PLAYERS * PLAYER_SIZE),
    ("func_80245868", 0x80245904): (G_PLAYERS, MAX_PLAYERS * PLAYER_SIZE),
    ("func_8023A104", 0x8023A128): (CONT_PADS[0], CONT_PADS[3]),  # clears the pads
    ("func_8023A104", 0x8023A168): (CONT_RING_AT[0], CONT_RING_AT[3]),  # a step of every controller's ring
    ("func_8023A26C", 0x8023A278): (CONT_RINGS[0], CONT_RINGS[3]),  # clears the rings
}

# (first address, end address, new address, capacity at the new address); build_moves() fills it in.
MOVES = []

# Arrays of the results screen overlay (ovl3_7) for 4 bombers. Other overlays have other data at these
# addresses, so only the functions with this prefix move. patches/eightslots.c: RESULTS_LIST.
RESULTS_PREFIX = "func_ovl3_7_"
RESULTS_MOVES = [
    (0x80046E30, 0x80046E80, 0x80707C00, MAX_PLAYERS * 0x14),  # 5 star models per bomber
    (0x80046E80, 0x80046E98, 0x80707CA0, (MAX_PLAYERS + 2) * 4),  # bomber models, 2 more on the winner screen
    (0x80046F20, 0x80046F30, 0x80707CD0, MAX_PLAYERS * 4),  # the slots shown
]


def build_moves():
    """Lays out the moved data. All of it stays below RELOCATED_BASE + 0x8000, so one lui reaches it all."""
    cursor = RELOCATED_BASE

    def place(start, end, capacity):
        nonlocal cursor
        MOVES.append((start, end, cursor, capacity))
        cursor += (capacity + 15) & ~15

    place(0x800AEDF0, 0x800AF1D8, MAX_PLAYERS * PLAYER_SIZE)  # g_Players
    place(0x800BCFE8, 0x800BD5B0, MAX_PLAYERS * AI_STATE_SIZE)  # the CPU players' state
    place(0x802ACF08, 0x802ACF58, MAX_PLAYERS * COMMAND_SIZE)  # g_ComputerPlayerMovement
    MOVES.append((0x802A0B98, 0x802A0BA8, SLOT_NUMBERS, 4 * MAX_PLAYERS))
    MOVES.extend(TEAM_MOVES)
    MOVES.extend([CONT_PADS, CONT_RINGS, CONT_RING_AT])
    place(RIDE_TABLE[0], RIDE_TABLE[1], RIDE_TABLE_ENTRIES * RIDE_ENTRY_SIZE)
    place(0x802ACE90, 0x802ACEA4, BLOCK_ARRAY_SIZE)
    place(0x802AC858, 0x802AC868, BLOCK_ARRAY_SIZE)  # how each player died
    place(0x802AC868, 0x802AC880, BLOCK_ARRAY_SIZE)
    # Config block + 0xB4, flags per slot (also slot_flag_patches): the wins of slots 4 to 7
    # take its place, after those of 0 to 3 (+ 0xA4), so code indexing the wins by slot works for 8.
    place(SLOT_FLAGS, SLOT_FLAGS + 0x10, BLOCK_ARRAY_SIZE)
    # A vector (3 floats) per player, used by the player update (func_8024E088). Slots 4 to 7 wrote past
    # it into the computer players' speeds (0x802ACFB0) and difficulty (0x802ACFC4), and all of them stopped.
    place(0x802ACF68, 0x802ACF98, MAX_PLAYERS * 0xC)

    used = set()
    indexed = set()
    for name, instructions in load_funcs().items():
        if "secure_call" in name:
            continue
        for r in resolve(instructions)[0]:
            if SLOT_BLOCK[0] <= r.target < SLOT_BLOCK[1]:
                used.add(r.target)
                if r.indexed:
                    indexed.add(r.target)
    starts = sorted(used)
    if starts[0] != SLOT_BLOCK[0]:
        raise ValueError(f"the block's first use is {starts[0]:08X}")
    for start, end in zip(starts, starts[1:] + [SLOT_BLOCK[1]]):
        place(start, end, max(end - start, BLOCK_ARRAY_SIZE if start in indexed else 0))

    # Counters per player (7 halfwords) that func_802797BC and func_8027B0BC count up when a thrown object hits a
    # bomber. Room for 5; slots 5 to 7 wrote into the enemy kind table after it (0x800BA8E8: type and model of
    # the team crystals) and the first enemy record, so crystals broke and team battles crashed (func_80285F8C).
    # Placed last, so that the places above stay where netplay.cpp's UNSYNCED_WORDS expects them.
    place(HIT_COUNTS[0], HIT_COUNTS[1], MAX_PLAYERS * 0xE)

    if cursor > RESULTS_MOVES[0][2]:
        raise ValueError(f"moved data ends at {cursor:08X}, in the way of RESULTS_MOVES")
    print(f"moved data: {len(MOVES)} pieces, {cursor - RELOCATED_BASE:#x} bytes at {RELOCATED_BASE:08X}")
    with open(os.path.join(HERE, "moves.txt"), "w") as f:
        for start, end, new, capacity in MOVES:
            f.write(f"{start:08X}-{end:08X} -> {new:08X} capacity {capacity:#x}\n")


def read_sections():
    """Returns function name -> (section ram, section rom), since overlays share their vram."""
    tables = {}
    placement = {}
    current = None
    with open(OVERLAYS) as f:
        for line in f:
            m = FUNCTION_TABLE.search(line)
            if m:
                current = tables.setdefault(m.group(1), [])
                continue
            m = SECTION.search(line)
            if m:
                placement[m.group(4)] = (int(m.group(2), 16), int(m.group(1), 16))
                continue
            m = FUNCTION_ENTRY.search(line)
            if m and current is not None:
                current.append(m.group(1))
    return {func: placement[name] for name, funcs in tables.items() if name in placement for func in funcs}


class Patcher:
    def __init__(self):
        with open(ROM, "rb") as f:
            self.rom = f.read()
        self.sections = read_sections()
        self.patches = []
        self.taken = {}

    def word_at(self, func, vram):
        ram, rom = self.sections[func]
        return struct.unpack(">I", self.rom[rom + vram - ram:rom + vram - ram + 4])[0]

    def patch(self, func, vram, expected, immediate):
        found = self.word_at(func, vram)
        if found != expected:
            raise ValueError(f"{func} {vram:08X}: expected {expected:08X}, the ROM has {found:08X}")
        self.set_immediate(func, vram, immediate)

    def replace(self, func, vram, expected, value):
        found = self.word_at(func, vram)
        if found != expected:
            raise ValueError(f"{func} {vram:08X}: expected {expected:08X}, the ROM has {found:08X}")
        self.taken[(func, vram)] = value
        self.patches.append((func, vram, value))

    def set_immediate(self, func, vram, immediate):
        """The one way an instruction changes here; two changes of one instruction must agree."""
        value = (self.word_at(func, vram) & 0xFFFF0000) | (immediate & 0xFFFF)
        key = (func, vram)
        if self.taken.get(key, value) != value:
            raise ValueError(f"{func} {vram:08X}: two different patches")
        if key not in self.taken:
            self.taken[key] = value
            self.patches.append((func, vram, value))


def lui(reg, value):
    return 0x3C000000 | (reg << 16) | value


def heap_patches(p):
    """func_80225B00 is sbrk and refuses to grow past 0x80300000 + 1 MB."""
    at = 1
    p.patch("func_80225B00", 0x80225B1C, lui(at, 0x10), HEAP_SIZE_MB * 0x10)


# (function, vram, old immediate, new immediate) for single values.
VALUE_PATCHES = [
    # Stage Select's last stage: 5, raised to 7 and 9 by unlocks in the save; all 10 battle stages from the start.
    ("func_ovl3_3_8004386C", 0x80043998, 5, 9),
    # Team results: the medals of earlier wins for teams 3 and 4 too.
    ("func_ovl3_7_8004400C", 0x80044718, 2, 4),
]


def value_patches(p):
    for func, vram, old, new in VALUE_PATCHES:
        if p.word_at(func, vram) & 0xFFFF != old:
            raise ValueError(f"{func} {vram:08X}: expected {old}")
        p.set_immediate(func, vram, new)


def limit_patches(p):
    """Replaces counts in the code: (function, vram of the instruction holding the count, its old value)."""
    for limits, new in ((SLOT_LIMITS, MAX_PLAYERS), (RIDE_TABLE_LIMITS, RIDE_TABLE_ENTRIES),
                        (HIGHEST_SLOT_LIMITS, MAX_PLAYERS - 1)):
        for func, vram, old in limits:
            word = p.word_at(func, vram)
            if word & 0xFFFF != old:
                raise ValueError(f"{func} {vram:08X}: expected the limit {old}, found {word & 0xFFFF}")
            p.set_immediate(func, vram, new)


def model_patches(p):
    """g_SpawnPlayer's switch only knows slots 0 to 3 and otherwise keeps a stale stack value as the model
    file. Worked out from the slot instead (the switch still overrides it for 0 to 3): slot 4 asks for 0x7F00,
    a recolored model that patches/eightslots.c makes."""
    lw_s1_0x38_sp = 0x8FB10038
    addiu_s1_s3_recolored = 0x26710000 | (0x7F00 - 4)
    for vram in (0x80245B0C, 0x80245B64):
        p.replace("g_SpawnPlayer", vram, lw_s1_0x38_sp, addiu_s1_s3_recolored)


SLOT_FLAGS = 0x802AC714  # config block + 0xB4
NOP = 0


def slot_flag_patches(p):
    """func_8023CA94 reaches the per-slot flags at config block + 0xB4 through its loop pointer s2 (block +
    4 * slot), which the relocation cannot follow. Each access becomes a nop and a hook doing it at the
    flags' new place: (vram, the instruction, the hook's place, C for the access with `flag` as the word)."""
    lw_t7, sw_t8, lw_t9, sw_t6 = 0x8E4F00B4, 0xAE5800B4, 0x8E5900B4, 0xAE4E00B4
    accesses = [
        (0x8023CAEC, lw_t7, 0x8023CAEC, "ctx->r15 = (int32_t)flag;"),
        (0x8023CB10, sw_t8, 0x8023CB10, "flag = (int32_t)ctx->r24;"),
        (0x8023CB24, lw_t9, 0x8023CB24, "ctx->r25 = (int32_t)flag;"),
        # A branch's delay slot: the hook goes on the branch, after t6 was set.
        (0x8023CB34, sw_t6, 0x8023CB30, "flag = (int32_t)ctx->r14;"),
    ]
    shift = new_address(SLOT_FLAGS) - SLOT_FLAGS
    for vram, word, hook_vram, access in accesses:
        p.replace("func_8023CA94", vram, word, NOP)
        text = access.replace("flag", f"MEM_W(0xB4, ctx->r18 + 0x{shift:X})")
        HOOKS.append(("func_8023CA94", hook_vram, "{ " + text + " }"))


def new_address(address):
    for start, end, new_start, _ in MOVES:
        if start <= address < end:
            return new_start + address - start
    return None


def foreign_bounds(func, instructions, refs):
    """Uses of a moved piece's start as the end of a loop through other data: those are not that piece."""
    starts = {start for start, _, _, _ in MOVES}
    return {use for use, bound, pointer in loop_bounds(func, instructions, refs)
            if bound in starts and new_address(pointer) is None}


def new_target(func, ref, keep=frozenset()):
    """Where a reference goes. An address just past a moved array is usually the next variable, so it
    stays unless LOOP_BOUNDS says that this reference is a loop's end."""
    if ref.use_vram in keep:
        return None
    if func.startswith(RESULTS_PREFIX):
        for start, end, new, _ in RESULTS_MOVES:
            if start <= ref.target < end:
                return new + ref.target - start
    if (func, ref.target) in BASES_BEFORE_ARRAY:
        steps = BASES_BEFORE_ARRAY[(func, ref.target)]
        return new_address(ref.target + steps * PLAYER_SIZE) - steps * PLAYER_SIZE
    if (func, ref.use_vram) in LOOP_BOUNDS:
        array, size = LOOP_BOUNDS[(func, ref.use_vram)]
        return new_address(array) + size
    return new_address(ref.target)


def high_half(address):
    return (address + 0x8000) >> 16 & 0xFFFF


def connected_groups(refs):
    """A lui can serve several uses and a use can come from several lui: those must move together."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for r in refs:
        parent[find(("lui", r.lui_vram))] = find(("use", r.use_vram))
    groups = {}
    for r in refs:
        groups.setdefault(find(("lui", r.lui_vram)), []).append(r)
    return groups.values()


def relocation_patches(p):
    """Moves every reference into MOVES by rewriting its lui and its low half."""
    moved = kept = 0
    report = []
    for name, instructions in load_funcs().items():
        if "secure_call" in name:
            continue
        refs, _ = resolve(instructions)
        if not any(new_target(name, r) is not None for r in refs):
            continue
        keep = foreign_bounds(name, instructions, refs)
        kept += len(keep)
        for group in connected_groups(refs):
            new = [new_target(name, r, keep) for r in group]
            if all(n is None for n in new):
                continue
            first_lui = min(r.lui_vram for r in group)
            if any(n is None for n in new):
                inside = sorted({r.target for r, n in zip(group, new) if n is not None})
                outside = sorted({r.target for r, n in zip(group, new) if n is None})
                report.append(f"{name} lui {first_lui:08X}: moves {[hex(a) for a in inside][:3]}, keeps {[hex(a) for a in outside][:3]}")
                continue
            highs = {high_half(n) for n in new}
            if len(highs) != 1:
                report.append(f"{name} lui {first_lui:08X}: new addresses need different lui: {[hex(n) for n in new][:4]}")
                continue
            high = highs.pop()
            for r, n in zip(group, new):
                # The ROM must hold what the analysis read.
                if p.word_at(name, r.lui_vram) & 0xFFFF != high_half(r.target):
                    raise ValueError(f"{name} {r.lui_vram:08X}: lui does not match {r.target:08X}")
                if p.word_at(name, r.use_vram) & 0xFFFF != r.target & 0xFFFF:
                    raise ValueError(f"{name} {r.use_vram:08X}: low half does not match {r.target:08X}")
                p.set_immediate(name, r.lui_vram, high)
                p.set_immediate(name, r.use_vram, n)
                moved += 1
    print(f"relocated references: {moved}, loop ends left in place: {kept}, conflicts: {len(report)}")
    with open(os.path.join(HERE, "relocation_report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    return len(report)


def scan_data_pointers(rom):
    """Pointers stored in ROM data cannot be found by reading code."""
    found = []
    for i in range(0, len(rom) - 3, 4):
        word = struct.unpack(">I", rom[i:i + 4])[0]
        if new_address(word) is not None:
            found.append((i, word))
    print(f"words in the ROM that look like pointers into MOVES: {len(found)}")
    for rom_address, word in found[:12]:
        print(f"  rom {rom_address:08X}: {word:08X}")


def write_data_symbols():
    """The game's C patches find its data by name, so the symbol file moves along."""
    os.makedirs(GENERATED, exist_ok=True)

    def shifted(m):
        new = new_address(int(m.group(1), 16))
        return m.group(0) if new is None else f"vram = 0x{new:08X}"

    with open(os.path.join(ROOT, "BM64Syms", "data_dump.toml")) as f:
        text = SYMBOL_ADDRESS.sub(shifted, f.read())
    with open(os.path.join(GENERATED, "data_dump.toml"), "w", newline="\n") as f:
        f.write(text)


def write_rom_table(p):
    """The runtime rebuilds hooked functions from the ROM, so src/main/main.cpp applies these to its copy.
    Overlays are left out: they are compressed in the ROM, and hooks cannot reach them anyway."""
    entries = []
    for func, vram, value in p.patches:
        ram, rom = p.sections[func]
        if not func.startswith("func_ovl"):
            entries.append(f"    {{ 0x{rom + vram - ram:08X}, 0x{value:08X} }},")
    os.makedirs(GENERATED, exist_ok=True)
    with open(os.path.join(GENERATED, "rom_patches.inc"), "w", newline="\n") as f:
        f.write("// Generated by tools/eightslots/gen_patches.py: { ROM offset, big-endian word }\n")
        f.write("\n".join(entries) + "\n")


def main():
    build_moves()
    p = Patcher()
    heap_patches(p)
    model_patches(p)
    limit_patches(p)
    value_patches(p)
    slot_flag_patches(p)
    conflicts = relocation_patches(p)
    scan_data_pointers(p.rom)
    write_data_symbols()
    write_rom_table(p)
    if conflicts:
        print("Conflicts stop the build: see relocation_report.txt")
        return 1

    with open(BASE_CONFIG, newline="") as f:
        base = f.read().replace("\r\n", "\n")
    lines = [base.rstrip("\n"), "", "# Generated by tools/eightslots/gen_patches.py, do not edit.", ""]
    for func, vram, value in p.patches:
        lines += ["[[patches.instruction]]", f'func = "{func}"', f"vram = 0x{vram:08X}", f"value = 0x{value:08X}", ""]
    for func, vram, text in HOOKS:
        lines += ["[[patches.hook]]", f'func = "{func}"', f"before_vram = 0x{vram:08X}", f'text = """{text}"""', ""]
    with open(OUT_CONFIG, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"{len(p.patches)} patches written to {os.path.basename(OUT_CONFIG)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
