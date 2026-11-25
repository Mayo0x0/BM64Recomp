// Custom battle levels (include/level_editor.h): level files, the RDRAM data for the game side (patches/level.c) and
// the level editor session. The game thread calls game_frame at every controller poll; everything else runs on the
// UI thread. The mutex guards what both use.
#include "level_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>

#include "json/json.hpp"
#include "netplay.h"
#include "restart.h"
#include "zelda_config.h"

const char* const levels::stage_names[LEVEL_STAGES] = {
    "Stage 1", "Stage 2", "Stage 3 (Pyramid)", "Stage 4 (Greedy Trap)", "Stage 5",
    "Stage 6", "Stage 7", "Stage 8", "Stage 9", "Stage 10",
};

namespace {

constexpr uint32_t RDRAM_BASE = 0x80000000u;
constexpr uint32_t CAMERA0 = 0x800BD5B0u;       // func_8022787C: 4 cameras of 0x70, viewport at + 0, view matrix at + 0x18
constexpr uint32_t PROJECTION = 0x802A53D8u;    // its projection (an RSP matrix, as the view matrix)
constexpr float TILE = 100.0f;

const char* const kind_names[TILE_KINDS] = { "free", "random", "soft", "hard" };
const std::pair<int, const char*> item_names[] = {
    { ITEM_FIRE, "fire" }, { ITEM_BOMB, "bomb" }, { ITEM_HEART, "heart" },
    { ITEM_POWER, "power" }, { ITEM_SKULL, "skull" }, { ITEM_REMOTE, "remote" },
};
const char* const rule_names[6] = { "minutes", "suddenDeath", "ghosts", "bombs", "fire", "randomBlocks" };

// What the game thread copies of the game for the editor (picking, labels, tile texts).
struct Snapshot {
    bool ready = false;
    int layers = 0;
    float heights[8]{};
    float view[16]{};
    float projection[16]{};
    int16_t viewport[8]{};
    std::vector<uint8_t> grid;      // as in RDRAM (32-bit words in host order)
    std::vector<uint8_t> original;
};

struct State {
    std::mutex mutex;
    bool initialized = false;
    // RDRAM data waiting for the game thread.
    std::vector<uint32_t> words;
    bool dirty = true;
    uint32_t revision = 0;
    bool session = false;            // netplay: the session's data replaces the local levels
    std::vector<uint32_t> session_words;
    // Editor.
    bool editor = false;
    std::string editor_file;
    levels::Level level;
    std::vector<levels::Level> undo, redo;
    bool stroke_changed = false;
    std::set<uint32_t> stroke_columns; // columns a Raise / Lower stroke already moved
    uint32_t look_source = LEVEL_NONE;  // the column LookPaint gives its look
    bool changed = false;
    int symmetry = 0;
    uint32_t cursor = LEVEL_NONE;
    Snapshot snap;
} s;

uint32_t key(int x, int layer, int z) {
    return LEVEL_PACK(x, layer, z, 0);
}

std::filesystem::path active_path() {
    return levels::folder() / "active.json";
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.good()) return nlohmann::json();
    return nlohmann::json::parse(file, nullptr, false);
}

std::map<int, std::string> read_active() {
    std::map<int, std::string> active;
    nlohmann::json json = read_json(active_path());
    if (!json.is_object()) return active;
    for (auto& [stage, file] : json.items()) {
        int index = std::atoi(stage.c_str());
        if (index >= 0 && index < LEVEL_STAGES && file.is_string()) active[index] = file.get<std::string>();
    }
    return active;
}

// Players 5 to 8 (players.json in the app folder): absent until Player Select chose them once.
struct ExtraPlayers {
    bool have = false;
    int32_t values[12]{}; // types, menu levels, teams
};

std::filesystem::path extra_path() {
    return zelda64::get_app_folder_path() / "players.json";
}

const ExtraPlayers& extra_players() {
    static ExtraPlayers extra;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        nlohmann::json json = read_json(extra_path());
        const char* keys[3] = { "types", "levels", "teams" };
        bool ok = json.is_object();
        for (int k = 0; k < 3 && ok; k++) {
            ok = json.contains(keys[k]) && json[keys[k]].is_array() && json[keys[k]].size() == 4;
            for (int i = 0; i < 4 && ok; i++) {
                ok = json[keys[k]][i].is_number_integer();
                if (ok) extra.values[k * 4 + i] = json[keys[k]][i];
            }
        }
        for (int i = 0; i < 4 && ok; i++) {
            ok = extra.values[i] >= 0 && extra.values[i] <= 2 && extra.values[4 + i] >= 0 && extra.values[4 + i] <= 4 &&
                 extra.values[8 + i] >= 0 && extra.values[8 + i] <= 3;
        }
        extra.have = ok;
    }
    return extra;
}

ExtraPlayers& extra_players_mut() {
    return const_cast<ExtraPlayers&>(extra_players());
}

void put_extra_players(std::vector<uint32_t>& words) {
    const ExtraPlayers& extra = extra_players();
    if (!extra.have) return; // the game keeps its defaults (players 5 to 8 off)
    words[LH_EXTRA] = EXTRA_MAGIC;
    for (int i = 0; i < 12; i++) words[LH_EXTRA_TYPES + i] = (uint32_t)extra.values[i];
}

