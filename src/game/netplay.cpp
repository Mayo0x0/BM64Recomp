// Lockstep netplay (include/netplay.h). One UDP socket; the host is the authority.
//
// Lobby: a client sends P_HELLO until P_WELCOME, then fetches the host's save in chunks and reports ready. P_START gives
// each PC its slots and the session settings (input delay, cheats); every PC then starts the game from boot.
// Frames: frame n is the n-th controller poll. At poll n a PC sends its local pads for frame n + delay; the host
// waits for every PC's pads of frame n, sends the 8 pads of frame n to each client, and all PCs write them into
// the game. Packets repeat the last WINDOW frames, so a lost packet is covered by the next one.
// Checks: every CHECK_INTERVAL frames both sides hash RAM; the host sends its hash with the frames. A client whose
// hash differs asks for a resync: at a frame chosen by the host, the host keeps a copy of RAM and the client
// fetches the pages whose hash differs and writes them before that frame goes on.
// A relay (tools/netplay_relay) forwards packets for PCs that cannot reach the host directly.
#include "netplay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
static void close_socket(socket_t s) { close(s); }
#endif

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "recomp_input.h"
#include "zelda_cheats.h"
#include "zelda_config.h"
#include "librecomp/mods.hpp"
#include "ultramodern/save.hpp"
#include "restart.h"
#include "level_editor.h"
#include "librecomp/game.hpp"

extern std::vector<recomp::GameEntry> supported_games;

namespace {

using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint32_t PROTOCOL = 3; // 3: 8 cheats
constexpr uint32_t MAGIC = 0x4E384D42;       // "BM8N"
constexpr uint32_t RELAY_MAGIC = 0x52384D42; // "BM8R"
constexpr int MAX = netplay::max_players;
constexpr uint32_t WINDOW = 8;
constexpr uint32_t RING = 512;
constexpr uint32_t CHECK_INTERVAL = 60;
constexpr uint32_t RDRAM_SIZE = 0x800000;
constexpr uint32_t PAGE = 0x1000;
constexpr uint32_t PAGES = RDRAM_SIZE / PAGE;
constexpr uint32_t CHUNK = 0x400;
constexpr uint32_t CHUNKS_PER_PAGE = PAGE / CHUNK;
constexpr uint32_t HASHES_PER_PACKET = 256;
constexpr uint32_t SAVE_SIZE = 0x20000; // SaveType::AllowAll
constexpr uint32_t SAVE_CHUNKS = SAVE_SIZE / CHUNK;
// The host sends its save and its custom levels (include/level_editor.h) as one block.
constexpr uint32_t SHARED_SIZE = SAVE_SIZE + LEVEL_AREA_SIZE;
constexpr uint32_t SHARED_CHUNKS = SHARED_SIZE / CHUNK;
constexpr auto PEER_TIMEOUT = 10s;
constexpr int NAME_LEN = 24;
constexpr int CHEATS = 8;
constexpr uint8_t PAD_NO_RESPONSE = 0x8;

enum Packet : uint8_t {
    P_HELLO = 1, P_WELCOME, P_REJECT, P_LOBBY, P_STATUS, P_SAVE_WANT, P_SAVE_CHUNK, P_START, P_INPUT, P_FRAMES,
    P_RS_REQUEST, P_RS_WANT_HASHES, P_RS_HASHES, P_RS_WANT_CHUNKS, P_RS_CHUNK, P_RS_STALE, P_BYE, P_RESET
};
enum RelayOp : uint8_t {
    R_REGISTER = 1, R_REGISTERED, R_TO_HOST, R_FROM_CLIENT, R_TO_CLIENT, R_FROM_HOST, R_ERROR, R_LIST, R_ROOMS
};

#pragma pack(push, 1)
struct Pad {
    uint16_t buttons;
    int8_t x;
    int8_t y;
};
struct Header { uint32_t magic; uint8_t type; };
struct Hello { Header h; uint32_t protocol; uint64_t build; uint8_t players; char name[NAME_LEN]; };
struct Welcome { Header h; uint8_t index; };
struct Reject { Header h; char reason[96]; };
struct LobbyEntry { uint8_t players; uint8_t ready; char name[NAME_LEN]; };
struct Lobby { Header h; uint64_t save_hash; uint8_t count; LobbyEntry peers[MAX]; };
struct StatusPacket { Header h; uint8_t ready; };
struct SaveWant { Header h; uint16_t count; uint16_t index[64]; };
struct SaveChunk { Header h; uint16_t index; uint8_t data[CHUNK]; };
struct Start {
    Header h; uint32_t session; uint8_t delay; uint8_t total_players; uint8_t slot_base; uint8_t players;
    int8_t cheats[CHEATS];
};
struct Input { Header h; uint32_t first; uint8_t count; uint8_t players; Pad pads[WINDOW][MAX]; };
struct FrameEntry { uint8_t flags; Pad pads[MAX]; };
struct Frames { Header h; uint32_t first; uint8_t count; uint32_t check_frame; uint64_t check_hash; FrameEntry frames[WINDOW]; };
struct RsRequest { Header h; uint32_t frame; };
struct RsWantHashes { Header h; uint32_t frame; };
struct RsHashes { Header h; uint32_t frame; uint16_t first; uint16_t count; uint32_t hash[HASHES_PER_PACKET]; };
struct RsWantChunks { Header h; uint32_t frame; uint16_t count; uint16_t index[300]; };
struct RsChunk { Header h; uint32_t frame; uint16_t index; uint8_t data[CHUNK]; };
struct RsStale { Header h; uint32_t frame; };
struct RelayHeader { uint32_t magic; uint8_t op; uint32_t room; uint32_t client; };
// The host's registration at a relay carries this (tools/netplay_relay); listed rooms show in the relay's room list.
struct RoomInfo { uint8_t listed; uint8_t players; uint8_t in_game; char name[NAME_LEN]; };
struct RoomEntry { uint32_t room; uint8_t players; uint8_t in_game; char name[NAME_LEN]; }; // R_ROOMS: count, entries
#pragma pack(pop)

constexpr uint8_t FLAG_RESYNC = 1;
constexpr uint8_t FLAG_RESET = 2; // every PC restarts the game at this frame (netplay::request_reset)

struct Link {
    sockaddr_in addr{};
    uint32_t relay_client = 0; // relay mode: the relay's id of this client
};

struct Peer { // host side; peers[0] is the host itself
    Link link;
    std::string name;
    int players = 0;
    int slot_base = 0;
    bool ready = false;
    bool started = false; // sent its first P_INPUT
    bool dropped = false;
    clock_type::time_point last_heard{};
    std::array<uint32_t, RING> input_frame{}; // frame + 1 of the stored pads (0 = none)
    std::array<std::array<Pad, MAX>, RING> inputs{};
    uint32_t resync_at = 0;
};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    socket_t sock = INVALID_SOCKET;
    std::thread thread;
    std::atomic_bool running = false;
    // At exit (quit or reset in a session): a joinable std::thread would end the program with std::terminate.
    ~State() {
        running = false;
        if (thread.joinable()) thread.join();
    }

    netplay::Role role = netplay::Role::None;
    netplay::Phase phase = netplay::Phase::Idle;
    std::string message;
    std::string name;
    uint64_t build = 0;
    std::vector<int> local_ports; // frontend ports of this PC's players

    // Relay
    bool relay = false;
    sockaddr_in relay_addr{};
    uint32_t room = 0;
    bool listed = false; // host: shown in the relay's room list
    clock_type::time_point last_register{};

    // Host
    std::vector<Peer> peers;
    std::vector<uint8_t> save; // the host's save (client: being received)
    uint64_t save_hash = 0;

    // Client
    Link host_link;
    int my_index = -1;
    std::vector<bool> save_have;
    bool save_ready = false;
    std::vector<LobbyEntry> lobby;
    clock_type::time_point last_sent{};
    clock_type::time_point last_heard{};

