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
 * Starts two-player netplay from the game's own wireless menus (phase 4, see docs/netplay-plan.md).
 *
 * While no session runs, the console's wireless goes to LobbyMP, which only watches what the game does:
 * - the game sends beacons    -> it created a group: advertise this device on the Wi-Fi network and accept a joiner
 * - wireless on, no beacons   -> it is looking for groups: connect to an advertised host with the same game
 * When a host and a joiner meet, each sends its own console state and firmware to the other, and both start a
 * lockstep session in which the joiner's game sees the host's group through the deterministic in-process link.
 *
 * Emulator thread only, except status().
 */
class NetplayLobby
{
public:
    enum class Action { RunFrame, HoldFrame, StartSession };

    struct SessionStart
    {
        bool isHost = false;
        std::unique_ptr<NetplayLink> link;
        std::vector<uint8_t> ownState;
        std::vector<uint8_t> remoteFirmware;
        std::vector<uint8_t> remoteState;
    };

    NetplayLobby();
    ~NetplayLobby();

    // Called before each frame while no session is running.
    Action poll(MelonInstance& console, bool softwareRenderer);
    SessionStart takeSessionStart() { return std::move(pending); }
    // Drop sockets and pending connections (session started, emulator stopped...).
    void reset();

    // "<phase> 0 0 <message>": phase is idle|hosting|searching|joining|error
    std::string status();

private:
    enum class Phase { Idle, Hosting, Searching, Exchanging };

    void installMp();
    bool openUdp(bool listen);
    void closeUdp();
    void advertise(uint32_t gameId);
    bool findHost(uint32_t gameId, std::string& address);
    bool snapshot(melonDS::NDS* nds, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware);
    void sendHello(melonDS::NDS* nds);
    bool checkHello(melonDS::NDS* nds, const uint8_t* data, size_t size, std::string& error);
    void sendState(const std::vector<uint8_t>& state, const std::vector<uint8_t>& firmware);
    bool readState(const uint8_t* data, size_t size, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware);
    void setMessage(const std::string& message);
    void setPhase(Phase phase);

    LobbyMP* mp = nullptr; // owned by melonDS::MPInterface while installed
    Phase phase = Phase::Idle;
    std::atomic<int> shownPhase { 0 };
    bool isHost = false;
    std::unique_ptr<NetplayLink> link;
    int udpSocket = -1;
    uint64_t lastAdvertMs = 0;
    uint64_t lastConnectMs = 0;
    uint64_t exchangeStartMs = 0;
    std::string hostAddress;
    bool helloReceived = false;
    bool stateSent = false;
    std::vector<uint8_t> receivedPacket;
    std::vector<uint8_t> ownState;
    std::vector<uint8_t> ownFirmware;
    SessionStart pending;

    std::mutex messageMutex;
    std::string message;
};

}

#endif // NETPLAYLOBBY_H