// One stage's record (include/level_layout.h).
void append_record(std::vector<uint32_t>& words, const levels::Level& level) {
    for (int rule : level.rules) words.push_back((uint32_t)rule);
    for (uint32_t start : level.starts) words.push_back(start);
    size_t tiles = std::min<size_t>(level.tiles.size(), LEVEL_MAX_TILES);
    size_t items = std::min<size_t>(level.items.size(), LEVEL_MAX_ITEMS);
    std::vector<uint32_t> shape;
    for (auto& [at, column] : level.shape) {
        if (shape.size() + column.size() > LEVEL_MAX_SHAPE) break;
        for (size_t l = 0; l < column.size() && l < 8; l++) {
            shape.push_back(LEVEL_PACK(LEVEL_X(at), l, LEVEL_Z(at), column[l]));
        }
    }
    size_t looks = std::min<size_t>(level.looks.size(), LEVEL_MAX_LOOKS);
    words.push_back((uint32_t)tiles);
    words.push_back((uint32_t)items);
    words.push_back((uint32_t)shape.size());
    words.push_back((uint32_t)looks);
    size_t n = 0;
    for (auto& [at, kind] : level.tiles) {
        if (n++ == tiles) break;
        words.push_back(at | (uint32_t)kind << 24);
    }
    n = 0;
    for (auto& [at, item] : level.items) {
        if (n++ == items) break;
        words.push_back(at | (uint32_t)item << 24);
    }
    words.insert(words.end(), shape.begin(), shape.end());
    n = 0;
    for (auto& [at, from] : level.looks) {
        if (n++ == looks) break;
        words.push_back(LOOK_PACK(LEVEL_X(at), LEVEL_Z(at), LEVEL_X(from), LEVEL_Z(from)));
    }
}

std::vector<uint32_t> build(const std::vector<const levels::Level*>& list, bool editor) {
    std::vector<uint32_t> words(LH_WORDS, 0);
    put_extra_players(words);
    if (list.empty()) return words; // no magic: the game plays the stages as they are
    words[LH_MAGIC] = LEVEL_MAGIC;
    words[LH_EDITOR] = editor ? 1 : 0;
    words[LH_EDIT_STAGE] = editor ? (uint32_t)list[0]->stage : 0;
    words[LH_CURSOR] = LEVEL_NONE;
    for (const levels::Level* level : list) {
        if (level->stage < 0 || level->stage >= LEVEL_STAGES || words[LH_OFFSETS + level->stage] != 0) continue;
        size_t at = words.size();
        std::vector<uint32_t> record;
        append_record(record, *level);
        if ((at + record.size()) * 4 > LEVEL_AREA_SIZE) continue;
        words[LH_OFFSETS + level->stage] = (uint32_t)(at * 4);
        words.insert(words.end(), record.begin(), record.end());
    }
    return words;
}

// The active levels, as the game should see them (offline, or what a netplay host sends).
std::vector<uint32_t> build_active() {
    std::vector<levels::Level> loaded;
    for (auto& [stage, file] : read_active()) {
        levels::Level level;
        if (levels::load(file, level) && level.stage == stage) loaded.push_back(level);
    }
    std::vector<const levels::Level*> list;
    for (const levels::Level& level : loaded) list.push_back(&level);
    return build(list, false);
}

void init_locked() {
    if (s.initialized) return;
    s.initialized = true;
    const char* edit = std::getenv("BM64_LEVEL_EDIT");
    if (edit != nullptr && *edit != '\0' && levels::load(edit, s.level)) {
        s.editor = true;
        s.editor_file = edit;
        s.words = build({ &s.level }, true);
    } else {
        s.words = build_active();
    }
    s.dirty = true;
}

void rebuild_editor_locked() {
    s.words = build({ &s.level }, true);
    s.dirty = true;
    s.changed = true;
}

// RDRAM access: words are kept in host order at their address.
uint32_t& word(uint8_t* rdram, uint32_t address) {
    return *(uint32_t*)(rdram + (address - RDRAM_BASE));
}

float word_float(uint8_t* rdram, uint32_t address) {
    float f;
    uint32_t w = word(rdram, address);
    std::memcpy(&f, &w, 4);
    return f;
}

// An RSP matrix (Mtx): 16 signed integer halves, then 16 fraction halves, row by row.
void read_mtx(uint8_t* rdram, uint32_t address, float* out) {
    for (int i = 0; i < 16; i++) {
        int16_t whole = *(int16_t*)(rdram + (((address + i * 2) ^ 2) - RDRAM_BASE));
        uint16_t fraction = *(uint16_t*)(rdram + (((address + 32 + i * 2) ^ 2) - RDRAM_BASE));
        out[i] = (float)(((int32_t)whole * 65536 + fraction) / 65536.0);
    }
}

// A copy of a grid: offsets from its (4-aligned) start, halfwords swapped as in RDRAM.
uint32_t copy_word(const std::vector<uint8_t>& grid, uint32_t offset) {
    return offset + 4 <= grid.size() ? *(const uint32_t*)(grid.data() + offset) : 0;
}

uint32_t copy_half(const std::vector<uint8_t>& grid, uint32_t offset) {
    return offset + 2 <= grid.size() ? *(const uint16_t*)(grid.data() + (offset ^ 2)) : 0;
}

uint32_t copy_byte(const std::vector<uint8_t>& grid, uint32_t offset) {
    return offset < grid.size() ? grid[offset ^ 3] : 0;
}

// func_8026D7D8 on a copy: the tile, or 0x2010 off the grid.
uint32_t tile_of(const std::vector<uint8_t>& grid, int x, int layer, int z) {
    if (grid.empty()) return 0x2010;
    int layers = (int)copy_byte(grid, 0);
    int chunks_x = (int)copy_word(grid, 4), chunks_z = (int)copy_word(grid, 8);
    if (layer < 0 || layer >= layers || x < 0 || z < 0 || x >= chunks_x * 8 || z >= chunks_z * 8) return 0x2010;
    uint32_t chunk = (uint32_t)((z >> 3) * chunks_x + (x >> 3));
    return copy_half(grid, layer * GRID_LAYER_SIZE + chunk * 0x82 + 0x14 + ((z & 7) * 8 + (x & 7)) * 2);
}

int grid_layers(const std::vector<uint8_t>& grid) {
    return std::min((int)copy_byte(grid, 0), 8);
}