    // Session
    bool start_request = false;
    bool ui_ready = false;
    bool game_started = false;
    bool in_game = false;
    uint32_t session = 0;
    int delay = 3;
    int total_players = 0;
    int slot_base = 0;
    int players = 0;
    int8_t cheats[CHEATS]{};
    uint32_t frame = 0; // next frame to sync
    std::array<uint32_t, RING> frame_known{}; // frame + 1 of the combined pads stored (client)
    std::array<FrameEntry, RING> frames{};
    std::array<std::array<Pad, MAX>, RING> my_inputs{}; // client: local pads by frame
    std::map<uint32_t, uint64_t> host_checks;  // client: host hash by frame
    std::map<uint32_t, uint64_t> own_checks;   // client: own hash by frame
    uint32_t latest_check_frame = 0;            // host
    uint64_t check_accum = 0;                   // the hash of this block's pages so far
    uint32_t skip_check_until = 0;              // client: a block with a resync inside is not compared
    uint64_t latest_check_hash = 0;
    uint32_t resync_wanted = 0;                 // client: frame of a failed check, 0 = none
    clock_type::time_point last_resync_request{};
    int resyncs = 0;
    bool check_log = false;
    bool check_pages = false; // BM64_NETPLAY_CHECKLOG=pages: also every page's hash
    uint8_t* rdram = nullptr;          // for the hang dump
    uint32_t watched_frame = 0;
    clock_type::time_point watched_since{};
    bool hang_dumped = false;
    bool no_resync = false;       // BM64_NETPLAY_NORESYNC: only log failed checks
    bool trace_rng = false;       // BM64_NETPLAY_TRACERNG: logs every random number's caller, to find a desync
    int jitter_ms = 0;            // BM64_NETPLAY_JITTER=ms: waits up to that long before each frame, like a slow link
    std::vector<uint32_t> dump_frames; // BM64_NETPLAY_DUMP=f1,f2: write RAM to netplay_<f>.bin

    // Resync, host: the copy of RAM at snapshot_frame and its page hashes
    uint32_t planned_resync = 0;
    uint32_t snapshot_frame = 0;
    std::vector<uint8_t> snapshot;
    std::vector<uint32_t> snapshot_hashes;
    // Resync, client
    uint32_t rs_frame = 0;
    std::vector<uint32_t> rs_hashes;
    std::vector<bool> rs_hash_have;
    struct RsChunkData { std::array<uint8_t, CHUNK> data; bool have = false; };
    std::map<uint32_t, RsChunkData> rs_chunks;
    bool rs_stale = false;

    int auto_start_players = 0;

    // Reset (request_reset)
    uint32_t reset_frame = 0;   // host: the frame with FLAG_RESET, 0 = none
    bool reset_requested = false; // client: asking the host
};

State g;

// ---------------------------------------------------------------- helpers

template <typename T> T make(uint8_t type) {
    T packet{};
    packet.h.magic = MAGIC;
    packet.h.type = type;
    return packet;
}

void copy_name(char* out, const std::string& name) {
    std::memset(out, 0, NAME_LEN);
    std::memcpy(out, name.data(), std::min<size_t>(name.size(), NAME_LEN - 1));
}

std::string read_name(const char* in) {
    return std::string(in, strnlen(in, NAME_LEN));
}

void raw_send(const sockaddr_in& to, const void* data, size_t size) {
    sendto(g.sock, (const char*)data, (int)size, 0, (const sockaddr*)&to, sizeof(to));
}

// Sends a packet to a client (host) or to the host (client), through the relay if there is one.
void send_link(const Link& link, const void* data, size_t size, bool to_host) {
    if (!g.relay) {
        raw_send(link.addr, data, size);
        return;
    }
    uint8_t buffer[2048];
    RelayHeader header{ RELAY_MAGIC, (uint8_t)(to_host ? R_TO_HOST : R_TO_CLIENT), g.room, link.relay_client };
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), data, size);
    raw_send(g.relay_addr, buffer, sizeof(header) + size);
}

template <typename T> void send_to_host(const T& packet, size_t size = sizeof(T)) {
    send_link(g.host_link, &packet, size, true);
}

template <typename T> void send_to_peer(const Peer& peer, const T& packet, size_t size = sizeof(T)) {
    send_link(peer.link, &packet, size, false);
}

bool same_link(const Link& a, const Link& b) {
    if (g.relay) {
        return a.relay_client == b.relay_client;
    }
    return a.addr.sin_addr.s_addr == b.addr.sin_addr.s_addr && a.addr.sin_port == b.addr.sin_port;
}

