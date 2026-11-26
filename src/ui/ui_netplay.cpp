// The Netplay page (assets/netplay.rml): host or join a session, see the lobby, start the game (include/netplay.h).
// The fields are kept in netplay.json in the app folder.
#include "recomp_ui.h"
#include "netplay.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "RmlUi/Core.h"
#include "json/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {

struct RoomView {
    std::string name;
    std::string players;
    std::string state;
};

struct NetplayModel {
    std::string name = "Player";
    std::string port = "7777";
    std::string relay;    // host: optional relay address
    bool listed = true;   // host through a relay: the room shows in the relay's room list
    std::string address;  // join: the host's or the relay's address
    std::string room;     // join: room code when joining through a relay
    std::string message;
    std::string room_code;
    std::string phase_text;
    std::vector<std::string> peers;
    bool idle = true;     // no session: show the host and join forms
    bool is_host = false;
    bool can_start = false;
    int total_players = 0;
    bool looking = false;  // join without a room code: asking the address for its room list
    bool browsing = false; // the relay answered: its rooms are shown
    std::vector<RoomView> rooms;
} model;

// The room list query runs on its own thread (netplay::list_rooms blocks); update_netplay_menu takes its result.
std::mutex query_mutex;
bool query_done = false, query_answered = false;
std::vector<netplay::RoomListing> query_rooms, shown_rooms;
std::string query_error;

void start_room_query(const std::string& address) {
    model.looking = true;
    model.message = "Looking for rooms at " + address + "...";
    std::thread([address]() {
        std::vector<netplay::RoomListing> rooms;
        std::string error;
        bool answered = netplay::list_rooms(address, rooms, error);
        std::lock_guard lock(query_mutex);
        query_rooms = std::move(rooms);
        query_error = error;
        query_answered = answered;
        query_done = true;
    }).detach();
}

Rml::DataModelHandle handle;
recompui::ContextId netplay_context;

std::filesystem::path settings_path() {
    return zelda64::get_app_folder_path() / "netplay.json";
}

void load_settings() {
    std::ifstream file(settings_path());
    if (!file.good()) return;
    nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    if (!json.is_object()) return;
    model.name = json.value("name", model.name);
    model.port = json.value("port", model.port);
    model.relay = json.value("relay", model.relay);
    model.address = json.value("address", model.address);
    model.room = json.value("room", model.room);
    model.listed = json.value("listed", model.listed);
}

void save_settings() {
    nlohmann::json json{
        {"name", model.name}, {"port", model.port}, {"relay", model.relay}, {"address", model.address}, {"room", model.room},
        {"listed", model.listed},
    };
    std::ofstream file(settings_path());
    file << json.dump(4);
}

std::string trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t");
    size_t end = text.find_last_not_of(" \t");
    return start == std::string::npos ? "" : text.substr(start, end - start + 1);
}

const char* phase_name(netplay::Phase phase) {
    switch (phase) {
    case netplay::Phase::Connecting: return "Connecting";
    case netplay::Phase::Lobby: return "Lobby";
    case netplay::Phase::Starting:
    case netplay::Phase::InGame: return "In game";
    case netplay::Phase::Ended: return "Ended";
    default: return "";
    }
}

} // namespace

recompui::ContextId recompui::get_netplay_context_id() {
    return netplay_context;
}

// Called every UI frame: copies the session's state into the page.
void recompui::update_netplay_menu() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (!handle || now - last < std::chrono::milliseconds(200)) return;
    last = now;
    netplay::Status status = netplay::status();
    bool done = false, answered = false;
    {
        std::lock_guard lock(query_mutex);
        if (query_done) {
            done = true;
            answered = query_answered;
            query_done = false;
            shown_rooms = query_rooms;
        }
    }
    if (done && model.looking) {
        model.looking = false;
        if (answered) { // a relay: pick a room
            model.browsing = true;
            model.rooms.clear();
            for (const netplay::RoomListing& room : shown_rooms) {
                model.rooms.push_back({ room.name, std::to_string(room.players) + (room.players == 1 ? " player" : " players"),
                                        room.in_game ? "in game" : "in the lobby" });
            }
            handle.DirtyVariable("np_rooms");
            handle.DirtyVariable("np_browsing");
        } else { // no relay there: a host to join directly
            netplay::join(trim(model.address), "", trim(model.name));
        }
        handle.DirtyVariable("np_looking");
        status = netplay::status();
    }
    if (model.looking || model.browsing) {
        model.message = model.looking ? model.message
            : model.rooms.empty() ? "No open rooms on this relay. Ask the host for the room code."
            : "Rooms on the relay:";
        handle.DirtyVariable("np_message");
        if (!(status.phase == netplay::Phase::Idle || status.phase == netplay::Phase::Ended)) model.browsing = false;
        else return;
    }
    model.message = status.message;
    model.room_code = status.room;
    model.phase_text = phase_name(status.phase);
    model.idle = status.phase == netplay::Phase::Idle || status.phase == netplay::Phase::Ended;
    model.is_host = status.role == netplay::Role::Host;
    model.can_start = status.can_start;
    model.total_players = status.total_players;
    model.peers.clear();
    for (size_t i = 0; i < status.peers.size(); i++) {
        const netplay::LobbyPeer& peer = status.peers[i];
        std::string line = peer.name + (i == 0 ? " (Host)" : "") + ": " + std::to_string(peer.players) +
            (peer.players == 1 ? " player" : " players") + (i != 0 && !peer.ready ? ", getting the save..." : "");
        model.peers.push_back(line);
    }
    for (const char* name : { "np_message", "np_room_code", "np_phase", "np_idle", "np_is_host", "np_can_start",
                              "np_total_players", "np_peers" }) {
        handle.DirtyVariable(name);
    }
}