// A column's kind per layer as the stage has it, and as the level shapes it.
std::vector<uint8_t> own_column(int x, int z) {
    std::vector<uint8_t> column((size_t)grid_layers(s.snap.original));
    for (int l = 0; l < (int)column.size(); l++) column[l] = (uint8_t)(tile_of(s.snap.original, x, l, z) & 0xF);
    return column;
}

std::vector<uint8_t> column_of(int x, int z) {
    auto it = s.level.shape.find(key(x, 0, z));
    return it != s.level.shape.end() ? it->second : own_column(x, z);
}

int shaped_kind(int x, int layer, int z) {
    if (x < 0 || z < 0 || x > 0xFF || z > 0xFF) return SHAPE_AIR;
    std::vector<uint8_t> column = column_of(x, z);
    return layer >= 0 && layer < (int)column.size() ? column[layer] : SHAPE_AIR;
}

bool is_ramp(int kind) {
    return kind >= SHAPE_RAMP_PZ && kind <= SHAPE_RAMP_NX;
}

// The top floor or ramp of a column, or -1.
int top_of(const std::vector<uint8_t>& column) {
    for (int l = (int)column.size() - 1; l >= 0; l--) {
        if (column[l] == SHAPE_FLOOR || is_ramp(column[l])) return l;
    }
    return -1;
}

int top_floor_of(const std::vector<uint8_t>& column) {
    for (int l = (int)column.size() - 1; l >= 0; l--) {
        if (column[l] == SHAPE_FLOOR) return l;
    }
    return -1;
}

// Columns of the stage itself (not the air around it) take shape tools.
bool shape_editable(int x, int z) {
    if (x < 0 || z < 0 || x > 0xFF || z > 0xFF) return false;
    for (uint8_t kind : own_column(x, z)) {
        if (kind != SHAPE_AIR) return true;
    }
    return false;
}

// Floor tiles (as the level shapes the stage) take the block, item and start tools.
bool editable(int x, int layer, int z) {
    return shaped_kind(x, layer, z) == SHAPE_FLOOR;
}

// What a floor tile has before the level's kinds: the stage's own, none on a floor the shape made (patches/level.c).
int original_kind(int x, int layer, int z) {
    uint32_t tile = tile_of(s.snap.original, x, layer, z);
    if ((tile & 0xF) != SHAPE_FLOOR) return TILE_FREE;
    if (tile & 0x60) return TILE_SOFT;
    return (tile & 0x10) ? TILE_FREE : TILE_RANDOM;
}

int kind_at(int x, int layer, int z) {
    auto it = s.level.tiles.find(key(x, layer, z));
    return it != s.level.tiles.end() ? it->second : original_kind(x, layer, z);
}

// The stage's own start of slot 0 to 3 once the level's shape is applied: on the top floor of its column when its
// tile is no floor any more (patches/level.c apply_shape), LEVEL_NONE without one.
uint32_t own_start(int slot) {
    uint32_t code = (uint32_t)(slot + 1) << 7;
    int layers = grid_layers(s.snap.original);
    int w = (int)copy_word(s.snap.original, 4) * 8, d = (int)copy_word(s.snap.original, 8) * 8;
    for (int l = 0; l < layers; l++) {
        for (int z = 0; z < d; z++) {
            for (int x = 0; x < w; x++) {
                if ((tile_of(s.snap.original, x, l, z) & 0x380) != code) continue;
                std::vector<uint8_t> column = column_of(x, z);
                if (column[l] == SHAPE_FLOOR) return key(x, l, z);
                int top = top_floor_of(column);
                return top < 0 ? LEVEL_NONE : key(x, top, z);
            }
        }
    }
    return LEVEL_NONE;
}

// The slot whose start is on this tile (the level's, or the stage's own for slots 0 to 3), or -1.
int start_at(int x, int layer, int z) {
    uint32_t at = key(x, layer, z);
    for (int slot = 0; slot < 8; slot++) {
        if (s.level.starts[slot] == at) return slot;
    }
    for (int slot = 0; slot < 4; slot++) {
        if (s.level.starts[slot] == LEVEL_NONE && own_start(slot) == at) return slot;
    }
    return -1;
}

// A start (the level's or the stage's own) somewhere in the column.
bool column_has_start(int x, int z) {
    for (int slot = 0; slot < 8; slot++) {
        uint32_t at = s.level.starts[slot] != LEVEL_NONE ? s.level.starts[slot] : slot < 4 ? own_start(slot) : LEVEL_NONE;
        if (at != LEVEL_NONE && LEVEL_X(at) == x && LEVEL_Z(at) == z) return true;
    }
    return false;
}

// Floor bounds over all layers: the middle that the mirror tools use.
void floor_bounds(int& min_x, int& max_x, int& min_z, int& max_z) {
    min_x = min_z = 1 << 30;
    max_x = max_z = -1;
    int layers = (int)copy_byte(s.snap.original, 0);
    int width = (int)copy_word(s.snap.original, 4) * 8, depth = (int)copy_word(s.snap.original, 8) * 8;
    for (int l = 0; l < std::min(layers, 8); l++) {
        for (int z = 0; z < depth; z++) {
            for (int x = 0; x < width; x++) {
                if (editable(x, l, z)) {
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_z = std::min(min_z, z);
                    max_z = std::max(max_z, z);
                }
            }
        }
    }
}

// 4x4 helpers (row vectors, as the N64's float matrices: v' = v * M).
void multiply(const float* a, const float* b, double* out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            for (int k = 0; k < 4; k++) sum += (double)a[i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = sum;
        }
    }
}

bool invert(const double* m, double* inv) {
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        }
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(a[c][j], a[pivot][j]);
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) inv[i * 4 + j] = a[i][j + 4];
    }
    return true;
}

// The N64 viewport in pixels, and how big an N64 pixel is in the window (the picture fills the height).
struct ScreenMap {
    double cx, cy, hx, hy, scale;
};

