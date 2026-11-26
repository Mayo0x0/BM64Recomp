#ifndef __NETPLAY_H__
#define __NETPLAY_H__

#include <cstdint>
#include <string>
#include <vector>

// Lockstep netplay: every PC runs the whole game; the host collects each frame's pads of all PCs and sends them
// back, so all PCs feed the game the same pads. The sync point is the game's controller poll (patches/eightslots.c).
// Desyncs are found by a hash of RAM every CHECK_INTERVAL frames and repaired by copying the host's differing pages.
namespace netplay {
    constexpr int max_players = 8;
    constexpr uint16_t default_port = 7777;
    constexpr uint16_t default_relay_port = 7778;

    enum class Role { None, Host, Client };
    enum class Phase { Idle, Connecting, Lobby, Starting, InGame, Ended };

    struct LobbyPeer {
        std::string name;
        int players;
        bool ready;
    };

    struct Status {
        Role role = Role::None;
        Phase phase = Phase::Idle;
        std::string message;       // last error or info for the UI
        std::string room;          // relay room code (host through a relay)
        std::vector<LobbyPeer> peers; // host first
        int total_players = 0;
        bool can_start = false;    // host: everyone has the save
        uint32_t frame = 0;
        int resyncs = 0;
    };

    // Lobby. relay is "" for a direct connection, else "host[:port]" of a relay server.
    // listed: a relay shows the room in its room list (list_rooms); else it is only joined by its code.
    bool host(uint16_t port, const std::string& relay, const std::string& name, bool listed = false);
    bool join(const std::string& address, const std::string& relay_room, const std::string& name);
    struct RoomListing {
        uint32_t room;
        std::string name; // the host's
        int players;
        bool in_game;
    };
    // Asks a relay ("host[:port]") for its listed rooms; blocks up to about 1.5 s. False when no relay answered.
    bool list_rooms(const std::string& relay, std::vector<RoomListing>& rooms, std::string& error);
    bool start(); // host: starts the game on all PCs
    void refresh_levels(); // host in the lobby: sends the custom levels again (include/level_editor.h)
    // Resets the game on every PC of the session: each starts the program again into the main menu (restart.h).
    void request_reset();
    void leave();
    Status status();

    // Set from the environment for unattended tests: BM64_NETPLAY=host[:port] | join:address[:port] |
    // relayhost:relay[:port] | relayjoin:relay[:port]:room, BM64_NETPLAY_PLAYERS=n starts the host once n players are in,
    // BM64_NETPLAY_LISTED=1 lists a relay host's room.
    void init_from_environment();

    // Called by every UI frame: marks the UI as running (the game may only start then) and starts the game if the
    // session asked for it earlier. True once after the session started the game, to hide the menus.
    bool take_start_request();

    // Game side.
    bool in_game();
    // Called on the game thread at every controller poll. pads: the 8 pads (OSContPad, 6 bytes each) at a guest
    // address, ports read locally; overwritten with the session's pads. Returns the bit mask of slots in use.
    int32_t sync_pads(uint8_t* rdram, int32_t pads_vram);
    bool slot_present(int slot);
    int32_t rng_seed(int32_t local_seed);
    // BM64_NETPLAY_TRACERNG: logs a random number taken (the caller's sp and a0, index = 0x802ACFE0).
    void trace_rng(uint32_t sp, uint32_t a0, uint32_t index);
    int cheat(int index, int local_value);
    // Wraps the game's entry point: a client switches to the host's save first.
    void before_game_entry();
}

#endif
