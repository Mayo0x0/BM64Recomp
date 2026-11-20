// Custom levels: rebuilds a battle stage's model to the shape a level gives its grid (include/level_layout.h).
//
// The stage model (STAGE_MODEL_POINTER) draws its floors, walls and ramps from a few F3DEX display lists with big
// triangles, each over many tiles. Where the level changes a column's shape, the triangles over it are cut at its
// borders: the pieces over the changed tiles (and wall pieces on their edges) go, the rest is drawn as before. The
// changed tiles get new geometry: floor and ramp tops, a cap on walls, and the sides on their edges. A new face takes
// the material of the nearest original triangle of its kind: it is drawn right after that triangle's state in a copy
// of its display list, with texture coordinates and normals from that triangle's mapping, so textures and lights stay
// the stage's. The copy lives at SHAPE_MESH and the original list's first command branches to it; the frontend keeps
// the original commands to undo that.
//
// patches/level.c calls this (recomp_level_shape) after the blocks are set up and after every editor change, with
// GRID_ORIGINAL holding the stage's own grid and EDITOR_INFO the layer heights. It only reads RDRAM the game wrote,
// so every netplay PC builds the same.
#include "level_editor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace {

uint8_t* R = nullptr;

uint32_t rd32(uint32_t a) { return *(uint32_t*)(R + (a & 0x7FFFFC)); }
void wr32(uint32_t a, uint32_t v) { *(uint32_t*)(R + (a & 0x7FFFFC)) = v; }
uint16_t rd16(uint32_t a) { return *(uint16_t*)(R + ((a ^ 2) & 0x7FFFFF)); }
void wr16(uint32_t a, uint16_t v) { *(uint16_t*)(R + ((a ^ 2) & 0x7FFFFF)) = v; }
uint8_t rd8(uint32_t a) { return R[(a ^ 3) & 0x7FFFFF]; }
void wr8(uint32_t a, uint8_t v) { R[(a ^ 3) & 0x7FFFFF] = v; }
float rdf(uint32_t a) {
    uint32_t w = rd32(a);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

constexpr float HALF = 50.0f;        // half a tile
constexpr float FOUNDATION = 200.0f; // how far the sides of a hole go down below the lowest layer

struct Grid {
    uint32_t base;
    int layers, w, d;
    explicit Grid(uint32_t address) : base(address) {
        layers = std::min<int>(rd8(address), 8);
        w = (int)rd32(address + 4) * 8;
        d = (int)rd32(address + 8) * 8;
    }
    int kind(int x, int l, int z) const {
        if (x < 0 || z < 0 || x >= w || z >= d || l < 0 || l >= layers) return 0;
        uint32_t chunk = (uint32_t)((z >> 3) * (w / 8) + (x >> 3));
        return rd16(base + l * GRID_LAYER_SIZE + chunk * 0x82 + 0x14 + ((z & 7) * 8 + (x & 7)) * 2) & 0xF;
    }
};

struct V {
    float x, y, z;
    float s, t;
    uint8_t n[4];
};

struct Tri {
    int dl;          // index into the display lists
    int cmd;         // command index in that list
    int half;        // 0: the only or first triangle of the command, 1: the second of a TRI2
    int state;       // render state (per list): everything but vertices and triangles counts as a change
    V v[3];
    float nx, ny, nz; // unit normal (geometric, oriented by the vertex normals)
    bool vertical, below;
    std::vector<uint32_t> edges; // vertical triangles on a tile edge: the edges
    bool hidden = false;
};

struct DisplayList {
    uint32_t address;
    std::vector<uint32_t> words; // the original commands, up to and with the end
    std::vector<std::vector<int>> live_loads; // per command: the vertex loads the cache holds after it (in order)
};

struct Saved {
    uint32_t model = 0;
    std::map<uint32_t, std::array<uint32_t, 2>> first; // display list -> its original first command
    std::vector<uint32_t> patched;                     // lists that branch to SHAPE_MESH now
    float offset[3]{};                                 // the instance position they were built for
} saved;

constexpr uint32_t STAGE_INSTANCE_POINTER = 0x80024420u; // the stage model's instance (drawn at its position:
constexpr uint32_t INSTANCE_POSITION = 0x34;             // some stages' models are centered on 0)

// The instance position, 0 while there is none.
void instance_offset(float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    uint32_t instance = rd32(STAGE_INSTANCE_POINTER);
    if ((instance >> 24) != 0x80 || (instance & 0xFFFFFF) >= 0x7FFF00) return;
    for (int k = 0; k < 3; k++) {
        float v = rdf(instance + INSTANCE_POSITION + k * 4);
        out[k] = std::isfinite(v) && std::fabs(v) < 20000.0f ? v : 0.0f;
    }
}

bool is_ramp(int kind) { return kind >= SHAPE_RAMP_PZ && kind <= SHAPE_RAMP_NX; }

uint32_t tile_key(int x, int z) { return (uint32_t)(x + 1) << 16 | (uint32_t)(z + 1); }
int key_x(uint32_t k) { return (int)(k >> 16) - 1; }
int key_z(uint32_t k) { return (int)(k & 0xFFFF) - 1; }
// The edge between tile (x, z) and (x + 1, z) (axis 0) or (x, z + 1) (axis 1).
uint32_t edge_key(int axis, int x, int z) { return (uint32_t)axis << 31 | tile_key(x, z); }

// A vertical triangle in the plane between two tile rows: the edges it lies on (empty when it is not on one).
std::vector<uint32_t> edges_of(const Tri& t) {
    std::vector<uint32_t> out;
    float minx = std::min({ t.v[0].x, t.v[1].x, t.v[2].x }), maxx = std::max({ t.v[0].x, t.v[1].x, t.v[2].x });
    float minz = std::min({ t.v[0].z, t.v[1].z, t.v[2].z }), maxz = std::max({ t.v[0].z, t.v[1].z, t.v[2].z });
    auto on_line = [](float v) { float m = std::fmod(v + HALF + 100000.0f, 100.0f); return m < 0.5f || m > 99.5f; };
    if (maxx - minx < 0.5f && on_line(minx)) {
        int a = (int)std::floor((minx - HALF + 0.5f) / 100.0f);
        for (int z = (int)std::floor((minz + HALF) / 100.0f); z * 100.0f - HALF < maxz - 1; z++) {
            if (z * 100.0f + HALF > minz + 1) out.push_back(edge_key(0, a, z));
        }
    } else if (maxz - minz < 0.5f && on_line(minz)) {
        int a = (int)std::floor((minz - HALF + 0.5f) / 100.0f);
        for (int x = (int)std::floor((minx + HALF) / 100.0f); x * 100.0f - HALF < maxx - 1; x++) {
            if (x * 100.0f + HALF > minx + 1) out.push_back(edge_key(1, x, a));
        }
    }
    return out;
}

// Texture coordinates as an affine function of two plane coordinates (from a triangle's three corners).
struct Mapping {
    double s[3], t[3]; // s = s[0] * a + s[1] * b + s[2]
    double n[3][3];    // the vertex normals the same way
    bool ok = false;
};

Mapping solve(const double a[3], const double b[3], const double s[3], const double t[3], const double n[3][3]) {
    Mapping m;
    double det = a[0] * (b[1] - b[2]) - b[0] * (a[1] - a[2]) + (a[1] * b[2] - a[2] * b[1]);
    if (std::fabs(det) < 1e-6) return m;
    auto fit = [&](const double* v, double* out) {
        out[0] = (v[0] * (b[1] - b[2]) - b[0] * (v[1] - v[2]) + (v[1] * b[2] - v[2] * b[1])) / det;
        out[1] = (a[0] * (v[1] - v[2]) - v[0] * (a[1] - a[2]) + (a[1] * v[2] - a[2] * v[1])) / det;
        out[2] = (a[0] * (b[1] * v[2] - b[2] * v[1]) - b[0] * (a[1] * v[2] - a[2] * v[1]) + v[0] * (a[1] * b[2] - a[2] * b[1])) / det;
    };
    fit(s, m.s);
    fit(t, m.t);
    for (int k = 0; k < 3; k++) {
        double v[3] = { n[0][k], n[1][k], n[2][k] };
        fit(v, m.n[k]);
    }
    m.ok = true;
    return m;
}

// A convex polygon (drawing order kept), drawn as a fan.
using Poly = std::vector<V>;

V lerp(const V& a, const V& b, float f) {
    V v;
    v.x = a.x + (b.x - a.x) * f;
    v.y = a.y + (b.y - a.y) * f;
    v.z = a.z + (b.z - a.z) * f;
    v.s = a.s + (b.s - a.s) * f;
    v.t = a.t + (b.t - a.t) * f;
    for (int k = 0; k < 3; k++) v.n[k] = (uint8_t)(int8_t)std::lround((int8_t)a.n[k] + ((int8_t)b.n[k] - (int8_t)a.n[k]) * f);
    v.n[3] = (uint8_t)std::lround(a.n[3] + (b.n[3] - a.n[3]) * f);
    return v;
}

// Cuts polygons by the plane x = c (axis 0) or z = c (axis 1); a polygon in that plane stays whole.
void split(std::vector<Poly>& polys, int axis, float c) {
    std::vector<Poly> out;
    for (const Poly& p : polys) {
        auto d = [&](const V& v) { return (axis == 0 ? v.x : v.z) - c; };
        bool below = false, above = false;
        for (const V& v : p) {
            below = below || d(v) < -0.01f;
            above = above || d(v) > 0.01f;
        }
        if (!below || !above) {
            out.push_back(p);
            continue;
        }
        Poly a, b;
        for (size_t i = 0; i < p.size(); i++) {
            const V& cur = p[i];
            const V& next = p[(i + 1) % p.size()];
            float dc = d(cur), dn = d(next);
            if (dc <= 0.01f) a.push_back(cur);
            if (dc >= -0.01f) b.push_back(cur);
            if ((dc < -0.01f && dn > 0.01f) || (dc > 0.01f && dn < -0.01f)) {
                V m = lerp(cur, next, dc / (dc - dn));
                a.push_back(m);
                b.push_back(m);
            }
        }
        if (a.size() >= 3) out.push_back(a);
        if (b.size() >= 3) out.push_back(b);
    }
    polys.swap(out);
}

struct Builder {
    Grid grid, original;
    float h[8]{};
    float offset[3]{}; // model coordinates + offset = world
    std::vector<DisplayList> lists;
    std::vector<Tri> tris;
    bool right_handed = true; // front faces: (v1 - v0) x (v2 - v0) points out
    std::set<uint32_t> region, edges;                 // the changed columns and their edges
    std::map<std::pair<int, int>, std::vector<Poly>> extra; // (list, command) -> polygons drawn after it
    std::map<std::pair<int, int>, int> anchors;               // (list, state) -> the command of its last triangle
    size_t faces = 0;

    Builder() : grid(GRID_ADDRESS), original(GRID_ORIGINAL) {}

    float top_of_layer(int l) const { return l + 1 < grid.layers ? h[l + 1] : h[l] + 100.0f; }

    // How high the solid part of layer l of a column reaches at a point (u, v) of the tile (-50 to 50 from its
    // middle), or -1e9 for none. Layer -1 is the ground under any column that has something.
    float solid_top(int x, int z, int l, float u, float v) const {
        if (l < 0) {
            for (int k = 0; k < grid.layers; k++) {
                if (grid.kind(x, k, z) != SHAPE_AIR) return h[0];
            }
            return -1e9f;
        }
        int kind = grid.kind(x, l, z);
        if (kind == SHAPE_SOLID) return top_of_layer(l);
        if (is_ramp(kind)) return ramp_height(kind, l, u, v);
        return -1e9f;
    }

    float layer_bottom(int l) const { return l < 0 ? h[0] - FOUNDATION : h[l]; }

    float ramp_height(int kind, int l, float u, float v) const {
        float f; // 0 at the low side, 1 at the high side
        switch (kind) {
        case SHAPE_RAMP_PZ: f = (v + HALF) / 100.0f; break;
        case SHAPE_RAMP_NZ: f = (HALF - v) / 100.0f; break;
        case SHAPE_RAMP_PX: f = (u + HALF) / 100.0f; break;
        default: f = (HALF - u) / 100.0f; break;
        }
        return h[l] + f * (top_of_layer(l) - h[l]);
    }

    bool read_lists() {
        uint32_t model = rd32(STAGE_MODEL_POINTER);
        if ((model >> 24) != 0x80 || rd32(model) != STAGE_MODEL_MAGIC) return false;
        if (saved.model != model) {
            saved = Saved();
            saved.model = model;
        }
        instance_offset(offset);
        uint32_t count = rd32(model + 4);
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint32_t entry = model + 12 + i * 12;
            if (rd32(entry) != 0) continue;
            uint32_t address = model + rd32(entry + 8);
            // Patched before: its original first command is saved. Else it is the stage's own (loaded again).
            uint32_t w0 = rd32(address), w1 = rd32(address + 4);
            bool patched = (w0 >> 24) == 0x06 && w1 >= SHAPE_MESH && w1 < SHAPE_MESH_END;
            auto it = saved.first.find(address);
            if (patched) {
                if (it == saved.first.end()) continue; // not ours to undo
                wr32(address, it->second[0]);
                wr32(address + 4, it->second[1]);
            } else {
                saved.first[address] = { w0, w1 };
            }
            DisplayList list;
            list.address = address;
            for (uint32_t a = address; a < address + 0x10000; a += 8) {
                list.words.push_back(rd32(a));
                list.words.push_back(rd32(a + 4));
                if ((rd32(a) >> 24) == 0xB8) break;
            }
            lists.push_back(std::move(list));
        }
        return !lists.empty();
    }

    // Walks a list: its triangles, their render states and what the vertex cache holds after each command.
    void parse(int index) {
        DisplayList& list = lists[index];
        uint32_t model = saved.model;
        std::array<V, 32> cache{};
        std::array<int, 32> loaded_by;
        loaded_by.fill(-1);
        int state = 0;
        size_t commands = list.words.size() / 2;
        list.live_loads.resize(commands);
        for (size_t c = 0; c < commands; c++) {
            uint32_t w0 = list.words[c * 2], w1 = list.words[c * 2 + 1];
            uint32_t op = w0 >> 24;
            if (op == 0x04) {
                int n = (int)((w0 >> 10) & 0x3F), v0 = (int)((w0 >> 16) & 0xFF) / 2;
                uint32_t address = (w1 >> 24) == 0x02 ? model + (w1 & 0xFFFFFF) : w1;
                for (int i = 0; i < n && v0 + i < 32; i++) {
                    uint32_t a = address + i * 16;
                    V& v = cache[v0 + i];
                    v.x = (int16_t)rd16(a) + offset[0];
                    v.y = (int16_t)rd16(a + 2) + offset[1];
                    v.z = (int16_t)rd16(a + 4) + offset[2];
                    v.s = (int16_t)rd16(a + 8);
                    v.t = (int16_t)rd16(a + 10);
                    for (int k = 0; k < 4; k++) v.n[k] = rd8(a + 12 + k);
                    loaded_by[v0 + i] = (int)c;
                }
            } else if (op == 0xBF || op == 0xB1) {
                for (int half = 0; half < (op == 0xB1 ? 2 : 1); half++) {
                    uint32_t w = (op == 0xB1 && half == 0) ? w0 : w1;
                    Tri t;
                    t.dl = index;
                    t.cmd = (int)c;
                    t.half = half;
                    t.state = state;
                    for (int k = 0; k < 3; k++) t.v[k] = cache[((w >> (16 - 8 * k)) & 0xFF) / 2 % 32];
                    tris.push_back(t);
                    anchors[{ index, state }] = (int)c;
                }
            } else if (op != 0xBE && op != 0x00 && op != 0xB8) {
                state++;
            }
            std::vector<int> live;
            for (int by : loaded_by) {
                if (by >= 0 && std::find(live.begin(), live.end(), by) == live.end()) live.push_back(by);
            }
            std::sort(live.begin(), live.end());
            list.live_loads[c] = live;
        }
    }

    void classify() {
        int agree = 0, disagree = 0;
        for (Tri& t : tris) {
            double ax = t.v[1].x - t.v[0].x, ay = t.v[1].y - t.v[0].y, az = t.v[1].z - t.v[0].z;
            double bx = t.v[2].x - t.v[0].x, by = t.v[2].y - t.v[0].y, bz = t.v[2].z - t.v[0].z;
            double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-6) { t.hidden = false; t.vertical = false; t.below = true; continue; }
            nx /= len; ny /= len; nz /= len;
            // The vertex normals (lit models: the color bytes) say which side is the front.
            double vn[3] = { 0, 0, 0 };
            for (auto& v : t.v) for (int k = 0; k < 3; k++) vn[k] += (int8_t)v.n[k];
            double dot = vn[0] * nx + vn[1] * ny + vn[2] * nz;
            if (dot > 0) agree++; else if (dot < 0) disagree++;
            double sign = dot < 0 ? -1.0 : 1.0;
            t.nx = (float)(nx * sign); t.ny = (float)(ny * sign); t.nz = (float)(nz * sign);
            t.vertical = std::fabs(ny) < 0.1;
            t.below = std::max({ t.v[0].y, t.v[1].y, t.v[2].y }) < h[0] - 1.0f;
            if (t.vertical) t.edges = edges_of(t);
        }
        right_handed = agree >= disagree;
    }

    bool in_region(int x, int z) const { return region.count(tile_key(x, z)) != 0; }

    // Cuts the triangles over the changed columns at their borders; the pieces over them (and wall pieces on their
    // edges) go, the rest is drawn after the triangle's command instead of it.
    void clip() {
        for (uint32_t k : region) {
            int x = key_x(k), z = key_z(k);
            edges.insert(edge_key(0, x, z));
            edges.insert(edge_key(0, x - 1, z));
            edges.insert(edge_key(1, x, z));
            edges.insert(edge_key(1, x, z - 1));
        }
        std::set<float> cuts_x, cuts_z; // the changed columns' borders
        for (uint32_t k : region) {
            cuts_x.insert(key_x(k) * 100.0f - HALF);
            cuts_x.insert(key_x(k) * 100.0f + HALF);
            cuts_z.insert(key_z(k) * 100.0f - HALF);
            cuts_z.insert(key_z(k) * 100.0f + HALF);
        }
        for (Tri& t : tris) {
            if (t.below) continue;
            float minx = std::min({ t.v[0].x, t.v[1].x, t.v[2].x }) - 0.5f, maxx = std::max({ t.v[0].x, t.v[1].x, t.v[2].x }) + 0.5f;
            float minz = std::min({ t.v[0].z, t.v[1].z, t.v[2].z }) - 0.5f, maxz = std::max({ t.v[0].z, t.v[1].z, t.v[2].z }) + 0.5f;
            // Every triangle a cut line crosses is cut there, so neighbors share the new corners (no cracks).
            std::vector<Poly> pieces{ Poly{ t.v[0], t.v[1], t.v[2] } };
            for (float c : cuts_x) if (c > minx && c < maxx) split(pieces, 0, c);
            for (float c : cuts_z) if (c > minz && c < maxz) split(pieces, 1, c);
            std::vector<Poly> kept;
            for (Poly& piece : pieces) {
                float cx = 0, cz = 0;
                for (const V& v : piece) { cx += v.x; cz += v.z; }
                cx /= piece.size();
                cz /= piece.size();
                int tx = (int)std::floor((cx + HALF) / 100.0f), tz = (int)std::floor((cz + HALF) / 100.0f);
                float ux = cx - tx * 100.0f, uz = cz - tz * 100.0f;
                bool inside = std::fabs(ux) < HALF - 0.25f && std::fabs(uz) < HALF - 0.25f;
                bool drop = inside && in_region(tx, tz);
                if (!drop && t.vertical) { // a wall piece on an edge of a changed column
                    if (std::fabs(std::fabs(ux) - HALF) < 0.5f && std::fabs(uz) < HALF - 0.25f) {
                        int ex = ux > 0 ? tx : tx - 1;
                        drop = edges.count(edge_key(0, ex, tz)) != 0;
                    } else if (std::fabs(std::fabs(uz) - HALF) < 0.5f && std::fabs(ux) < HALF - 0.25f) {
                        int ez = uz > 0 ? tz : tz - 1;
                        drop = edges.count(edge_key(1, tx, ez)) != 0;
                    }
                }
                if (!drop) kept.push_back(std::move(piece));
            }
            if (pieces.size() == 1 && kept.size() == 1) continue; // neither cut nor over a changed column
            t.hidden = true;
            auto& out = extra[{ t.dl, t.cmd }];
            for (Poly& piece : kept) out.push_back(std::move(piece));
        }
    }

    // The height of a flat triangle's plane at (x, z), when the point is over it.
    static bool plane_at(const Tri& t, double x, double z, double& y) {
        double x0 = t.v[0].x, z0 = t.v[0].z, x1 = t.v[1].x, z1 = t.v[1].z, x2 = t.v[2].x, z2 = t.v[2].z;
        double det = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
        if (std::fabs(det) < 1e-6) return false;
        double a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / det;
        double b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / det;
        double c = 1.0 - a - b;
        if (a < -1e-4 || b < -1e-4 || c < -1e-4) return false;
        y = a * t.v[0].y + b * t.v[1].y + c * t.v[2].y;
        return true;
    }

    // The original triangle whose material a face takes: the nearest of its kind (tops: flat ones over the same
    // tile and height first; sides: vertical ones facing the same way).
    const Tri* material_for(bool side, float cx, float cy, float cz, float nx, float nz) const {
        const Tri* best = nullptr;
        double best_score = 1e18;
        bool edge_sides = false; // sides: walls on tile edges, when the stage has some (not round decorations)
        for (const Tri& t : tris) edge_sides = edge_sides || (t.vertical && !t.edges.empty());
        for (const Tri& t : tris) {
            if (t.below || t.vertical != side || (side && edge_sides && t.edges.empty())) continue;
            double mx = (t.v[0].x + t.v[1].x + t.v[2].x) / 3.0, my = (t.v[0].y + t.v[1].y + t.v[2].y) / 3.0;
            double mz = (t.v[0].z + t.v[1].z + t.v[2].z) / 3.0;
            double score;
            if (side) {
                score = std::sqrt((mx - cx) * (mx - cx) + (mz - cz) * (mz - cz)) + std::fabs(my - cy);
                if (t.nx * nx + t.nz * nz < 0.7) score += 2000.0;
            } else {
                // Over the point: by how far its plane is from the height there; else by distance too.
                double y;
                if (plane_at(t, cx, cz, y)) score = 10.0 * std::fabs(y - cy);
                else score = std::sqrt((mx - cx) * (mx - cx) + (mz - cz) * (mz - cz)) + 10.0 * std::fabs(my - cy);
                if (t.ny < 0.3) score += 5000.0; // facing down
            }
            if (score < best_score) {
                best_score = score;
                best = &t;
            }
        }
        return best;
    }

    // Adds a face: corners in any order around it, its outward normal; texture coordinates from the material.
    void add_face(const Tri* m, std::array<std::array<float, 3>, 4> c, float nx, float ny, float nz) {
        if (m == nullptr) return;
        bool side = m->vertical;
        // Plane coordinates: tops (x, z); sides (along, y), along being z for x-facing faces and x otherwise.
        auto coords = [&](const float* p, bool face_x, double& a, double& b) {
            if (!side) { a = p[0]; b = p[2]; }
            else { a = face_x ? p[2] : p[0]; b = p[1]; }
        };
        bool m_face_x = std::fabs(m->nx) > std::fabs(m->nz);
        double a[3], b[3], s[3], t[3], n[3][3];
        for (int k = 0; k < 3; k++) {
            float p[3] = { m->v[k].x, m->v[k].y, m->v[k].z };
            coords(p, m_face_x, a[k], b[k]);
            s[k] = m->v[k].s;
            t[k] = m->v[k].t;
            for (int j = 0; j < 3; j++) n[k][j] = (int8_t)m->v[k].n[j];
        }
        Mapping map = solve(a, b, s, t, n);
        // A face facing the material's way takes its vertex normals (the stage's soft shading), others their own.
        bool own_normals = m->nx * nx + m->ny * ny + m->nz * nz < 0.95f;
        if (!map.ok) return;
        bool face_x = std::fabs(nx) > std::fabs(nz);
        // Order the corners so the face is a front face (the diagonals' cross product: one corner pair may meet).
        double ax = c[2][0] - c[0][0], ay = c[2][1] - c[0][1], az = c[2][2] - c[0][2];
        double bx = c[3][0] - c[1][0], by = c[3][1] - c[1][1], bz = c[3][2] - c[1][2];
        double gx = ay * bz - az * by, gy = az * bx - ax * bz, gz = ax * by - ay * bx;
        bool out = gx * nx + gy * ny + gz * nz > 0;
        if (out != right_handed) std::swap(c[1], c[3]);
        Poly q(4);
        double shift_s = 0, shift_t = 0;
        for (int k = 0; k < 4; k++) {
            double pa, pb;
            coords(c[k].data(), face_x, pa, pb);
            q[k].x = c[k][0];
            q[k].y = c[k][1];
            q[k].z = c[k][2];
            q[k].s = (float)(map.s[0] * pa + map.s[1] * pb + map.s[2]);
            q[k].t = (float)(map.t[0] * pa + map.t[1] * pb + map.t[2]);
            for (int j = 0; j < 3; j++) {
                double own = (j == 0 ? nx : j == 1 ? ny : nz) * 127.0;
                double v = own_normals ? own : map.n[j][0] * pa + map.n[j][1] * pb + map.n[j][2];
                q[k].n[j] = (uint8_t)(int8_t)std::clamp(std::lround(v), -127L, 127L);
            }
            q[k].n[3] = 0xFF;
        }
        // Keep the coordinates in range: a whole number of texture repeats (8192 covers 256 texels).
        shift_s = std::floor(q[0].s / 8192.0) * 8192.0;
        shift_t = std::floor(q[0].t / 8192.0) * 8192.0;
        for (auto& v : q) {
            v.s -= (float)shift_s;
            v.t -= (float)shift_t;
        }
        extra[{ m->dl, anchors[{ m->dl, m->state }] }].push_back(q);
        faces++;
    }

    void build_faces() {
        // Tops of the region's columns.
        for (uint32_t k : region) {
            int x = key_x(k), z = key_z(k);
            float cx = x * 100.0f, cz = z * 100.0f;
            for (int l = 0; l < grid.layers; l++) {
                int kind = grid.kind(x, l, z);
                bool cap = kind == SHAPE_SOLID && (l + 1 >= grid.layers || grid.kind(x, l + 1, z) == SHAPE_AIR);
                if (kind != SHAPE_FLOOR && !is_ramp(kind) && !cap) continue;
                std::array<std::array<float, 3>, 4> c;
                static const float corners[4][2] = { { -HALF, -HALF }, { HALF, -HALF }, { HALF, HALF }, { -HALF, HALF } };
                for (int i = 0; i < 4; i++) {
                    float y = kind == SHAPE_FLOOR ? h[l] : cap ? top_of_layer(l) : ramp_height(kind, l, corners[i][0], corners[i][1]);
                    c[i] = { cx + corners[i][0], y, cz + corners[i][1] };
                }
                float nx = 0, ny = 1, nz = 0;
                if (is_ramp(kind)) {
                    float rise = top_of_layer(l) - h[l];
                    float len = std::sqrt(rise * rise + 100.0f * 100.0f);
                    float dir = (kind == SHAPE_RAMP_PZ || kind == SHAPE_RAMP_PX) ? -1.0f : 1.0f;
                    ny = 100.0f / len;
                    if (kind == SHAPE_RAMP_PZ || kind == SHAPE_RAMP_NZ) nz = dir * rise / len;
                    else nx = dir * rise / len;
                }
                float cy = (c[0][1] + c[2][1]) / 2;
                add_face(material_for(false, cx, cy, cz, 0, 0), c, nx, ny, nz);
            }
        }
        // Sides on the changed columns' edges.
        for (uint32_t e : edges) {
            int axis = (int)(e >> 31), x = key_x(e & 0x7FFFFFFF), z = key_z(e & 0x7FFFFFFF);
            int bx = axis == 0 ? x + 1 : x, bz = axis == 0 ? z : z + 1;
            // The edge's corners, as (u, v) seen from tile a and from tile b.
            float ex = axis == 0 ? x * 100.0f + HALF : x * 100.0f, ez = axis == 0 ? z * 100.0f : z * 100.0f + HALF;
            float p[2][2], ua[2][2], ub[2][2];
            for (int i = 0; i < 2; i++) {
                float off = i == 0 ? -HALF : HALF;
                p[i][0] = axis == 0 ? ex : ex + off;
                p[i][1] = axis == 0 ? ez + off : ez;
                ua[i][0] = p[i][0] - x * 100.0f;  ua[i][1] = p[i][1] - z * 100.0f;
                ub[i][0] = p[i][0] - bx * 100.0f; ub[i][1] = p[i][1] - bz * 100.0f;
            }
            for (int l = -1; l < grid.layers; l++) {
                float base = layer_bottom(l);
                for (int side = 0; side < 2; side++) {
                    // side 0: tile a's face toward b; side 1: b's toward a.
                    int fx = side == 0 ? x : bx, fz = side == 0 ? z : bz, ox = side == 0 ? bx : x, oz = side == 0 ? bz : z;
                    float (*uf)[2] = side == 0 ? ua : ub;
                    float (*uo)[2] = side == 0 ? ub : ua;
                    float top[2], bottom[2];
                    bool any = false;
                    for (int i = 0; i < 2; i++) {
                        top[i] = solid_top(fx, fz, l, uf[i][0], uf[i][1]);
                        float other = solid_top(ox, oz, l, uo[i][0], uo[i][1]);
                        bottom[i] = std::max(base, other);
                        if (top[i] < bottom[i]) top[i] = bottom[i];
                        any = any || top[i] - bottom[i] > 0.5f;
                    }
                    if (!any) continue;
                    float nx = axis == 0 ? (side == 0 ? 1.0f : -1.0f) : 0.0f;
                    float nz = axis == 1 ? (side == 0 ? 1.0f : -1.0f) : 0.0f;
                    std::array<std::array<float, 3>, 4> c = { { { p[0][0], bottom[0], p[0][1] }, { p[1][0], bottom[1], p[1][1] },
                                                               { p[1][0], top[1], p[1][1] }, { p[0][0], top[0], p[0][1] } } };
                    float cy = (top[0] + top[1] + bottom[0] + bottom[1]) / 4;
                    add_face(material_for(true, (p[0][0] + p[1][0]) / 2, cy, (p[0][1] + p[1][1]) / 2, nx, nz), c, nx, 0, nz);
                }
            }
        }
    }

    // Writes the vertices and the new lists to SHAPE_MESH and points the original lists there.
    void emit() {
        uint32_t at = SHAPE_MESH;
        // Per (list, command): vertex loads of up to 32 vertices, each with its triangles.
        struct Batch {
            uint32_t address;
            uint32_t count;
            std::vector<std::array<int, 3>> tris;
        };
        std::map<std::pair<int, int>, std::vector<Batch>> batches;
        bool full = false;
        for (auto& [key, polys] : extra) {
            std::vector<Batch>& list = batches[key];
            for (const Poly& p : polys) {
                if (p.size() < 3 || p.size() > 32) continue;
                if (at + p.size() * 16 > SHAPE_MESH_END - 0x20000) { full = true; break; } // room for the lists
                if (list.empty() || list.back().count + p.size() > 32) list.push_back({ at, 0, {} });
                Batch& b = list.back();
                for (size_t i = 1; i + 1 < p.size(); i++) {
                    b.tris.push_back({ (int)b.count, (int)(b.count + i), (int)(b.count + i + 1) });
                }
                for (const V& v : p) {
                    wr16(at + 0, (uint16_t)(int16_t)std::lround(v.x - offset[0]));
                    wr16(at + 2, (uint16_t)(int16_t)std::lround(v.y - offset[1]));
                    wr16(at + 4, (uint16_t)(int16_t)std::lround(v.z - offset[2]));
                    wr16(at + 6, 0);
                    wr16(at + 8, (uint16_t)(int16_t)std::clamp(std::lround(v.s), -32768L, 32767L));
                    wr16(at + 10, (uint16_t)(int16_t)std::clamp(std::lround(v.t), -32768L, 32767L));
                    for (int k = 0; k < 4; k++) wr8(at + 12 + k, v.n[k]);
                    at += 16;
                }
                b.count += (uint32_t)p.size();
            }
        }
        if (full) printf("LEVEL shape: out of room for vertices\n");
        int hidden = 0;
        for (size_t li = 0; li < lists.size(); li++) {
            DisplayList& list = lists[li];
            std::map<int, std::array<bool, 2>> hide;
            bool any = false;
            for (const Tri& t : tris) {
                if (t.dl == (int)li && t.hidden) {
                    hide[t.cmd][t.half] = true;
                    any = true;
                    hidden++;
                }
            }
            for (auto& [key, b] : batches) any = any || (key.first == (int)li && !b.empty());
            if (!any) continue;
            std::vector<uint32_t> out;
            size_t commands = list.words.size() / 2;
            for (size_t c = 0; c < commands; c++) {
                uint32_t w0 = list.words[c * 2], w1 = list.words[c * 2 + 1];
                uint32_t op = w0 >> 24;
                auto h = hide.find((int)c);
                if (h != hide.end() && op == 0xBF) {
                    w0 = 0; w1 = 0; // no-op
                } else if (h != hide.end() && op == 0xB1) {
                    bool a = h->second[0], b = h->second[1];
                    if (a && b) { w0 = 0; w1 = 0; }
                    else if (a) { w0 = 0xBF000000; w1 = w1 & 0xFFFFFF; }
                    else { w1 = w0 & 0xFFFFFF; w0 = 0xBF000000; }
                }
                out.push_back(w0);
                out.push_back(w1);
                auto found = batches.find({ (int)li, (int)c });
                if (found == batches.end() || found->second.empty()) continue;
                for (const Batch& b : found->second) {
                    out.push_back(0x04000000u | b.count << 10 | (b.count * 16 - 1));
                    out.push_back(b.address);
                    for (const auto& tri : b.tris) {
                        out.push_back(0xBF000000u);
                        out.push_back((uint32_t)(tri[0] * 2) << 16 | (uint32_t)(tri[1] * 2) << 8 | (uint32_t)(tri[2] * 2));
                    }
                }
                for (int load : list.live_loads[c]) { // the cache as the list left it
                    out.push_back(list.words[load * 2]);
                    out.push_back(list.words[load * 2 + 1]);
                }
            }
            at = (at + 7) & ~7u;
            if (at + out.size() * 4 > SHAPE_MESH_END) {
                printf("LEVEL shape: no room for list %08X\n", list.address);
                continue;
            }
            uint32_t start = at;
            for (uint32_t w : out) {
                wr32(at, w);
                at += 4;
            }
            wr32(list.address, 0x06010000); // branch to the copy
            wr32(list.address + 4, start);
            saved.patched.push_back(list.address);
        }
        printf("LEVEL shape: offset %.0f,%.0f,%.0f, %zu changed tiles, %d triangles cut, %zu faces added, %u bytes\n",
               offset[0], offset[1], offset[2], region.size(), hidden, faces, at - SHAPE_MESH);
        fflush(stdout);
    }
};

} // namespace