bool resolve(const std::string& text, uint16_t default_port, sockaddr_in& out) {
    std::string host = text;
    uint16_t port = default_port;
    size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        port = (uint16_t)std::atoi(text.c_str() + colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    out = *(sockaddr_in*)result->ai_addr;
    out.sin_port = htons(port);
    freeaddrinfo(result);
    return true;
}

uint64_t build_hash() {
    // shortcut: two builds of the same source count as different; use a source revision once there is one.
    std::string text = std::string(__DATE__ " " __TIME__);
    for (const auto& mod : recomp::mods::get_all_mod_details("bm64")) {
        if (recomp::mods::is_mod_enabled(mod.mod_id)) {
            text += "|" + mod.mod_id + "@" + mod.version.to_string();
        }
    }
    return XXH3_64bits(text.data(), text.size());
}

std::filesystem::path save_path(bool netplay_copy) {
    std::filesystem::path path = zelda64::get_app_folder_path() / "saves";
    if (netplay_copy) {
        path /= "netplay";
    }
    return path / "bm64_us.bin";
}

// The game state that checks compare and resyncs copy. Left out: what differs between PCs in a correct session
// (two PCs compared page by page, tools/eightslots/netplay_diff.py): libultra's thread structs and the scheduler,
// which hold times and retrace counts, frame and depth buffers and display lists, and the heap, whose models and
// animations follow the retrace count, and what differs in long runs whose game state stays the same: sound, by
// the look of it (0x80098000 to 0x800A5FFF, 0x80220000 to 0x80224FFF). Copying thread structs or stacks would also
// break the running threads.
struct Range { uint32_t start; uint32_t end; };
constexpr Range SYNCED[] = {
    { 0x80030000, 0x80098000 }, { 0x800A6000, 0x800BE000 }, { 0x800BF000, 0x800C0000 }, // game data, objects
    { 0x80225000, 0x8029F000 }, { 0x802A0000, 0x802A5000 }, { 0x802A6000, 0x802A8000 }, // code and globals
    { 0x802A9000, 0x802AA000 }, { 0x802AB000, 0x802AD000 }, { 0x802AE000, 0x802AF000 },
    { 0x802B0000, 0x80300000 },
    { 0x80700000, 0x80800000 }, // per-player data moved for 8 players (tools/eightslots)
};

bool page_synced(uint32_t page) {
    uint32_t vram = 0x80000000u + page * PAGE;
    for (const Range& range : SYNCED) {
        if (vram >= range.start && vram < range.end) return true;
    }
    return false;
}

// Words inside synced pages that differ anyway, found in long sessions whose game state stayed the same:
// a flag set for a frame at different times (0x80064434), the per-player array moved from the display task page
// 0x802AFCE8, the per-player float cache of func_8024D108 (moved from 0x802ACA78) and a per-player word at
// 0x800BD7C4 that is -1 on one PC and a small number on the other for a moment (a sound handle, by the look of it).
constexpr Range UNSYNCED_WORDS[] = {
    { 0x80064430, 0x80064440 }, { 0x80701000, 0x80701300 }, { 0x807019C0, 0x80701A00 }, { 0x800BD7C0, 0x800BD7E0 },
};

// Pointers into the graphics memory that is not synced (frame and depth buffers, display lists, matrices:
// 0x800C0000 to 0x8021FFFF). The render thread (Gfx_Render) flips between two display list buffers
// (Gfx_InitGfx: 0x8014C8D0 + index * 0x27000) as frames get drawn, so which one a frame uses depends on timing,
// and objects keep pointers into them. Such a word counts as 0 in checks, and a resync keeps this PC's own word
// where the two count the same.
bool graphics_pointer(uint32_t value) {
    return value >= 0x800C0000u && value < 0x80220000u;
}

// A word as checks see it.
uint32_t compared(uint32_t value) {
    return graphics_pointer(value) ? 0 : value;
}

bool unsynced_byte(uint32_t vram) {
    for (const Range& range : UNSYNCED_WORDS) {
        if (vram >= range.start && vram < range.end) return true;
    }
    return false;
}

uint32_t page_hash(const uint8_t* rdram, uint32_t page) {
    if (!page_synced(page)) return 0;
    uint32_t base = 0x80000000u + page * PAGE;
    uint32_t copy[PAGE / 4];
    std::memcpy(copy, rdram + page * PAGE, PAGE); // words as the game sees them (memory is word-swapped)
    for (uint32_t i = 0; i < PAGE / 4; i++) {
        copy[i] = unsynced_byte(base + i * 4) ? 0 : compared(copy[i]);
    }
    return (uint32_t)XXH3_64bits(copy, PAGE);
}

// Writes a chunk of the host's RAM, keeping this PC's UNSYNCED_WORDS and display list pointers.
void write_chunk(uint8_t* rdram, uint32_t index, const uint8_t* data) {
    uint32_t base = 0x80000000u + index * CHUNK;
    uint32_t* out = (uint32_t*)(rdram + index * CHUNK);
    const uint32_t* in = (const uint32_t*)data;
    for (uint32_t i = 0; i < CHUNK / 4; i++) {
        if (unsynced_byte(base + i * 4) || compared(in[i]) == compared(out[i])) continue;
        out[i] = in[i];
    }
}

// A check covers CHECK_INTERVAL frames: at frame f every PC hashes the pages p with p % CHECK_INTERVAL ==
// f % CHECK_INTERVAL, so a check costs little in any one frame. Returns true with the hash at a block's last frame.
bool advance_check(const uint8_t* rdram, uint32_t frame, uint64_t& hash) {
    uint32_t slice = frame % CHECK_INTERVAL;
    if (slice == 0) g.check_accum = 0;
    static uint32_t block_pages[PAGES]; // check log: each page's hash when it was taken
    for (uint32_t page = slice; page < PAGES; page += CHECK_INTERVAL) {
        uint32_t value = page_hash(rdram, page);
        block_pages[page] = value;
        g.check_accum = XXH3_64bits_withSeed(&value, sizeof(value), g.check_accum);
    }
    if (slice != CHECK_INTERVAL - 1) return false;
    hash = g.check_accum;
    if (g.check_log) {
        printf("NETPLAY check f=%u %016llX\n", frame, (unsigned long long)hash);
        if (g.check_pages) {
            std::string text = "NETPLAY pages f=" + std::to_string(frame) + ":";
            for (uint32_t page = 0; page < PAGES; page++) {
                if (!page_synced(page)) continue;
                char item[24];
                std::snprintf(item, sizeof(item), " %X=%08X", page, block_pages[page]);
                text += item;
            }
            text += "\n";
            fwrite(text.data(), 1, text.size(), stdout); // one write: unbuffered printf goes byte by byte
        }
    }
    return true;
}

// Guest memory is stored word-swapped.
void write_u16(uint8_t* rdram, uint32_t vram, uint16_t value) {
    *(uint16_t*)(rdram + ((vram - 0x80000000u) ^ 2)) = value;
}
uint16_t read_u16(uint8_t* rdram, uint32_t vram) {
    return *(uint16_t*)(rdram + ((vram - 0x80000000u) ^ 2));
}
void write_u8(uint8_t* rdram, uint32_t vram, uint8_t value) {
    *(rdram + ((vram - 0x80000000u) ^ 3)) = value;
}
int8_t read_s8(uint8_t* rdram, uint32_t vram) {
    return (int8_t) * (rdram + ((vram - 0x80000000u) ^ 3));
}

// Starts the game from the network thread, as the UI loop of a window in the background may not run for a while,
// but only once the UI ran (the runtime is set up by then); until then the UI starts it (take_start_request).
void start_game() {
    g.start_request = true;
    if (g.ui_ready && !g.game_started) {
        g.game_started = true;
        recomp::start_game(supported_games[0].game_id);
    }
}

void set_message(const std::string& text) {
    g.message = text;
    printf("NETPLAY %s\n", text.c_str());
    fflush(stdout);
}

void end_session(const std::string& reason) {
    g.phase = netplay::Phase::Ended;
    g.in_game = false;
    set_message(reason);
    g.cv.notify_all();
}

// ---------------------------------------------------------------- host

void host_send_lobby() {
    Lobby packet = make<Lobby>(P_LOBBY);
    packet.save_hash = g.save_hash;
    packet.count = (uint8_t)g.peers.size();
    for (size_t i = 0; i < g.peers.size(); i++) {
        packet.peers[i].players = (uint8_t)g.peers[i].players;
        packet.peers[i].ready = g.peers[i].ready;
        copy_name(packet.peers[i].name, g.peers[i].name);
    }
    for (size_t i = 1; i < g.peers.size(); i++) {
        send_to_peer(g.peers[i], packet);
    }
}

void host_send_start(Peer& peer) {
    Start packet = make<Start>(P_START);
    packet.session = g.session;
    packet.delay = (uint8_t)g.delay;
    packet.total_players = (uint8_t)g.total_players;
    packet.slot_base = (uint8_t)peer.slot_base;
    packet.players = (uint8_t)peer.players;
    std::memcpy(packet.cheats, g.cheats, CHEATS);
    send_to_peer(peer, packet);
}

int total_lobby_players() {
    int total = 0;
    for (const auto& peer : g.peers) {
        total += peer.players;
    }
    return total;
}

// Sends a client the combined frames up to the newest one the host has.
void host_send_frames(Peer& peer) {
    if (g.frame == 0) {
        return;
    }
    Frames packet = make<Frames>(P_FRAMES);
    uint32_t last = g.frame - 1;
    uint32_t first = last + 1 >= WINDOW ? last + 1 - WINDOW : 0;
    packet.first = first;
    packet.count = (uint8_t)(last - first + 1);
    packet.check_frame = g.latest_check_frame;
    packet.check_hash = g.latest_check_hash;
    for (uint32_t f = first; f <= last; f++) {
        FrameEntry& entry = packet.frames[f - first];
        entry = g.frames[f % RING];
        entry.flags |= peer.resync_at == f && f != 0 ? FLAG_RESYNC : 0;
    }
    send_to_peer(peer, packet, offsetof(Frames, frames) + packet.count * sizeof(FrameEntry));
}

Peer* host_find(const Link& link) {
    for (size_t i = 1; i < g.peers.size(); i++) {
        if (same_link(g.peers[i].link, link)) {
            return &g.peers[i];
        }
    }
    return nullptr;
}

void host_receive(const Link& from, const uint8_t* data, size_t size) {
    const Header* header = (const Header*)data;
    Peer* peer = host_find(from);
    if (peer != nullptr) {
        peer->last_heard = clock_type::now();
    }
    switch (header->type) {
    case P_HELLO: {
        if (size < sizeof(Hello)) return;
        const Hello* hello = (const Hello*)data;
        if (peer != nullptr) {
            Welcome welcome = make<Welcome>(P_WELCOME);
            welcome.index = (uint8_t)(peer - g.peers.data());
            send_to_peer(*peer, welcome);
            return;
        }
        Peer candidate{};
        candidate.link = from;
        auto reject = [&](const char* reason) {
            Reject packet = make<Reject>(P_REJECT);
            std::snprintf(packet.reason, sizeof(packet.reason), "%s", reason);
            send_to_peer(candidate, packet);
        };
        if (hello->protocol != PROTOCOL || hello->build != g.build) {
            reject("Another version or other mods than the host.");
            return;
        }
        if (g.phase != netplay::Phase::Lobby) {
            reject("The game has already started.");
            return;
        }
        if (total_lobby_players() + hello->players > MAX) {
            reject("Too many players (8 at most).");
            return;
        }
        candidate.name = read_name(hello->name);
        candidate.players = hello->players;
        candidate.last_heard = clock_type::now();
        g.peers.push_back(candidate);
        Welcome welcome = make<Welcome>(P_WELCOME);
        welcome.index = (uint8_t)(g.peers.size() - 1);
        send_to_peer(g.peers.back(), welcome);
        set_message(candidate.name + " joined.");
        host_send_lobby();
        return;
    }
    case P_STATUS: {
        if (peer == nullptr || size < sizeof(StatusPacket)) return;
        peer->ready = ((const StatusPacket*)data)->ready != 0;
        if (g.phase == netplay::Phase::InGame && !peer->started) {
            host_send_start(*peer);
        }
        return;
    }
    case P_SAVE_WANT: {
        if (peer == nullptr || size < offsetof(SaveWant, index)) return;
        const SaveWant* want = (const SaveWant*)data;
        for (int i = 0; i < std::min<int>(want->count, 64); i++) {
            uint16_t index = want->index[i];
            if (index >= SHARED_CHUNKS) continue;
            SaveChunk chunk = make<SaveChunk>(P_SAVE_CHUNK);
            chunk.index = index;
            std::memcpy(chunk.data, g.save.data() + index * CHUNK, CHUNK);
            send_to_peer(*peer, chunk);
        }
        return;
    }
    case P_INPUT: {
        if (peer == nullptr || size < offsetof(Input, pads)) return;
        const Input* input = (const Input*)data;
        peer->started = true;
        if (input->players != peer->players || size < offsetof(Input, pads) + input->count * sizeof(Input::pads[0])) return;
        for (uint32_t i = 0; i < input->count && i < WINDOW; i++) {
            uint32_t f = input->first + i;
            if (f + RING / 2 < g.frame) continue; // far too old
            peer->input_frame[f % RING] = f + 1;
            std::copy(std::begin(input->pads[i]), std::end(input->pads[i]), peer->inputs[f % RING].begin());
        }
        // The client may be waiting for frames whose packet was lost.
        host_send_frames(*peer);
        g.cv.notify_all();
        return;
    }
    case P_RS_REQUEST: {
        if (peer == nullptr || size < sizeof(RsRequest)) return;
        uint32_t bad = ((const RsRequest*)data)->frame;
        if (peer->resync_at > bad) return; // already planned after that check
        if (g.planned_resync <= g.frame) {
            g.planned_resync = g.frame + 1;
        }
        peer->resync_at = g.planned_resync;
        printf("NETPLAY %s: check of frame %u failed, resync at %u\n", peer->name.c_str(), bad, peer->resync_at);
        return;
    }
    case P_RS_WANT_HASHES: {
        if (peer == nullptr || size < sizeof(RsWantHashes)) return;
        uint32_t frame = ((const RsWantHashes*)data)->frame;
        if (frame != g.snapshot_frame || g.snapshot.empty()) {
            RsStale stale = make<RsStale>(P_RS_STALE);
            stale.frame = frame;
            send_to_peer(*peer, stale);
            return;
        }
        for (uint32_t first = 0; first < PAGES; first += HASHES_PER_PACKET) {
            RsHashes packet = make<RsHashes>(P_RS_HASHES);
            packet.frame = frame;
            packet.first = (uint16_t)first;
            packet.count = HASHES_PER_PACKET;
            std::memcpy(packet.hash, g.snapshot_hashes.data() + first, sizeof(packet.hash));
            send_to_peer(*peer, packet);
        }
        return;
    }
    case P_RS_WANT_CHUNKS: {
        if (peer == nullptr || size < offsetof(RsWantChunks, index)) return;
        const RsWantChunks* want = (const RsWantChunks*)data;
        if (want->frame != g.snapshot_frame || g.snapshot.empty()) {
            RsStale stale = make<RsStale>(P_RS_STALE);
            stale.frame = want->frame;
            send_to_peer(*peer, stale);
            return;
        }
        for (int i = 0; i < std::min<int>(want->count, 300); i++) {
            uint16_t index = want->index[i];
            if (index >= PAGES * CHUNKS_PER_PAGE) continue;
            RsChunk chunk = make<RsChunk>(P_RS_CHUNK);
            chunk.frame = want->frame;
            chunk.index = index;
            std::memcpy(chunk.data, g.snapshot.data() + index * CHUNK, CHUNK);
            send_to_peer(*peer, chunk);
        }
        return;
    }
    case P_RESET: {
        if (peer == nullptr) return;
        if (g.phase == netplay::Phase::InGame && g.reset_frame < g.frame) {
            g.reset_frame = g.frame; // the next frame the host builds; repeated requests keep it
            set_message(peer->name + " resets the game.");
        }
        return;
    }
    case P_BYE: {
        if (peer == nullptr) return;
        peer->dropped = true;
        set_message(peer->name + " left the game.");
        g.cv.notify_all();
        return;
    }
    }
}

// ---------------------------------------------------------------- client

void client_receive(const uint8_t* data, size_t size) {
    const Header* header = (const Header*)data;
    g.last_heard = clock_type::now();
    switch (header->type) {
    case P_WELCOME: {
        if (size < sizeof(Welcome)) return;
        if (g.phase == netplay::Phase::Connecting) {
            g.my_index = ((const Welcome*)data)->index;
            g.phase = netplay::Phase::Lobby;
            set_message("Connected to the host.");
        }
        return;
    }
    case P_REJECT: {
        if (size < sizeof(Reject)) return;
        end_session(std::string("Refused: ") + std::string(((const Reject*)data)->reason, strnlen(((const Reject*)data)->reason, 96)));
        return;
    }
    case P_LOBBY: {
        if (size < offsetof(Lobby, peers)) return;
        const Lobby* lobby = (const Lobby*)data;
        g.lobby.assign(lobby->peers, lobby->peers + std::min<int>(lobby->count, MAX));
        if (lobby->save_hash != g.save_hash) {
            g.save_hash = lobby->save_hash;
            g.save.assign(SHARED_SIZE, 0);
            g.save_have.assign(SHARED_CHUNKS, false);
            g.save_ready = false;
        }
        return;
    }
    case P_SAVE_CHUNK: {
        if (size < sizeof(SaveChunk) || g.save_have.empty()) return;
        const SaveChunk* chunk = (const SaveChunk*)data;
        if (chunk->index >= SHARED_CHUNKS) return;
        std::memcpy(g.save.data() + chunk->index * CHUNK, chunk->data, CHUNK);
        g.save_have[chunk->index] = true;
        if (std::all_of(g.save_have.begin(), g.save_have.end(), [](bool have) { return have; })) {
            g.save_ready = XXH3_64bits(g.save.data(), g.save.size()) == g.save_hash;
            if (!g.save_ready) {
                g.save_have.assign(SHARED_CHUNKS, false);
            }
        }
        return;
    }
    case P_START: {
        if (size < sizeof(Start) || g.phase != netplay::Phase::Lobby || !g.save_ready) return;
        const Start* start = (const Start*)data;
        g.session = start->session;
        g.delay = start->delay;
        g.total_players = start->total_players;
        g.slot_base = start->slot_base;
        g.players = start->players;
        std::memcpy(g.cheats, start->cheats, CHEATS);
        // The game reads the host's save from a copy; the own save stays as it is.
        std::filesystem::path path = save_path(true);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::remove(path.string() + ".bak", error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write((const char*)g.save.data(), SAVE_SIZE);
        file.close();
        levels::set_session_data(g.save.data() + SAVE_SIZE, LEVEL_AREA_SIZE);
        g.phase = netplay::Phase::InGame;
        g.in_game = true;
        start_game();
        set_message("The game starts.");
        return;
    }
    case P_FRAMES: {
        if (size < offsetof(Frames, frames)) return;
        const Frames* frames = (const Frames*)data;
        if (size < offsetof(Frames, frames) + frames->count * sizeof(FrameEntry)) return;
        for (uint32_t i = 0; i < frames->count && i < WINDOW; i++) {
            uint32_t f = frames->first + i;
            g.frames[f % RING] = frames->frames[i];
            g.frame_known[f % RING] = f + 1;
        }
        if (frames->check_frame != 0 && !g.host_checks.count(frames->check_frame)) {
            g.host_checks[frames->check_frame] = frames->check_hash;
            while (g.host_checks.size() > 16) g.host_checks.erase(g.host_checks.begin());
        }
        g.cv.notify_all();
        return;
    }
    case P_RS_HASHES: {
        if (size < sizeof(RsHashes)) return;
        const RsHashes* hashes = (const RsHashes*)data;
        if (hashes->frame != g.rs_frame || g.rs_hashes.empty() || hashes->first + hashes->count > PAGES) return;
        std::memcpy(g.rs_hashes.data() + hashes->first, hashes->hash, hashes->count * sizeof(uint32_t));
        for (uint32_t i = 0; i < hashes->count; i++) g.rs_hash_have[hashes->first + i] = true;
        g.cv.notify_all();
        return;
    }
    case P_RS_CHUNK: {
        if (size < sizeof(RsChunk)) return;
        const RsChunk* chunk = (const RsChunk*)data;
        auto it = g.rs_chunks.find(chunk->index);
        if (chunk->frame != g.rs_frame || it == g.rs_chunks.end()) return;
        std::memcpy(it->second.data.data(), chunk->data, CHUNK);
        it->second.have = true;
        g.cv.notify_all();
        return;
    }
    case P_RS_STALE: {
        if (size < sizeof(RsStale) || ((const RsStale*)data)->frame != g.rs_frame) return;
        g.rs_stale = true;
        g.cv.notify_all();
        return;
    }
    }
}


// ---------------------------------------------------------------- network thread

void handle_packet(const sockaddr_in& from, const uint8_t* data, size_t size) {
    if (g.relay) {
        if (size < sizeof(RelayHeader) || ((const RelayHeader*)data)->magic != RELAY_MAGIC) return;
        const RelayHeader* relay = (const RelayHeader*)data;
        const uint8_t* payload = data + sizeof(RelayHeader);
        size_t payload_size = size - sizeof(RelayHeader);
        switch (relay->op) {
        case R_REGISTERED:
            if (g.room != relay->room) {
                g.room = relay->room;
                set_message("Room code: " + std::to_string(g.room));
            }
            if (g.phase == netplay::Phase::Connecting) g.phase = netplay::Phase::Lobby;
            return;
        case R_ERROR:
            end_session("Relay: " + std::string((const char*)payload, strnlen((const char*)payload, payload_size)));
            return;
        case R_FROM_CLIENT: {
            if (g.role != netplay::Role::Host || payload_size < sizeof(Header) || ((const Header*)payload)->magic != MAGIC) return;
            Link link{};
            link.relay_client = relay->client;
            host_receive(link, payload, payload_size);
            return;
        }
        case R_FROM_HOST:
            if (g.role != netplay::Role::Client || payload_size < sizeof(Header) || ((const Header*)payload)->magic != MAGIC) return;
            client_receive(payload, payload_size);
            return;
        }
        return;
    }
    if (size < sizeof(Header) || ((const Header*)data)->magic != MAGIC) return;
    if (g.role == netplay::Role::Host) {
        Link link{};
        link.addr = from;
        host_receive(link, data, size);
    } else if (g.role == netplay::Role::Client && from.sin_addr.s_addr == g.host_link.addr.sin_addr.s_addr &&
               from.sin_port == g.host_link.addr.sin_port) {
        client_receive(data, size);
    }
}

bool every(clock_type::time_point& last, std::chrono::milliseconds interval) {
    auto now = clock_type::now();
    if (now - last < interval) return false;
    last = now;
    return true;
}

bool start_locked();

void periodic() {
    auto now = clock_type::now();
    // Debugging aid: a game that stops polling for 5 seconds writes its RAM once (netplay_hang.bin).
    if (g.in_game && g.rdram != nullptr && g.dump_frames.size() > 0) {
        if (g.frame != g.watched_frame) {
            g.watched_frame = g.frame;
            g.watched_since = now;
        } else if (!g.hang_dumped && now - g.watched_since > 5s) {
            g.hang_dumped = true;
            std::ofstream dump("netplay_hang.bin", std::ios::binary);
            dump.write((const char*)g.rdram, RDRAM_SIZE);
            printf("NETPLAY no poll since frame %u: RAM written to netplay_hang.bin\n", g.frame);
        }
    }
    static clock_type::time_point last_lobby, last_hello, last_status, last_save_want, last_start;
    if (g.phase == netplay::Phase::Ended || g.phase == netplay::Phase::Idle) return;
    if (g.relay && g.role == netplay::Role::Host && every(g.last_register, 2000ms)) {
        uint8_t packet[sizeof(RelayHeader) + sizeof(RoomInfo)];
        RelayHeader header{ RELAY_MAGIC, R_REGISTER, g.room, 0 };
        RoomInfo info{ (uint8_t)g.listed, (uint8_t)total_lobby_players(), (uint8_t)g.in_game, {} };
        copy_name(info.name, g.name);
        std::memcpy(packet, &header, sizeof(header));
        std::memcpy(packet + sizeof(header), &info, sizeof(info));
        raw_send(g.relay_addr, packet, sizeof(packet));
    }
    if (g.role == netplay::Role::Host) {
        if (g.phase == netplay::Phase::Lobby) {
            size_t before = g.peers.size();
            g.peers.erase(std::remove_if(g.peers.begin() + 1, g.peers.end(),
                [&](const Peer& peer) { return now - peer.last_heard > PEER_TIMEOUT; }), g.peers.end());
            if (g.peers.size() != before) set_message("A player was disconnected.");
            if (every(last_lobby, 500ms)) host_send_lobby();
            bool all_ready = std::all_of(g.peers.begin() + 1, g.peers.end(), [](const Peer& peer) { return peer.ready; });
            if (g.auto_start_players > 0 && total_lobby_players() >= g.auto_start_players && all_ready) {
                start_locked();
            }
        } else if (g.phase == netplay::Phase::InGame && every(last_start, 200ms)) {
            for (size_t i = 1; i < g.peers.size(); i++) {
                if (!g.peers[i].started && !g.peers[i].dropped) host_send_start(g.peers[i]);
            }
        }
        return;
    }
    // Client
    if (g.phase == netplay::Phase::Connecting) {
        if (g.last_sent != clock_type::time_point{} && now - g.last_sent > PEER_TIMEOUT) {
            end_session("The host does not answer.");
            return;
        }
        if (every(last_hello, 250ms)) {
            if (g.last_sent == clock_type::time_point{}) g.last_sent = now;
            Hello hello = make<Hello>(P_HELLO);
            hello.protocol = PROTOCOL;
            hello.build = g.build;
            hello.players = (uint8_t)g.local_ports.size();
            copy_name(hello.name, g.name);
            send_to_host(hello);
        }
        return;
    }
    if (now - g.last_heard > PEER_TIMEOUT) {
        end_session("Lost the connection to the host.");
        return;
    }
    if (every(last_status, 300ms)) {
        StatusPacket status = make<StatusPacket>(P_STATUS);
        status.ready = g.save_ready;
        send_to_host(status);
    }
    if (g.phase == netplay::Phase::Lobby && !g.save_have.empty() && !g.save_ready && every(last_save_want, 100ms)) {
        SaveWant want = make<SaveWant>(P_SAVE_WANT);
        for (uint32_t i = 0; i < SHARED_CHUNKS && want.count < 64; i++) {
            if (!g.save_have[i]) want.index[want.count++] = (uint16_t)i;
        }
        send_to_host(want);
    }
}

void receive_loop() {
    uint8_t buffer[4096];
    while (g.running) {
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        int received = recvfrom(g.sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &length);
        std::lock_guard lock(g.mutex);
        if (received > 0) {
            handle_packet(from, buffer, (size_t)received);
        }
        periodic();
    }
}

bool open_socket(uint16_t port) {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
#endif
    g.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g.sock == INVALID_SOCKET) return false;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(g.sock, (const sockaddr*)&local, sizeof(local)) != 0) {
        close_socket(g.sock);
        g.sock = INVALID_SOCKET;
        return false;
    }
#ifdef _WIN32
    DWORD timeout = 10;
    setsockopt(g.sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    // Without this an ICMP "port unreachable" makes every later recvfrom fail.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(g.sock, _WSAIOW(IOC_VENDOR, 12), &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    timeval timeout{ 0, 10000 };
    setsockopt(g.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    int buffer_size = 1 << 20;
    setsockopt(g.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buffer_size, sizeof(buffer_size));
    return true;
}

void stop_thread() {
    g.running = false;
    if (g.thread.joinable()) g.thread.join();
    if (g.sock != INVALID_SOCKET) {
        close_socket(g.sock);
        g.sock = INVALID_SOCKET;
    }
}

void reset_state() {
    stop_thread();
    std::lock_guard lock(g.mutex);
    g.role = netplay::Role::None;
    g.phase = netplay::Phase::Idle;
    g.relay = false;
    g.room = 0;
    g.peers.clear();
    g.save.clear();
    g.save_have.clear();
    g.save_ready = false;
    g.save_hash = 0;
    g.lobby.clear();
    g.my_index = -1;
    g.start_request = false;
    g.game_started = false;
    g.in_game = false;
    g.frame = 0;
    g.frame_known.fill(0);
    g.host_checks.clear();
    g.own_checks.clear();
    g.latest_check_frame = 0;
    g.reset_frame = 0;
    g.reset_requested = false;
    g.check_accum = 0;
    g.skip_check_until = 0;
    g.resync_wanted = 0;
    g.planned_resync = 0;
    g.snapshot.clear();
    g.snapshot_frame = 0;
    g.resyncs = 0;
    g.last_sent = {};
    g.last_heard = clock_type::now();
    g.cv.notify_all();
}

bool begin(netplay::Role role, uint16_t port, const std::string& name) {
    reset_state();
    std::lock_guard lock(g.mutex);
    g.local_ports.clear();
    for (int p = 0; p < recomp::max_ports && (int)g.local_ports.size() < MAX; p++) {
        if (recomp::get_port_mode(p) != recomp::ControllerPortMode::Off) g.local_ports.push_back(p);
    }
    if (g.local_ports.empty()) {
        set_message("No controller port is on (Setup controls).");
        return false;
    }
    if (!open_socket(port)) {
        set_message("Cannot open port " + std::to_string(port) + ".");
        return false;
    }
    g.role = role;
    g.name = name.empty() ? "Player" : name;
    g.build = build_hash();
    g.last_heard = clock_type::now();
    g.running = true;
    g.thread = std::thread(receive_loop);
    return true;
}

// Host, with the lock held.
bool start_locked() {
    if (g.role != netplay::Role::Host || g.phase != netplay::Phase::Lobby) return false;
    int slot = 0;
    for (size_t i = 0; i < g.peers.size(); i++) {
        Peer& peer = g.peers[i];
        if (i != 0 && !peer.ready) return false;
        peer.slot_base = slot;
        slot += peer.players;
    }
    g.total_players = slot;
    g.slot_base = 0;
    g.players = g.peers[0].players;
    std::random_device random;
    g.session = random();
    if (const char* delay = std::getenv("BM64_NETPLAY_DELAY")) g.delay = std::clamp(std::atoi(delay), 1, (int)WINDOW - 2);
    g.cheats[0] = (int8_t)zelda64::get_moonjump();
    g.cheats[1] = (int8_t)zelda64::get_always_max_bomb();
    g.cheats[2] = (int8_t)zelda64::get_always_max_fire();
    g.cheats[3] = (int8_t)zelda64::get_always_have_remote_bombs();
    g.cheats[4] = (int8_t)zelda64::get_always_have_red_bombs();
    g.cheats[5] = (int8_t)zelda64::get_infinite_credits();
    g.cheats[6] = (int8_t)zelda64::get_infinite_lives();
    g.cheats[7] = (int8_t)zelda64::get_unlock_all_stages();
    for (size_t i = 1; i < g.peers.size(); i++) host_send_start(g.peers[i]);
    levels::set_session_data(g.save.data() + SAVE_SIZE, LEVEL_AREA_SIZE);
    g.phase = netplay::Phase::InGame;
    g.in_game = true;
    start_game();
    set_message("The game starts with " + std::to_string(g.total_players) + " players.");
    return true;
}

// Client at a frame the host flagged: fetches the host's pages that differ and writes them.
void client_resync(std::unique_lock<std::mutex>& lock, uint8_t* rdram, uint32_t frame) {
    g.rs_frame = frame;
    g.rs_stale = false;
    g.rs_hashes.assign(PAGES, 0);
    g.rs_hash_have.assign(PAGES, false);
    g.rs_chunks.clear();
    std::vector<uint32_t> own(PAGES);
    for (uint32_t page = 0; page < PAGES; page++) own[page] = page_hash(rdram, page);
    auto deadline = clock_type::now() + PEER_TIMEOUT;
    auto all_hashes = [] { return std::all_of(g.rs_hash_have.begin(), g.rs_hash_have.end(), [](bool have) { return have; }); };
    while (!all_hashes() && !g.rs_stale && clock_type::now() < deadline && g.in_game) {
        RsWantHashes want = make<RsWantHashes>(P_RS_WANT_HASHES);
        want.frame = frame;
        send_to_host(want);
        g.cv.wait_for(lock, 60ms);
    }
    bool done = false;
    if (all_hashes() && !g.rs_stale) {
        std::string pages_text;
        for (uint32_t page = 0; page < PAGES; page++) {
            if (own[page] == g.rs_hashes[page]) continue;
            for (uint32_t k = 0; k < CHUNKS_PER_PAGE; k++) g.rs_chunks[page * CHUNKS_PER_PAGE + k];
            char text[16];
            std::snprintf(text, sizeof(text), " %06X", page * PAGE);
            pages_text += text;
        }
        auto missing = [] { return std::any_of(g.rs_chunks.begin(), g.rs_chunks.end(), [](const auto& c) { return !c.second.have; }); };
        while (missing() && !g.rs_stale && clock_type::now() < deadline && g.in_game) {
            RsWantChunks want = make<RsWantChunks>(P_RS_WANT_CHUNKS);
            want.frame = frame;
            for (const auto& [index, chunk] : g.rs_chunks) {
                if (!chunk.have && want.count < 300) want.index[want.count++] = (uint16_t)index;
            }
            send_to_host(want, offsetof(RsWantChunks, index) + want.count * sizeof(uint16_t));
            g.cv.wait_for(lock, 40ms);
        }
        if (!missing() && !g.rs_stale) {
            // The first word of each chunk that differs and counts, for finding what went out of step.
            for (const auto& [index, chunk] : g.rs_chunks) {
                for (uint32_t i = 0; i < CHUNK; i += 4) {
                    uint32_t host_word = *(const uint32_t*)(chunk.data.data() + i);
                    uint32_t own_word = *(const uint32_t*)(rdram + index * CHUNK + i);
                    bool ignored = unsynced_byte(0x80000000u + index * CHUNK + i) || compared(host_word) == compared(own_word);
                    if (!ignored) {
                        char text[48];
                        std::snprintf(text, sizeof(text), " [%08X %08X/%08X]", 0x80000000u + index * CHUNK + i,
                            *(const uint32_t*)(chunk.data.data() + i), *(const uint32_t*)(rdram + index * CHUNK + i));
                        pages_text += text;
                        break;
                    }
                }
            }
            for (const auto& [index, chunk] : g.rs_chunks) write_chunk(rdram, index, chunk.data.data());
            g.resyncs++;
            done = true;
            printf("NETPLAY resync at frame %u: %zu pages:%s\n", frame, g.rs_chunks.size() / CHUNKS_PER_PAGE, pages_text.c_str());
        }
    }
    if (!done) printf("NETPLAY resync at frame %u failed\n", frame);
    g.rs_chunks.clear();
    g.rs_hashes.clear();
    g.rs_hash_have.clear();
    g.rs_frame = 0;
    g.resync_wanted = 0;
    // Checks made before the repair say nothing any more, nor does the block it happened in.
    g.skip_check_until = frame - frame % CHECK_INTERVAL + CHECK_INTERVAL - 1;
    g.host_checks.erase(g.host_checks.begin(), g.host_checks.lower_bound(frame + 1));
    g.own_checks.erase(g.own_checks.begin(), g.own_checks.lower_bound(frame + 1));
}

void client_compare_checks() {
    if (g.resync_wanted != 0) return;
    for (const auto& [frame, hash] : g.host_checks) {
        auto own = g.own_checks.find(frame);
        if (own != g.own_checks.end() && own->second != hash) {
            if (g.no_resync) {
                printf("NETPLAY check of frame %u differs from the host\n", frame);
                g.own_checks.erase(own);
                return;
            }
            g.resync_wanted = frame;
            printf("NETPLAY check of frame %u differs from the host\n", frame);
            return;
        }
    }
}

} // namespace

// ---------------------------------------------------------------- API

bool netplay::host(uint16_t port, const std::string& relay, const std::string& name, bool listed) {
    if (!begin(Role::Host, relay.empty() ? port : 0, name)) return false;
    std::lock_guard lock(g.mutex);
    g.listed = listed;
    if (!relay.empty()) {
        if (!resolve(relay, default_relay_port, g.relay_addr)) {
            set_message("Relay address not found: " + relay);
            g.phase = Phase::Ended;
            return false;
        }
        g.relay = true;
        g.phase = Phase::Connecting;
        set_message("Connecting to the relay...");
    } else {
        g.phase = Phase::Lobby;
        set_message("Waiting for players on port " + std::to_string(port) + ".");
    }
    Peer self{};
    self.name = g.name;
    self.players = (int)g.local_ports.size();
    self.ready = true;
    g.peers.push_back(self);
    // The host's own save, read the way the game reads it (missing = all zeroes).
    g.save.assign(SAVE_SIZE, 0);
    std::ifstream file(save_path(false), std::ios::binary);
    if (file.good()) file.read((char*)g.save.data(), SAVE_SIZE);
    // The custom levels go with it; the host plays from the same copy (start_locked).
    std::vector<uint8_t> level_data = levels::session_data();
    g.save.insert(g.save.end(), level_data.begin(), level_data.end());
    g.save.resize(SHARED_SIZE, 0);
    g.save_hash = XXH3_64bits(g.save.data(), g.save.size());
    return true;
}

bool netplay::list_rooms(const std::string& relay, std::vector<RoomListing>& rooms, std::string& error) {
    rooms.clear();
    sockaddr_in to{};
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data); // counted; the session's socket may not be open yet
#endif
    if (!resolve(relay, default_relay_port, to)) {
        error = "Address not found: " + relay;
        return false;
    }
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        error = "Cannot open a socket.";
        return false;
    }