class NetplayMenu : public recompui::MenuController {
public:
    void load_document() override {
        netplay_context = recompui::create_context(zelda64::get_asset_path("netplay.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "netplay_host", [](const std::string&, Rml::Event&) {
            save_settings();
            int port = std::atoi(model.port.c_str());
            netplay::host(port > 0 && port < 65536 ? (uint16_t)port : netplay::default_port, trim(model.relay), trim(model.name),
                          model.listed);
        });
        recompui::register_event(listener, "netplay_join", [](const std::string&, Rml::Event&) {
            save_settings();
            if (model.looking) return;
            if (trim(model.room).empty()) {
                // No room code: a relay there shows its listed rooms; anything else is a host joined directly.
                start_room_query(trim(model.address));
                handle.DirtyVariable("np_looking");
                handle.DirtyVariable("np_message");
                return;
            }
            netplay::join(trim(model.address), trim(model.room), trim(model.name));
        });
        recompui::register_event(listener, "netplay_rooms_refresh", [](const std::string&, Rml::Event&) {
            if (!model.looking) start_room_query(trim(model.address));
            handle.DirtyVariable("np_looking");
        });
        recompui::register_event(listener, "netplay_rooms_close", [](const std::string&, Rml::Event&) {
            model.browsing = false;
            model.message.clear();
            handle.DirtyVariable("np_browsing");
            handle.DirtyVariable("np_message");
        });
        recompui::register_event(listener, "netplay_start", [](const std::string&, Rml::Event&) {
            netplay::start();
        });
        recompui::register_event(listener, "netplay_leave", [](const std::string&, Rml::Event&) {
            netplay::leave();
        });
        recompui::register_event(listener, "netplay_back", [](const std::string&, Rml::Event&) {
            recompui::hide_all_contexts();
            recompui::show_context(recompui::get_launcher_context_id(), "");
        });
    }
    void make_bindings(Rml::Context* context) override {
        load_settings();
        Rml::DataModelConstructor constructor = context->CreateDataModel("netplay_model");
        constructor.RegisterArray<std::vector<std::string>>();
        if (auto room = constructor.RegisterStruct<RoomView>()) {
            room.RegisterMember("name", &RoomView::name);
            room.RegisterMember("players", &RoomView::players);
            room.RegisterMember("state", &RoomView::state);
        }
        constructor.RegisterArray<std::vector<RoomView>>();
        constructor.Bind("np_listed", &model.listed);
        constructor.Bind("np_looking", &model.looking);
        constructor.Bind("np_browsing", &model.browsing);
        constructor.Bind("np_rooms", &model.rooms);
        constructor.BindEventCallback("np_set_listed", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& inputs) {
            model.listed = !inputs.empty() && inputs.at(0).Get<int>() != 0;
            h.DirtyVariable("np_listed");
            save_settings();
        });
        constructor.BindEventCallback("np_join_room", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& inputs) {
            size_t i = inputs.empty() ? shown_rooms.size() : inputs.at(0).Get<size_t>();
            if (i >= shown_rooms.size()) return;
            model.browsing = false;
            h.DirtyVariable("np_browsing");
            netplay::join(trim(model.address), std::to_string(shown_rooms[i].room), trim(model.name));
        });
        constructor.Bind("np_name", &model.name);
        constructor.Bind("np_port", &model.port);
        constructor.Bind("np_relay", &model.relay);
        constructor.Bind("np_address", &model.address);
        constructor.Bind("np_room", &model.room);
        constructor.Bind("np_message", &model.message);
        constructor.Bind("np_room_code", &model.room_code);
        constructor.Bind("np_phase", &model.phase_text);
        constructor.Bind("np_peers", &model.peers);
        constructor.Bind("np_idle", &model.idle);
        constructor.Bind("np_is_host", &model.is_host);
        constructor.Bind("np_can_start", &model.can_start);
        constructor.Bind("np_total_players", &model.total_players);
        handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_netplay_menu() {
    return std::make_unique<NetplayMenu>();
}