void levels::build_shape(uint8_t* rdram) {
    R = rdram;
    if (std::getenv("BM64_NO_SHAPE_MESH") != nullptr) return;
    saved.patched.clear();
    Builder b;
    if (!b.read_lists()) return; // also gives the lists their own first command back
    std::copy(b.offset, b.offset + 3, saved.offset);
    if (b.grid.layers != b.original.layers || b.grid.w != b.original.w || b.grid.d != b.original.d) return;
    for (int l = 0; l < b.grid.layers; l++) b.h[l] = rdf(EDITOR_INFO + (EI_HEIGHTS + l) * 4);
    for (int z = 0; z < b.grid.d; z++) {
        for (int x = 0; x < b.grid.w; x++) {
            for (int l = 0; l < b.grid.layers; l++) {
                if (b.grid.kind(x, l, z) != b.original.kind(x, l, z)) {
                    b.region.insert(tile_key(x, z));
                    break;
                }
            }
        }
    }
    if (b.region.empty()) return; // the stage's own model
    for (size_t i = 0; i < b.lists.size(); i++) b.parse((int)i);
    b.classify();
    b.clip();
    b.build_faces();
    b.emit();
}

// Every frame (game_frame): the game places the stage model after the blocks are set up, so a rebuilt model is
// built again once its instance moved (only while our lists are still in place: a new stage replaces them).
void levels::shape_frame(uint8_t* rdram) {
    R = rdram;
    if (saved.patched.empty()) return;
    for (uint32_t address : saved.patched) {
        if ((rd32(address) >> 24) != 0x06 || rd32(address + 4) < SHAPE_MESH || rd32(address + 4) >= SHAPE_MESH_END) {
            saved.patched.clear();
            return;
        }
    }
    float now[3];
    instance_offset(now);
    for (int k = 0; k < 3; k++) {
        if (std::fabs(now[k] - saved.offset[k]) > 0.5f) {
            build_shape(rdram);
            return;
        }
    }
}