#ifdef _WIN32
    DWORD timeout = 300;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    BOOL report = FALSE; // an ICMP "port unreachable" (nothing listening) must not end the waiting early
    DWORD returned = 0;
    WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12), &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    timeval timeout{ 0, 300000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    RelayHeader ask{ RELAY_MAGIC, R_LIST, 0, 0 };
    bool answered = false;
    // A few tries over about 1.5 s: UDP may lose one.
    for (int attempt = 0; attempt < 5 && !answered; attempt++) {
        sendto(sock, (const char*)&ask, sizeof(ask), 0, (const sockaddr*)&to, sizeof(to));
        uint8_t buffer[4096];
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        int received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &length);
        if (received < (int)sizeof(RelayHeader) + 1) continue;
        const RelayHeader* header = (const RelayHeader*)buffer;
        if (header->magic != RELAY_MAGIC || header->op != R_ROOMS) continue;
        size_t count = buffer[sizeof(RelayHeader)];
        if (sizeof(RelayHeader) + 1 + count * sizeof(RoomEntry) > (size_t)received) continue;
        for (size_t i = 0; i < count; i++) {
            RoomEntry entry;
            std::memcpy(&entry, buffer + sizeof(RelayHeader) + 1 + i * sizeof(RoomEntry), sizeof(entry));
            rooms.push_back({ entry.room, read_name(entry.name), entry.players, entry.in_game != 0 });
        }
        answered = true;
    }
    close_socket(sock);
    if (!answered) error = "No relay answered at " + relay + ".";
    return answered;
}