ScreenMap screen_map(float width, float height) {
    const int16_t* vp = s.snap.viewport;
    ScreenMap m;
    m.hx = vp[0] / 4.0;
    m.hy = vp[1] / 4.0;
    m.cx = vp[4] / 4.0;
    m.cy = vp[5] / 4.0;
    if (m.hx <= 0 || m.hy <= 0 || m.cy <= 0) {
        m.hx = m.cx = 160.0;
        m.hy = m.cy = 120.0;
    }
    m.scale = height / (2.0 * m.cy);
    (void)width;
    return m;
}

bool project(float wx, float wy, float wz, float width, float height, float& out_x, float& out_y) {
    double m[16];
    multiply(s.snap.view, s.snap.projection, m);
    double v[4] = { wx, wy, wz, 1.0 }, c[4];
    for (int j = 0; j < 4; j++) c[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    if (c[3] <= 1e-6) return false;
    ScreenMap map = screen_map(width, height);
    double sx = map.cx + c[0] / c[3] * map.hx;
    double sy = map.cy - c[1] / c[3] * map.hy;
    out_x = (float)(width / 2.0 + (sx - map.cx) * map.scale);
    out_y = (float)(height / 2.0 + (sy - map.cy) * map.scale);
    return true;
}

// The floor tile under a window pixel: the first floor met from the top layer down; walls and ramps stop it.
bool pick(float px, float py, float width, float height, uint32_t& out) {
    if (!s.snap.ready || s.snap.grid.empty()) return false;
    double m[16], inv[16];
    multiply(s.snap.view, s.snap.projection, m);
    if (!invert(m, inv)) return false;
    ScreenMap map = screen_map(width, height);
    double sx = map.cx + (px - width / 2.0) / map.scale;
    double sy = map.cy + (py - height / 2.0) / map.scale;
    double nx = (sx - map.cx) / map.hx, ny = -(sy - map.cy) / map.hy;
    double points[2][3];
    for (int k = 0; k < 2; k++) {
        double c[4] = { nx, ny, k == 0 ? -1.0 : 1.0, 1.0 }, w[4];
        for (int j = 0; j < 4; j++) w[j] = c[0] * inv[j] + c[1] * inv[4 + j] + c[2] * inv[8 + j] + c[3] * inv[12 + j];
        if (std::fabs(w[3]) < 1e-9) return false;
        for (int j = 0; j < 3; j++) points[k][j] = w[j] / w[3];
    }
    double d[3] = { points[1][0] - points[0][0], points[1][1] - points[0][1], points[1][2] - points[0][2] };
    if (std::fabs(d[1]) < 1e-9) return false;
    for (int l = std::min(s.snap.layers, 8) - 1; l >= 0; l--) {
        double t = (s.snap.heights[l] - points[0][1]) / d[1];
        if (t < 0) continue;
        int x = (int)std::floor((points[0][0] + t * d[0]) / TILE + 0.5);
        int z = (int)std::floor((points[0][2] + t * d[2]) / TILE + 0.5);
        uint32_t tile = tile_of(s.snap.grid, x, l, z);
        int kind = (int)(tile & 0xF);
        if (kind == SHAPE_FLOOR || is_ramp(kind)) {
            out = key(x, l, z);
            return true;
        }
        if (kind != SHAPE_AIR) return false;
        if (l == 0 && shape_editable(x, z)) { // a hole the shape made
            out = key(x, 0, z);
            return true;
        }
    }
    return false;
}

const char* item_text(int item) {
    switch (item) {
    case ITEM_FIRE: return "Feuer";
    case ITEM_BOMB: return "Bombe";
    case ITEM_HEART: return "Herz";
    case ITEM_POWER: return "Power-Bombe";
    case ITEM_SKULL: return "Totenkopf";
    case ITEM_REMOTE: return "Fernzünder";
    default: return "?";
    }
}

const char* ramp_text(int kind) {
    switch (kind) {
    case SHAPE_RAMP_PZ: return "Rampe, steigt nach vorne";
    case SHAPE_RAMP_NZ: return "Rampe, steigt nach hinten";
    case SHAPE_RAMP_PX: return "Rampe, steigt nach rechts";
    default: return "Rampe, steigt nach links";
    }
}

std::string describe(uint32_t at) {
    int x = LEVEL_X(at), l = LEVEL_LAYER(at), z = LEVEL_Z(at);
    std::string text = "Ebene " + std::to_string(l + 1) + ", Feld " + std::to_string(x) + "/" + std::to_string(z) + ": ";
    std::string shaped = s.level.shape.count(key(x, 0, z)) ? " (Form geändert)" : "";
    auto look = s.level.looks.find(key(x, 0, z));
    if (look != s.level.looks.end()) {
        shaped += " (Optik von Feld " + std::to_string(LEVEL_X(look->second)) + "/" + std::to_string(LEVEL_Z(look->second)) + ")";
    }
    int kind = shaped_kind(x, l, z);
    if (top_of(column_of(x, z)) < 0) return text + "Loch" + shaped;
    if (is_ramp(kind)) return text + ramp_text(kind) + shaped;
    if (!editable(x, l, z)) return text + "nicht änderbar";
    static const char* const names[TILE_KINDS] = { "Boden (nie ein Block)", "Boden (Zufallsblock möglich)",
                                                   "Weicher Block", "Fester Block" };
    text += names[kind_at(x, l, z)];
    auto item = s.level.items.find(at);
    if (item != s.level.items.end()) text += std::string(", Item: ") + item_text(item->second);
    int slot = start_at(x, l, z);
    if (slot >= 0) text += ", Start Spieler " + std::to_string(slot + 1);
    if (s.level.tiles.count(at) || s.level.items.count(at)) text += " (geändert)";
    return text + shaped;
}

// Gives a column a new shape. What stood on a floor tile that is gone goes too; the level's starts in the column
// move to its top floor.
void set_column(int x, int z, const std::vector<uint8_t>& column) {
    uint32_t at = key(x, 0, z);
    if (column == own_column(x, z)) {
        s.level.shape.erase(at);
    } else {
        s.level.shape[at] = column;
    }
    for (int l = 0; l < (int)column.size(); l++) {
        if (column[l] == SHAPE_FLOOR) continue;
        s.level.tiles.erase(key(x, l, z));
        s.level.items.erase(key(x, l, z));
    }
    int top = top_floor_of(column);
    for (uint32_t& start : s.level.starts) {
        if (start == LEVEL_NONE || LEVEL_X(start) != x || LEVEL_Z(start) != z) continue;
        int l = LEVEL_LAYER(start);
        if (l < (int)column.size() && column[l] == SHAPE_FLOOR) continue;
        start = top < 0 ? LEVEL_NONE : key(x, top, z);
    }
}

void apply_shape_tool(int x, int z, levels::editor::Tool tool, int value) {
    using levels::editor::Tool;
    if (!shape_editable(x, z)) return;
    std::vector<uint8_t> column = column_of(x, z);
    int size = (int)column.size(), top = top_of(column);
    switch (tool) {
    case Tool::Raise:
        if (s.stroke_columns.count(key(x, 0, z))) return;
        if (top < 0) {
            column[0] = SHAPE_FLOOR;
        } else if (top + 1 < size) {
            column[top] = SHAPE_SOLID;
            column[top + 1] = SHAPE_FLOOR;
        }
        break;
    case Tool::Lower:
        if (s.stroke_columns.count(key(x, 0, z)) || top < 1) return;
        column[top] = SHAPE_AIR;
        column[top - 1] = SHAPE_FLOOR;
        break;
    case Tool::Ramp:
        if (top < 0 || top + 1 >= size || !is_ramp(value)) return;
        column[top] = (uint8_t)value;
        break;
    case Tool::Flat:
        if (top < 0) return;
        column[top] = SHAPE_FLOOR;
        break;
    case Tool::Hole:
        if (column_has_start(x, z)) return; // a bomber would start in the air
        std::fill(column.begin(), column.end(), (uint8_t)SHAPE_AIR);
        break;
    case Tool::Erase:
        column = own_column(x, z);
        break;
    default:
        return;
    }
    s.stroke_columns.insert(key(x, 0, z));
    set_column(x, z, column);
}

void set_kind(uint32_t at, int kind) {
    int x = LEVEL_X(at), l = LEVEL_LAYER(at), z = LEVEL_Z(at);
    if (kind == original_kind(x, l, z)) {
        s.level.tiles.erase(at);
    } else {
        s.level.tiles[at] = kind;
    }
    if (kind != TILE_SOFT) s.level.items.erase(at);
    // A bomber cannot start in a block or where a random one may fall.
    if (kind != TILE_FREE) {
        for (uint32_t& start : s.level.starts) {
            if (start == at) start = LEVEL_NONE;
        }
    }
}

void apply_one(uint32_t at, levels::editor::Tool tool, int value) {
    using levels::editor::Tool;
    int x = LEVEL_X(at), l = LEVEL_LAYER(at), z = LEVEL_Z(at);
    if (tool == Tool::LookPick || tool == Tool::LookPaint) {
        if (!shape_editable(x, z)) return;
        if (tool == Tool::LookPick) s.look_source = key(x, 0, z);
        else if (s.look_source != LEVEL_NONE) s.level.looks[key(x, 0, z)] = s.look_source;
        return;
    }
    if (levels::editor::shape_tool(tool)) {
        apply_shape_tool(x, z, tool, value);
        return;
    }
    if (!editable(x, l, z)) return;
    switch (tool) {
    case Tool::Free: set_kind(at, TILE_FREE); break;
    case Tool::Random: set_kind(at, TILE_RANDOM); break;
    case Tool::Soft: set_kind(at, TILE_SOFT); break;
    case Tool::Hard: set_kind(at, TILE_HARD); break;
    case Tool::Item:
        if (kind_at(x, l, z) != TILE_SOFT) set_kind(at, TILE_SOFT);
        s.level.items[at] = value;
        break;
    case Tool::Start:
        if (value < 0 || value >= 8) break;
        for (uint32_t& start : s.level.starts) {
            if (start == at) start = LEVEL_NONE;
        }
        set_kind(at, TILE_FREE);
        s.level.starts[value] = at;
        break;
    case Tool::Erase:
        s.level.tiles.erase(at);
        s.level.items.erase(at);
        for (uint32_t& start : s.level.starts) {
            if (start == at) start = LEVEL_NONE;
        }
        break;
    }
}

} // namespace

