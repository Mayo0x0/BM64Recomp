// Netplay relay for BM64Recompiled (src/game/netplay.cpp): forwards UDP packets between a host and its clients
// when the clients cannot reach the host directly. A host registers and gets a room code; clients send to the
// relay with that code. The relay never looks into the game's packets. A host may also list its room: its
// registration then carries the host's name and player count, and R_LIST answers with every listed room.
// Hole punching: a client sends R_INTRO; the relay answers both sides with R_PEER (the other's public and local
// address and a token), and the two then try to reach each other directly. The relay stays the fallback.
//
// Build: c++ -std=c++17 -O2 relay.cpp -o bm64_relay   (Windows: add -lws2_32, or use the CMake target)
// Run:   bm64_relay [port]   (default 7778, UDP)
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#endif

namespace {

using clock_type = std::chrono::steady_clock;
constexpr uint32_t RELAY_MAGIC = 0x52384D42; // "BM8R", as in netplay.cpp
enum RelayOp : uint8_t {
    R_REGISTER = 1, R_REGISTERED, R_TO_HOST, R_FROM_CLIENT, R_TO_CLIENT, R_FROM_HOST, R_ERROR, R_LIST, R_ROOMS,
    R_INTRO, R_PEER
};
constexpr size_t NAME_LEN = 24;
constexpr auto TIMEOUT = std::chrono::seconds(60);
constexpr auto LISTED_SILENCE = std::chrono::seconds(6);

#pragma pack(push, 1)
struct RelayHeader { uint32_t magic; uint8_t op; uint32_t room; uint32_t client; };
// R_REGISTER's payload (optional; older hosts send none and are not listed).
struct RoomInfo { uint8_t listed; uint8_t players; uint8_t in_game; char name[NAME_LEN]; };
// R_ROOMS: a count, then that many entries.
struct RoomEntry { uint32_t room; uint8_t players; uint8_t in_game; char name[NAME_LEN]; };
struct Endpoint { uint32_t ip; uint16_t port; }; // network byte order; R_REGISTER (after RoomInfo) and R_INTRO carry the sender's LAN address
struct PeerInfo { uint64_t token; Endpoint pub; Endpoint local; }; // R_PEER
#pragma pack(pop)
constexpr size_t MAX_LISTED = (4096 - sizeof(RelayHeader) - 1) / sizeof(RoomEntry);

struct Client { sockaddr_in addr; clock_type::time_point seen; uint64_t token = 0; Endpoint local{}; };
struct Room {
    sockaddr_in host;
    clock_type::time_point seen;
    std::map<uint32_t, Client> clients;
    uint32_t next_client = 1;
    RoomInfo info{}; // listed = 0: only joined by its code
    Endpoint host_local{};
};

bool same(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

socket_t sock;
std::map<uint32_t, Room> rooms;
std::mt19937 random_codes{ std::random_device{}() };

void send_packet(const sockaddr_in& to, uint8_t op, uint32_t room, uint32_t client, const uint8_t* payload, size_t size) {
    uint8_t buffer[4096];
    if (sizeof(RelayHeader) + size > sizeof(buffer)) return;
    RelayHeader header{ RELAY_MAGIC, op, room, client };
    std::memcpy(buffer, &header, sizeof(header));
    if (size) std::memcpy(buffer + sizeof(header), payload, size);
    sendto(sock, (const char*)buffer, (int)(sizeof(header) + size), 0, (const sockaddr*)&to, sizeof(to));
}

void send_error(const sockaddr_in& to, const char* text) {
    send_packet(to, R_ERROR, 0, 0, (const uint8_t*)text, std::strlen(text) + 1);
}

Endpoint endpoint(const sockaddr_in& addr) {
    return { addr.sin_addr.s_addr, addr.sin_port };
}

// The relay's id of the client at from, a new one for a new address.
uint32_t client_id(Room& room, const sockaddr_in& from, clock_type::time_point now) {
    uint32_t id = 0;
    for (auto& [client_id, client] : room.clients) {
        if (same(client.addr, from)) id = client_id;
    }
    if (id == 0) {
        id = room.next_client++;
        room.clients[id] = Client{ from, now };
    }
    room.clients[id].seen = now;
    return id;
}

void handle(const sockaddr_in& from, const uint8_t* data, size_t size) {
    if (size < sizeof(RelayHeader)) return;
    RelayHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != RELAY_MAGIC) return;
    const uint8_t* payload = data + sizeof(header);
    size_t payload_size = size - sizeof(header);
    auto now = clock_type::now();
    switch (header.op) {
    case R_REGISTER: {
        // A host keeps its room by registering again every few seconds.
        auto it = rooms.find(header.room);
        if (header.room == 0 || it == rooms.end() || !same(it->second.host, from)) {
            uint32_t code;
            do { code = 100000 + random_codes() % 900000; } while (rooms.count(code));
            it = rooms.emplace(code, Room{ from, now, {}, 1 }).first;
            printf("room %u opened\n", code);
        }
        it->second.seen = now;
        if (payload_size >= sizeof(RoomInfo)) {
            std::memcpy(&it->second.info, payload, sizeof(RoomInfo));
            it->second.info.name[NAME_LEN - 1] = 0;
        }
        if (payload_size >= sizeof(RoomInfo) + sizeof(Endpoint)) {
            std::memcpy(&it->second.host_local, payload + sizeof(RoomInfo), sizeof(Endpoint));
        }
        send_packet(from, R_REGISTERED, it->first, 0, nullptr, 0);
        return;
    }
    case R_LIST: {
        uint8_t list[1 + MAX_LISTED * sizeof(RoomEntry)];
        uint8_t count = 0;
        for (auto& [code, room] : rooms) {
            // A host registers every 2 s: one silent for longer is gone or stuck, though its room stays a while.
            if (!room.info.listed || count == MAX_LISTED || now - room.seen > LISTED_SILENCE) continue;
            RoomEntry entry{ code, room.info.players, room.info.in_game, {} };
            std::memcpy(entry.name, room.info.name, NAME_LEN);
            std::memcpy(list + 1 + count * sizeof(RoomEntry), &entry, sizeof(entry));
            count++;
        }
        list[0] = count;
        send_packet(from, R_ROOMS, 0, 0, list, 1 + count * sizeof(RoomEntry));
        return;
    }
    case R_TO_HOST: {
        auto it = rooms.find(header.room);
        if (it == rooms.end()) {
            send_error(from, "Room not found.");
            return;
        }
        Room& room = it->second;
        uint32_t id = client_id(room, from, now);
        send_packet(room.host, R_FROM_CLIENT, header.room, id, payload, payload_size);
        return;
    }
    case R_INTRO: {
        auto it = rooms.find(header.room);
        if (it == rooms.end()) return;
        Room& room = it->second;
        uint32_t id = client_id(room, from, now);
        Client& client = room.clients[id];
        if (client.token == 0) client.token = ((uint64_t)random_codes() << 32 | random_codes()) | 1;
        if (payload_size >= sizeof(Endpoint)) std::memcpy(&client.local, payload, sizeof(Endpoint));
        PeerInfo to_client{ client.token, endpoint(room.host), room.host_local };
        PeerInfo to_host{ client.token, endpoint(from), client.local };
        send_packet(from, R_PEER, header.room, id, (const uint8_t*)&to_client, sizeof(to_client));
        send_packet(room.host, R_PEER, header.room, id, (const uint8_t*)&to_host, sizeof(to_host));
        return;
    }
    case R_TO_CLIENT: {
        auto it = rooms.find(header.room);
        if (it == rooms.end() || !same(it->second.host, from)) return;
        it->second.seen = now;
        auto client = it->second.clients.find(header.client);
        if (client != it->second.clients.end()) {
            send_packet(client->second.addr, R_FROM_HOST, header.room, 0, payload, payload_size);
        }
        return;
    }
    }
}

void forget_silent() {
    auto now = clock_type::now();
    for (auto it = rooms.begin(); it != rooms.end();) {
        if (now - it->second.seen > TIMEOUT) {
            printf("room %u closed\n", it->first);
            it = rooms.erase(it);
            continue;
        }
        for (auto client = it->second.clients.begin(); client != it->second.clients.end();) {
            client = now - client->second.seen > TIMEOUT ? it->second.clients.erase(client) : std::next(client);
        }
        ++it;
    }
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : 7778;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock, (const sockaddr*)&local, sizeof(local)) != 0) {
        fprintf(stderr, "cannot open UDP port %u\n", port);
        return 1;
    }
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    BOOL report = FALSE; // an ICMP "port unreachable" must not stop the receiving
    DWORD returned = 0;
    WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12), &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    timeval timeout{ 1, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("BM64 netplay relay on UDP port %u\n", port);
    auto last_cleanup = clock_type::now();
    for (;;) {
        uint8_t buffer[4096];
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        int received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &length);
        if (received > 0) handle(from, buffer, (size_t)received);
        if (clock_type::now() - last_cleanup > std::chrono::seconds(5)) {
            forget_silent();
            last_cleanup = clock_type::now();
        }
    }
}