bool netplay::join(const std::string& address, const std::string& relay_room, const std::string& name) {
    if (!begin(Role::Client, 0, name)) return false;
    std::lock_guard lock(g.mutex);
    bool found;
    if (!relay_room.empty()) {
        g.relay = true;
        g.room = (uint32_t)std::strtoul(relay_room.c_str(), nullptr, 10);
        found = resolve(address, default_relay_port, g.relay_addr);
    } else {
        found = resolve(address, default_port, g.host_link.addr);
    }
    if (!found) {
        set_message("Address not found: " + address);
        g.phase = Phase::Ended;
        return false;
    }
    g.phase = Phase::Connecting;
    set_message("Connecting...");
    return true;
}

namespace {

// At a frame with FLAG_RESET, with the lock held: every PC starts the program again into the main menu (the
// session ends there) and quits.
void relaunch_session() {
    set_message("Resetting the game.");
    g.in_game = false;
    g.phase = netplay::Phase::Ended;
    restart::relaunch({ { "BM64_NETPLAY", "" }, { "BM64_RESTART", "" }, { "BM64_AUTOSTART", "" } });
}

} // namespace

void netplay::request_reset() {
    std::lock_guard lock(g.mutex);
    if (!g.in_game) return;
    if (g.role == Role::Host) {
        if (g.reset_frame < g.frame) g.reset_frame = g.frame;
    } else {
        g.reset_requested = true; // sent with every input (send_inputs)
    }
}