std::filesystem::path levels::folder() {
    std::filesystem::path path = zelda64::get_app_folder_path() / "levels";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

std::vector<levels::LevelFile> levels::list() {
    std::vector<LevelFile> files;
    std::map<int, std::string> active = read_active();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder(), error)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json" || entry.path().filename() == "active.json") continue;
        Level level;
        std::string file = entry.path().filename().string();
        if (!load(file, level)) continue;
        auto it = active.find(level.stage);
        files.push_back({ file, level.name, level.stage, it != active.end() && it->second == file });
    }
    std::sort(files.begin(), files.end(), [](const LevelFile& a, const LevelFile& b) {
        return a.stage != b.stage ? a.stage < b.stage : a.name < b.name;
    });
    return files;
}

bool levels::load(const std::string& file, Level& level) {
    nlohmann::json json = read_json(folder() / file);
    if (!json.is_object()) return false;
    level = Level();
    level.name = json.value("name", level.name);
    level.stage = std::clamp(json.value("stage", 0), 0, LEVEL_STAGES - 1);
    if (json.contains("rules") && json["rules"].is_object()) {
        for (int i = 0; i < 6; i++) level.rules[i] = json["rules"].value(rule_names[i], -1);
    }
    if (json.contains("starts") && json["starts"].is_array()) {
        for (size_t i = 0; i < json["starts"].size() && i < 8; i++) {
            const auto& start = json["starts"][i];
            if (start.is_array() && start.size() == 3) level.starts[i] = key(start[0], start[1], start[2]);
        }
    }
    if (json.contains("tiles") && json["tiles"].is_array()) {
        for (const auto& tile : json["tiles"]) {
            if (!tile.is_array() || tile.size() != 4 || !tile[3].is_string()) continue;
            for (int kind = 0; kind < TILE_KINDS; kind++) {
                if (tile[3] == kind_names[kind]) level.tiles[key(tile[0], tile[1], tile[2])] = kind;
            }
        }
    }
    if (json.contains("items") && json["items"].is_array()) {
        for (const auto& item : json["items"]) {
            if (!item.is_array() || item.size() != 4 || !item[3].is_string()) continue;
            for (auto& [id, name] : item_names) {
                if (item[3] == name) level.items[key(item[0], item[1], item[2])] = id;
            }
        }
    }
    // The shape: [x, z, "FF1..."], a hex digit per layer from the bottom ('.' for air).
    if (json.contains("looks") && json["looks"].is_array()) {
        for (const auto& entry : json["looks"]) {
            if (!entry.is_array() || entry.size() != 4) continue;
            bool ok = true;
            int v[4];
            for (int i = 0; i < 4; i++) {
                ok = ok && entry[i].is_number_integer();
                if (ok) v[i] = entry[i];
                ok = ok && v[i] >= 0 && v[i] <= 0xFF;
            }
            if (ok) level.looks[key(v[0], 0, v[1])] = key(v[2], 0, v[3]);
        }
    }
    if (json.contains("shape") && json["shape"].is_array()) {
        for (const auto& entry : json["shape"]) {
            if (!entry.is_array() || entry.size() != 3 || !entry[0].is_number_integer() || !entry[1].is_number_integer() ||
                !entry[2].is_string()) continue;
            int x = entry[0], z = entry[1];
            std::string text = entry[2];
            if (x < 0 || x > 0xFF || z < 0 || z > 0xFF || text.empty() || text.size() > 8) continue;
            std::vector<uint8_t> column;
            for (char c : text) {
                int kind = c == '.' ? 0 : std::isxdigit((unsigned char)c) ? std::stoi(std::string(1, c), nullptr, 16) : -1;
                if (kind < 0) break;
                column.push_back((uint8_t)kind);
            }
            if (column.size() == text.size()) level.shape[key(x, 0, z)] = column;
        }
    }
    return true;
}

