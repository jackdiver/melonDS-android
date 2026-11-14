#ifndef NETPLAYLOBBY_H
#define NETPLAYLOBBY_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "NetplayLink.h"

namespace melonDS { class NDS; class MPInterface; }

namespace MelonDSAndroid
{

class MelonInstance;
class LobbyMP;

/**
 * Starts 2 to 8 player netplay from the game's own wireless menus (see docs/netplay-plan.md).
 *
 * While no session runs, the console's wireless goes to LobbyMP, which only watches what the game does:
 * - the game sends beacons    -> it created a group: advertise this device on the Wi-Fi network and accept joiners
 * - wireless on, no beacons   -> it is looking for groups: connect to an advertised host with the same game
 * The roster is fixed when the host starts (automatically a few seconds after the last joiner, or at once on request):
 * the host sends every joiner its session id and the addresses of the other joiners, the devices link up with each
 * other (full mesh), every device sends its own console state and firmware to every other, and all start a lockstep
 * session. Session ids: host = 0, joiners in the order they joined.
 *
 * Emulator thread only, except status() and requestStart().
 */
class NetplayLobby
{
public:
    enum class Action { RunFrame, HoldFrame, StartSession };

    struct SessionStart
    {
        int selfSid = 0;
        int count = 0;
        std::vector<std::unique_ptr<NetplayLink>> links;   // by session id (own entry null)
        std::vector<uint8_t> ownState;
        std::vector<std::vector<uint8_t>> firmware;       // by session id
        std::vector<std::vector<uint8_t>> states;         // by session id
    };

    NetplayLobby();
    ~NetplayLobby();

    // Called before each frame while no session is running.
    Action poll(MelonInstance& console, bool softwareRenderer);
    SessionStart takeSessionStart() { return std::move(pending); }
    // Drop sockets and pending connections (session started, emulator stopped...).
    void reset();
    // Host: start now with the players that joined so far (thread-safe)
    void requestStart() { startRequested = true; }
    // Most players this device can run (own console + hidden copies); the host start and joins are limited by it
    void setMaxPlayers(int value) { maxPlayers = value; }

    // "<phase> 0 0 <message>": phase is idle|hosting|searching|joining|error
    std::string status();

private:
    enum class Phase { Idle, Hosting, Searching, Joined, Exchanging };
    struct Joiner { std::unique_ptr<NetplayLink> link; int slot = 0; };

    void installMp();
    bool openUdp(bool listen);
    void closeUdp();
    void advertise(uint32_t gameId);
    bool findHost(uint32_t gameId, std::string& address, uint16_t& port);
    bool snapshot(melonDS::NDS* nds, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware);
    void sendHello(melonDS::NDS* nds);
    bool checkHello(melonDS::NDS* nds, const uint8_t* data, size_t size, std::string& error);
    void sendState(NetplayLink& to, const std::vector<uint8_t>& state, const std::vector<uint8_t>& firmware);
    bool readState(const uint8_t* data, size_t size, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware);
    void setMessage(const std::string& message);
    void setPhase(Phase phase);
    bool openListener();
    void beginExchange(int self, int count);
    Action startAsHost(melonDS::NDS* nds, uint64_t now);
    Action exchange(uint64_t now);

    LobbyMP* mp = nullptr; // owned by melonDS::MPInterface while installed
    Phase phase = Phase::Idle;
    std::atomic<int> shownPhase { 0 };
    bool isHost = false;
    int udpSocket = -1;
    uint64_t lastAdvertMs = 0;
    uint64_t lastConnectMs = 0;
    uint64_t exchangeStartMs = 0;
    uint64_t rejectUntilMs = 0;
    uint64_t lastJoinMs = 0;
    std::atomic<bool> startRequested { false };
    int maxPlayers = 8;

    // Host: joiners that said hello, plus one open listener for the next one
    std::vector<Joiner> joiners;
    std::unique_ptr<NetplayLink> listener;
    int listenerSlot = 0;
    std::unique_ptr<NetplayLink> rejected;
    // Joiner: link to the host
    std::unique_ptr<NetplayLink> hostLink;
    std::string hostAddress;
    uint16_t hostPort = 0;
    bool welcomed = false;

    // Exchange (all roles)
    int selfSid = 0;
    int playerCount = 0;
    std::vector<std::unique_ptr<NetplayLink>> links; // by session id
    std::vector<char> stateSent, stateGot;
    std::vector<std::vector<uint8_t>> peerFirmware, peerState;
    std::vector<uint8_t> ownState;
    std::vector<uint8_t> ownFirmware;
    SessionStart pending;

    std::mutex messageMutex;
    std::string message;
};

}

#endif // NETPLAYLOBBY_H