// The host's custom levels changed in the lobby: the clients fetch the new block (P_LOBBY carries its hash).
void netplay::refresh_levels() {
    std::lock_guard lock(g.mutex);
    if (g.role != Role::Host || g.phase != Phase::Lobby || g.save.size() != SHARED_SIZE) return;
    std::vector<uint8_t> level_data = levels::session_data();
    std::fill(g.save.begin() + SAVE_SIZE, g.save.end(), 0);
    std::copy(level_data.begin(), level_data.begin() + std::min<size_t>(level_data.size(), LEVEL_AREA_SIZE), g.save.begin() + SAVE_SIZE);
    g.save_hash = XXH3_64bits(g.save.data(), g.save.size());
    for (size_t i = 1; i < g.peers.size(); i++) g.peers[i].ready = false;
}

bool netplay::start() {
    std::lock_guard lock(g.mutex);
    return start_locked();
}

void netplay::leave() {
    {
        std::lock_guard lock(g.mutex);
        if (g.sock != INVALID_SOCKET && g.role != Role::None) {
            Header bye{ MAGIC, P_BYE };
            if (g.role == Role::Client) {
                send_to_host(bye);
            } else {
                for (size_t i = 1; i < g.peers.size(); i++) send_to_peer(g.peers[i], bye);
            }
        }
    }
    reset_state();
}