bool levels::save(const std::string& file, const Level& level) {
    nlohmann::json json;
    json["name"] = level.name;
    json["stage"] = level.stage;
    json["version"] = 1;
    nlohmann::json rules = nlohmann::json::object();
    for (int i = 0; i < 6; i++) rules[rule_names[i]] = level.rules[i];
    json["rules"] = rules;
    nlohmann::json starts = nlohmann::json::array();
    for (uint32_t start : level.starts) {
        if (start == LEVEL_NONE) starts.push_back(nullptr);
        else starts.push_back({ LEVEL_X(start), LEVEL_LAYER(start), LEVEL_Z(start) });
    }
    json["starts"] = starts;
    nlohmann::json tiles = nlohmann::json::array();
    for (auto& [at, kind] : level.tiles) tiles.push_back({ LEVEL_X(at), LEVEL_LAYER(at), LEVEL_Z(at), kind_names[kind] });
    json["tiles"] = tiles;
    nlohmann::json items = nlohmann::json::array();
    for (auto& [at, item] : level.items) {
        for (auto& [id, name] : item_names) {
            if (id == item) items.push_back({ LEVEL_X(at), LEVEL_LAYER(at), LEVEL_Z(at), name });
        }
    }
    json["items"] = items;
    nlohmann::json shape = nlohmann::json::array();
    for (auto& [at, column] : level.shape) {
        std::string text;
        for (uint8_t kind : column) text += kind == 0 ? '.' : "0123456789ABCDEF"[kind & 0xF];
        shape.push_back({ LEVEL_X(at), LEVEL_Z(at), text });
    }
    json["shape"] = shape;
    nlohmann::json looks = nlohmann::json::array();
    for (auto& [at, from] : level.looks) looks.push_back({ LEVEL_X(at), LEVEL_Z(at), LEVEL_X(from), LEVEL_Z(from) });
    json["looks"] = looks;
    std::ofstream out(folder() / file);
    if (!out.good()) return false;
    out << json.dump(1);
    return out.good();
}

std::string levels::new_file_name(const std::string& name) {
    std::string base;
    for (char c : name) {
        if (std::isalnum((unsigned char)c)) base += (char)std::tolower((unsigned char)c);
        else if (!base.empty() && base.back() != '_') base += '_';
    }
    if (base.empty()) base = "level";
    std::string file = base + ".json";
    for (int i = 2; std::filesystem::exists(folder() / file); i++) file = base + "_" + std::to_string(i) + ".json";
    return file;
}

bool levels::remove(const std::string& file) {
    set_active(file, false);
    std::error_code error;
    return std::filesystem::remove(folder() / file, error);
}

void levels::set_active(const std::string& file, bool active) {
    Level level;
    if (!load(file, level)) return;
    std::map<int, std::string> current = read_active();
    if (active) {
        current[level.stage] = file;
    } else if (current.count(level.stage) && current[level.stage] == file) {
        current.erase(level.stage);
    }
    nlohmann::json json = nlohmann::json::object();
    for (auto& [stage, name] : current) json[std::to_string(stage)] = name;
    {
        std::ofstream out(active_path());
        out << json.dump(1);
    }
    {
        std::lock_guard lock(s.mutex);
        init_locked();
        if (!s.editor && !s.session) {
            s.words = build_active();
            s.dirty = true;
        }
    }
    netplay::refresh_levels();
}