netplay::Status netplay::status() {
    std::lock_guard lock(g.mutex);
    Status status;
    status.role = g.role;
    status.phase = g.phase;
    status.message = g.message;
    status.room = g.relay && g.room != 0 ? std::to_string(g.room) : "";
    if (g.role == Role::Host) {
        for (const auto& peer : g.peers) status.peers.push_back({ peer.name, peer.players, peer.ready });
        status.can_start = g.phase == Phase::Lobby &&
            std::all_of(g.peers.begin() + 1, g.peers.end(), [](const Peer& peer) { return peer.ready; });
    } else {
        for (const auto& entry : g.lobby) status.peers.push_back({ read_name(entry.name), entry.players, entry.ready != 0 });
    }
    status.total_players = g.role == Role::Host && g.phase == Phase::Lobby ? total_lobby_players() : g.total_players;
    status.frame = g.frame;
    status.resyncs = g.resyncs;
    return status;
}

void netplay::init_from_environment() {
    if (const char* jitter = std::getenv("BM64_NETPLAY_JITTER")) g.jitter_ms = std::atoi(jitter); // also offline
    g.trace_rng = std::getenv("BM64_NETPLAY_TRACERNG") != nullptr;
    const char* mode = std::getenv("BM64_NETPLAY");
    if (mode == nullptr) return;
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unattended tests read the log while the game runs
    std::string text = mode;
    const char* name = std::getenv("BM64_NETPLAY_NAME");
    if (const char* players = std::getenv("BM64_NETPLAY_PLAYERS")) g.auto_start_players = std::atoi(players);
    g.check_log = std::getenv("BM64_NETPLAY_CHECKLOG") != nullptr;
    g.check_pages = g.check_log && std::string(std::getenv("BM64_NETPLAY_CHECKLOG")) == "pages";
    g.no_resync = std::getenv("BM64_NETPLAY_NORESYNC") != nullptr;
    if (const char* dump = std::getenv("BM64_NETPLAY_DUMP")) {
        for (const char* p = dump; *p; ) {
            g.dump_frames.push_back((uint32_t)std::strtoul(p, (char**)&p, 10));
            if (*p == ',') p++; else break;
        }
    }
    auto after = [&](const char* prefix) { return text.substr(std::strlen(prefix)); };
    if (text.rfind("relayhost:", 0) == 0) {
        const char* listed = std::getenv("BM64_NETPLAY_LISTED"); // 1: in the relay's room list
        host(default_port, after("relayhost:"), name ? name : "Host", listed != nullptr && listed[0] == '1');
    } else if (text.rfind("relayjoin:", 0) == 0) {
        std::string rest = after("relayjoin:");
        size_t colon = rest.rfind(':');
        join(rest.substr(0, colon), rest.substr(colon + 1), name ? name : "Client");
    } else if (text.rfind("host", 0) == 0) {
        uint16_t port = text.size() > 5 ? (uint16_t)std::atoi(text.c_str() + 5) : default_port;
        host(port, "", name ? name : "Host");
    } else if (text.rfind("join:", 0) == 0) {
        join(after("join:"), "", name ? name : "Client");
    }
}

bool netplay::take_start_request() {
    std::lock_guard lock(g.mutex);
    g.ui_ready = true;
    bool request = g.start_request;
    g.start_request = false;
    if (request && !g.game_started) {
        g.game_started = true;
        recomp::start_game(supported_games[0].game_id);
    }
    return request;
}

bool netplay::in_game() {
    std::lock_guard lock(g.mutex);
    return g.in_game;
}