std::vector<uint8_t> levels::session_data() {
    std::vector<uint32_t> words = build_active();
    std::vector<uint8_t> bytes(LEVEL_AREA_SIZE, 0);
    std::memcpy(bytes.data(), words.data(), std::min<size_t>(words.size() * 4, LEVEL_AREA_SIZE));
    return bytes;
}

void levels::set_session_data(const uint8_t* data, size_t size) {
    std::lock_guard lock(s.mutex);
    init_locked();
    s.session = true;
    s.session_words.assign(LEVEL_AREA_SIZE / 4, 0);
    std::memcpy(s.session_words.data(), data, std::min<size_t>(size, LEVEL_AREA_SIZE));
    s.words = s.session_words;
    s.dirty = true;
}

void levels::save_extra_players(const int32_t* values) {
    if (netplay::in_game()) return; // the host's choice for this session, not this PC's
    ExtraPlayers& extra = extra_players_mut();
    extra.have = true;
    std::copy(values, values + 12, extra.values);
    nlohmann::json json;
    json["types"] = std::vector<int32_t>(values, values + 4);
    json["levels"] = std::vector<int32_t>(values + 4, values + 8);
    json["teams"] = std::vector<int32_t>(values + 8, values + 12);
    std::ofstream out(extra_path());
    out << json.dump(1);
}

void levels::game_frame(uint8_t* rdram) {
    shape_frame(rdram);
    std::lock_guard lock(s.mutex);
    init_locked();
    if (s.dirty) {
        s.dirty = false;
        std::vector<uint32_t> words = s.words;
        words.resize(LEVEL_AREA_SIZE / 4, 0);
        if (words[LH_MAGIC] == LEVEL_MAGIC) words[LH_REVISION] = ++s.revision;
        for (size_t i = 0; i < words.size(); i++) word(rdram, LEVEL_AREA + (uint32_t)i * 4) = words[i];
    }
    if (!s.editor) return;
    word(rdram, LEVEL_AREA + LH_CURSOR * 4) = s.cursor;
    word(rdram, LEVEL_AREA + LH_SYMMETRY * 4) = (uint32_t)s.symmetry;
    Snapshot& snap = s.snap;
    snap.ready = word(rdram, EDITOR_INFO + EI_READY * 4) != 0;
    snap.layers = (int)word(rdram, EDITOR_INFO + EI_LAYERS * 4);
    for (int l = 0; l < 8; l++) snap.heights[l] = word_float(rdram, EDITOR_INFO + (EI_HEIGHTS + l) * 4);
    uint32_t camera = word(rdram, EDITOR_INFO + EI_CAMERA * 4);
    if (camera < CAMERA0 || camera >= CAMERA0 + 4 * 0x70) camera = CAMERA0;
    read_mtx(rdram, camera + 0x18, snap.view);
    read_mtx(rdram, PROJECTION, snap.projection);
    for (int i = 0; i < 8; i++) snap.viewport[i] = *(int16_t*)(rdram + (((camera + i * 2) ^ 2) - RDRAM_BASE));
    snap.grid.assign(rdram + (GRID_ADDRESS - RDRAM_BASE), rdram + (GRID_ADDRESS - RDRAM_BASE) + GRID_SIZE);
    static int debug_frames = 0;
    if (std::getenv("BM64_LEVEL_DEBUG") != nullptr && ++debug_frames % 120 == 0) {
        printf("LEVELDBG ready=%d layers=%d h0=%.1f h1=%.1f vp=%d %d %d %d | %d %d %d %d cursor=%08X\n", snap.ready, snap.layers,
               snap.heights[0], snap.heights[1], snap.viewport[0], snap.viewport[1], snap.viewport[2], snap.viewport[3],
               snap.viewport[4], snap.viewport[5], snap.viewport[6], snap.viewport[7], s.cursor);
        printf("LEVELDBG view"); for (float f : snap.view) printf(" %.3f", f); printf("\n");
        printf("LEVELDBG proj"); for (float f : snap.projection) printf(" %.3f", f); printf("\n");
        fflush(stdout);
    }
    if (snap.ready && snap.original.empty()) {
        snap.original.assign(rdram + (GRID_ORIGINAL - RDRAM_BASE), rdram + (GRID_ORIGINAL - RDRAM_BASE) + GRID_SIZE);
    }
}

bool levels::editor_session() {
    std::lock_guard lock(s.mutex);
    init_locked();
    return s.editor;
}

void levels::start_editor(const std::string& file) {
    restart::relaunch({ { "BM64_RESTART", "1" }, { "BM64_NETPLAY", "" }, { "BM64_LEVEL_EDIT", file } });
}

void levels::leave_editor() {
    restart::relaunch({ { "BM64_RESTART", "" }, { "BM64_NETPLAY", "" }, { "BM64_LEVEL_EDIT", "" } }); // to the launcher
}

bool levels::editor::shape_tool(Tool tool) {
    return tool == Tool::Raise || tool == Tool::Lower || tool == Tool::Ramp || tool == Tool::Flat || tool == Tool::Hole;
}

levels::Level& levels::editor::level() {
    return s.level;
}

const std::string& levels::editor::file() {
    return s.editor_file;
}

levels::editor::Status levels::editor::status() {
    std::lock_guard lock(s.mutex);
    Status status;
    status.ready = s.snap.ready && !s.snap.original.empty();
    status.has_cursor = s.cursor != LEVEL_NONE;
    status.cursor = s.cursor;
    status.dirty = s.changed;
    if (status.ready && status.has_cursor) status.tile_text = describe(s.cursor);
    return status;
}

void levels::editor::hover(float x, float y, float width, float height) {
    std::lock_guard lock(s.mutex);
    uint32_t at;
    if (std::getenv("BM64_LEVEL_DEBUG") != nullptr) {
        bool ok = pick(x, y, width, height, at);
        printf("LEVELDBG hover %.0f,%.0f of %.0fx%.0f -> %d %08X\n", x, y, width, height, ok, ok ? at : 0);
    }
    bool ok = pick(x, y, width, height, at);
    s.cursor = ok && (editable(LEVEL_X(at), LEVEL_LAYER(at), LEVEL_Z(at)) || shape_editable(LEVEL_X(at), LEVEL_Z(at)))
                   ? at : LEVEL_NONE;
}

void levels::editor::leave_view() {
    std::lock_guard lock(s.mutex);
    s.cursor = LEVEL_NONE;
}

void levels::editor::begin_stroke() {
    std::lock_guard lock(s.mutex);
    s.undo.push_back(s.level);
    s.redo.clear();
    s.stroke_changed = false;
    s.stroke_columns.clear();
}

void levels::editor::apply(Tool tool, int value, bool erase) {
    std::lock_guard lock(s.mutex);
    if (s.cursor == LEVEL_NONE || s.snap.original.empty()) return;
    bool shape = shape_tool(tool);
    Tool picked_tool = tool;
    if (erase) tool = Tool::Erase;
    std::set<uint32_t> targets{ s.cursor };
    // Start points are per player: only the tile itself.
    if (s.symmetry != 0 && tool != Tool::Start && tool != Tool::LookPick) {
        int min_x, max_x, min_z, max_z;
        floor_bounds(min_x, max_x, min_z, max_z);
        int x = LEVEL_X(s.cursor), l = LEVEL_LAYER(s.cursor), z = LEVEL_Z(s.cursor);
        int mx = min_x + max_x - x, mz = min_z + max_z - z;
        if (s.symmetry & 1) targets.insert(key(mx, l, z));
        if (s.symmetry & 2) targets.insert(key(x, l, mz));
        if ((s.symmetry & 3) == 3) targets.insert(key(mx, l, mz));
    }
    Level before = s.level;
    for (uint32_t at : targets) {
        int x = LEVEL_X(at), z = LEVEL_Z(at);
        if (x < 0 || x > 0xFF || z < 0 || z > 0xFF) continue;
        if (shape && tool == Tool::Erase) {
            apply_shape_tool(x, z, Tool::Erase, 0);
        } else if (erase && (picked_tool == Tool::LookPaint || picked_tool == Tool::LookPick)) {
            s.level.looks.erase(key(x, 0, z)); // the look the editor picks itself again
        } else {
            apply_one(at, tool, value);
        }
    }
    if (before.tiles != s.level.tiles || before.items != s.level.items || before.starts != s.level.starts ||
        before.shape != s.level.shape || before.looks != s.level.looks) {
        s.stroke_changed = true;
        rebuild_editor_locked();
    }
}

void levels::editor::end_stroke() {
    std::lock_guard lock(s.mutex);
    if (!s.stroke_changed && !s.undo.empty()) s.undo.pop_back();
}

void levels::editor::set_symmetry(int symmetry) {
    std::lock_guard lock(s.mutex);
    s.symmetry = symmetry & 3;
}

int levels::editor::symmetry() {
    std::lock_guard lock(s.mutex);
    return s.symmetry;
}

void levels::editor::undo() {
    std::lock_guard lock(s.mutex);
    if (s.undo.empty()) return;
    s.redo.push_back(s.level);
    s.level = s.undo.back();
    s.undo.pop_back();
    rebuild_editor_locked();
}

void levels::editor::redo() {
    std::lock_guard lock(s.mutex);
    if (s.redo.empty()) return;
    s.undo.push_back(s.level);
    s.level = s.redo.back();
    s.redo.pop_back();
    rebuild_editor_locked();
}

bool levels::editor::can_undo() {
    std::lock_guard lock(s.mutex);
    return !s.undo.empty();
}

bool levels::editor::can_redo() {
    std::lock_guard lock(s.mutex);
    return !s.redo.empty();
}

void levels::editor::set_rule(int rule, int value) {
    std::lock_guard lock(s.mutex);
    if (rule < 0 || rule >= 6 || s.level.rules[rule] == value) return;
    s.level.rules[rule] = value;
    rebuild_editor_locked();
}

void levels::editor::set_name(const std::string& name) {
    std::lock_guard lock(s.mutex);
    if (s.level.name == name) return;
    s.level.name = name;
    s.changed = true;
}

bool levels::editor::save() {
    std::lock_guard lock(s.mutex);
    if (s.editor_file.empty() || !levels::save(s.editor_file, s.level)) return false;
    s.changed = false;
    return true;
}

std::vector<levels::editor::Label> levels::editor::labels(float width, float height) {
    std::lock_guard lock(s.mutex);
    std::vector<Label> out;
    if (!s.snap.ready || s.snap.original.empty()) return out;
    static const uint32_t colors[8] = { 0xFFFFFFFF, 0x404040FF, 0xFF4040FF, 0x4070FFFF,
                                        0x40D050FF, 0xFFE040FF, 0xB050F0FF, 0xFFA030FF };
    auto add = [&](uint32_t at, const std::string& text, uint32_t color) {
        int l = LEVEL_LAYER(at);
        if (l >= 8) return;
        float x, y;
        if (project(LEVEL_X(at) * TILE, s.snap.heights[l] + 10.0f, LEVEL_Z(at) * TILE, width, height, x, y)) {
            out.push_back({ text, x, y, color });
        }
    };
    for (int slot = 0; slot < 8; slot++) {
        uint32_t at = s.level.starts[slot];
        if (at == LEVEL_NONE && slot < 4) at = own_start(slot);
        if (at != LEVEL_NONE) add(at, std::to_string(slot + 1), colors[slot]);
    }
    for (auto& [at, item] : s.level.items) add(at, item_text(item), 0xFFFFFFFF);
    return out;
}