bool netplay::slot_present(int slot) {
    std::lock_guard lock(g.mutex);
    return slot >= 0 && slot < g.total_players;
}

void netplay::trace_rng(uint32_t sp, uint32_t a0, uint32_t index) {
    if (!g.trace_rng) return;
    // Buffered in its own file: printing every call slowed the game until the session dropped. The thread
    // too: a random number taken outside the game thread would differ between PCs.
    static FILE* out = std::fopen("netplay_rng.txt", "w");
    if (out == nullptr) return;
    static uint32_t flushed_frame = 0;
    if (g.frame != flushed_frame) { // the tests end the game by killing it
        std::fflush(out);
        flushed_frame = g.frame;
    }
    std::fprintf(out, "%u %08X %08X %u %zX\n", g.frame, sp, a0, index,
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
}

int32_t netplay::rng_seed(int32_t local_seed) {
    std::lock_guard lock(g.mutex);
    return g.in_game ? (int32_t)g.session : local_seed;
}

int netplay::cheat(int index, int local_value) {
    std::lock_guard lock(g.mutex);
    return g.in_game && index >= 0 && index < CHEATS ? g.cheats[index] : local_value;
}

void netplay::before_game_entry() {
    bool client;
    {
        std::lock_guard lock(g.mutex);
        client = g.in_game && g.role == Role::Client;
    }
    if (client) ultramodern::change_save_file(u8"netplay", u8"bm64_us");
}

int32_t netplay::sync_pads(uint8_t* rdram, int32_t pads_vram) {
    if (g.jitter_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::rand() % (g.jitter_ms + 1)));
    auto started = clock_type::now();
    std::unique_lock lock(g.mutex);
    if (!g.in_game) return -1;
    uint32_t pads = (uint32_t)pads_vram;
    clock_type::duration waited{};
    g.rdram = rdram;
    uint32_t f = g.frame;
    uint32_t target = f + g.delay;
    std::array<Pad, MAX> local{};
    for (int k = 0; k < g.players && k < (int)g.local_ports.size(); k++) {
        uint32_t pad = pads + 6 * g.local_ports[k];
        local[k] = { read_u16(rdram, pad), read_s8(rdram, pad + 2), read_s8(rdram, pad + 3) };
    }
    int32_t present = (1 << g.total_players) - 1;
    // Before any hash or copy of RAM: the pads still hold this PC's own reads until then.
    auto write_pads = [&](const FrameEntry& entry) {
        for (int slot = 0; slot < MAX; slot++) {
            uint32_t pad = pads + 6 * slot;
            bool here = (present >> slot) & 1;
            write_u16(rdram, pad, here ? entry.pads[slot].buttons : 0);
            write_u8(rdram, pad + 2, here ? (uint8_t)entry.pads[slot].x : 0);
            write_u8(rdram, pad + 3, here ? (uint8_t)entry.pads[slot].y : 0);
            write_u8(rdram, pad + 4, here ? 0 : PAD_NO_RESPONSE);
        }
    };
    if (g.role == Role::Host) {
        Peer& self = g.peers[0];
        self.inputs[target % RING] = local;
        self.input_frame[target % RING] = target + 1;
        auto have_all = [&] {
            if (f < (uint32_t)g.delay) return true;
            for (size_t i = 1; i < g.peers.size(); i++) {
                const Peer& peer = g.peers[i];
                if (!peer.dropped && peer.input_frame[f % RING] != f + 1) return false;
            }
            return true;
        };
        auto wait_start = clock_type::now();
        while (!have_all()) {
            g.cv.wait_for(lock, 5ms);
            auto now = clock_type::now();
            for (size_t i = 1; i < g.peers.size(); i++) {
                Peer& peer = g.peers[i];
                // Booting takes a while: until its first input a client has longer.
                auto timeout = peer.started ? PEER_TIMEOUT : 3 * PEER_TIMEOUT;
                if (!peer.dropped && peer.input_frame[f % RING] != f + 1 && now - peer.last_heard > timeout) {
                    peer.dropped = true;
                    set_message(peer.name + " stopped answering and was disconnected.");
                }
            }
        }
        waited = clock_type::now() - wait_start;
        FrameEntry entry{};
        for (const Peer& peer : g.peers) {
            if (f < (uint32_t)g.delay || peer.dropped) continue;
            for (int k = 0; k < peer.players; k++) entry.pads[peer.slot_base + k] = peer.inputs[f % RING][k];
        }
        if (g.reset_frame == f && f != 0) entry.flags |= FLAG_RESET;
        g.frames[f % RING] = entry;
        write_pads(entry);
        bool snapshot = false;
        for (const Peer& peer : g.peers) snapshot |= peer.resync_at == f && f != 0;
        if (snapshot) {
            g.snapshot.assign(rdram, rdram + RDRAM_SIZE);
            g.snapshot_hashes.resize(PAGES);
            for (uint32_t page = 0; page < PAGES; page++) g.snapshot_hashes[page] = page_hash(rdram, page);
            g.snapshot_frame = f;
        }
        uint64_t hash;
        if (advance_check(rdram, f, hash)) {
            g.latest_check_frame = f;
            g.latest_check_hash = hash;
        }
        g.frame = f + 1;
        for (size_t i = 1; i < g.peers.size(); i++) {
            if (!g.peers[i].dropped && g.peers[i].started) host_send_frames(g.peers[i]);
        }
        if (entry.flags & FLAG_RESET) {
            // The clients must see this frame before the host goes: send it a few more times.
            for (int i = 0; i < 10; i++) {
                g.cv.wait_for(lock, 50ms);
                for (size_t p = 1; p < g.peers.size(); p++) {
                    if (!g.peers[p].dropped && g.peers[p].started) host_send_frames(g.peers[p]);
                }
            }
            relaunch_session();
            return present;
        }
    } else {
        g.my_inputs[target % RING] = local;
        auto send_inputs = [&] {
            Input input = make<Input>(P_INPUT);
            uint32_t first = std::max<uint32_t>(g.delay, target + 1 >= WINDOW ? target + 1 - WINDOW : 0);
            input.first = first;
            input.count = (uint8_t)(target - first + 1);
            input.players = (uint8_t)g.players;
            for (uint32_t i = 0; i < input.count; i++) {
                std::copy(g.my_inputs[(first + i) % RING].begin(), g.my_inputs[(first + i) % RING].end(), input.pads[i]);
            }
            send_to_host(input, offsetof(Input, pads) + input.count * sizeof(input.pads[0]));
            if (g.resync_wanted != 0 && every(g.last_resync_request, 200ms)) {
                RsRequest request = make<RsRequest>(P_RS_REQUEST);
                request.frame = g.resync_wanted;
                send_to_host(request);
            }
            if (g.reset_requested) {
                Header reset{ MAGIC, P_RESET };
                send_to_host(reset);
            }
        };
        send_inputs();
        auto last_send = clock_type::now();
        auto wait_start = last_send;
        while (g.in_game && g.frame_known[f % RING] != f + 1) {
            g.cv.wait_for(lock, 5ms);
            if (clock_type::now() - last_send > 15ms) {
                send_inputs();
                last_send = clock_type::now();
            }
        }
        waited = clock_type::now() - wait_start;
        if (!g.in_game) return -1;
        write_pads(g.frames[f % RING]);
        if (g.frames[f % RING].flags & FLAG_RESET) {
            g.frame = f + 1;
            relaunch_session();
            return present;
        }
        if (g.frames[f % RING].flags & FLAG_RESYNC) {
            client_resync(lock, rdram, f);
            if (!g.in_game) return -1;
        }
        uint64_t hash;
        if (advance_check(rdram, f, hash) && f > g.skip_check_until) {
            g.own_checks[f] = hash;
            while (g.own_checks.size() > 16) g.own_checks.erase(g.own_checks.begin());
        }
        client_compare_checks();
        g.frame = f + 1;
    }
    if (std::find(g.dump_frames.begin(), g.dump_frames.end(), f) != g.dump_frames.end()) {
        std::ofstream dump("netplay_" + std::to_string(f) + ".bin", std::ios::binary);
        dump.write((const char*)rdram, RDRAM_SIZE);
    }
    if (g.check_log) {
        auto total = clock_type::now() - started;
        if (total > 8ms) {
            printf("NETPLAY slow frame %u: %lld ms, %lld ms of it waiting\n", f,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(total).count(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
        }
    }
    return present;
}
