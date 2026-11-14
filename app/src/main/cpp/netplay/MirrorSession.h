#ifndef MIRRORSESSION_H
#define MIRRORSESSION_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "types.h"
#include "NetplayCommon.h"
#include "AREngine.h"
#include "NetplayLink.h"

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

class MelonInstance;
class DeterministicMP;

/**
 * Runs N consoles on one device in lockstep frames, linked by DeterministicMP (docs/netplay-plan.md): the device's own
 * console (primary, shown and controlled here) and one hidden console for every other player (mirrors), whose input
 * arrives over the network. Every mirror runs on its own thread because local wireless needs all consoles to
 * progress concurrently. Software rendering only for mirrors: the picture must be identical on every device.
 *
 * Session ids (the same on every device): host console = 0, joiners in join order. The primary has id `selfSid`.
 */
class MirrorSession
{
public:
    static constexpr int kMaxPlayers = 8;

    ~MirrorSession();

    void stop();
    bool isActive() const { return active; }

    // Emulator thread: runs one frame on all consoles.
    melonDS::u32 runFrame();

    void requestNetStop();
    // Lobby path: every device already exchanged its own console state + firmware with every other one. Emulator thread,
    // at a frame boundary. `mirrors` holds one fresh instance for each other session id, ascending. `links`,
    // `remoteFirmware` and `remoteStates` are indexed by session id (the own entry is empty/null). The session ends
    // (and the mirrors are dropped) when any connection ends: see shouldEnd().
    bool startNetSession(std::shared_ptr<MelonInstance> primary, std::vector<std::shared_ptr<MelonInstance>> mirrors,
                         int selfSid, std::vector<std::unique_ptr<NetplayLink>> links, int inputDelay,
                         const std::vector<uint8_t>& ownState,
                         const std::vector<std::vector<uint8_t>>& remoteFirmware,
                         const std::vector<std::vector<uint8_t>>& remoteStates);
    bool shouldEnd() const { return active && netAutoStop && !netLinksUp(); }
    // Emulator thread, while the emulator is paused (pause menu, app in background): keeps the connections alive and
    // processes a pending Stop. The other devices wait for this one in the meantime.
    void servicePaused();
    // Thread-safe summary for the UI: "<phase> <frame> <desyncs> <message>"
    std::string netStatus();

    // Focus 0 = this device's own console, k >= 1 = the k-th other console (by session id)
    void setFocus(int index);
    int getFocus() const { return focus.load(); }
    MelonInstance* inputTarget();
    MelonInstance* getInstance(int index);
    MelonInstance* getInstanceUnchecked(int index);
    int playerCount() const { return (int) consoles.size(); }

private:
    static void setThreadedRendering(melonDS::NDS* nds, bool threaded);
    bool begin(std::shared_ptr<MelonInstance> primary, std::vector<std::shared_ptr<MelonInstance>> mirrors, int selfSid);
    void installLink();
    void inputHook(int sid, FrameInput& input);
    void workerLoop(MelonInstance* console);
    void waitForMirrors();

    // Netplay
    enum class NetPhase { Off, Running };
    struct InputSlot { uint32_t frame = 0xFFFFFFFF; FrameInput input {}; };
    struct HashEntry { uint32_t frame = 0; std::vector<uint64_t> hashes; };
    struct Peer
    {
        int sid = 0;
        std::unique_ptr<NetplayLink> link;
        std::deque<HashEntry> hashes;
        bool byeReceived = false;
    };
    static constexpr int kInputRing = 4096;
    bool netLinksUp() const;
    void handleNetRequest();
    bool netBeforeFrame();
    void netAfterFrame();
    void netInputHook(int sid, FrameInput& input);
    void netOnPacket(Peer& peer, const uint8_t* data, size_t size);
    void netSendInputs();
    void netEnd(const char* reason);
    void netBeginRunning();
    void netCompareHashes();
    bool servicePeers(int timeoutMs); // false when a peer is gone
    InputSlot& remoteSlot(int sid, uint32_t frame) { return netRemote[(size_t) sid * kInputRing + frame % kInputRing]; }
    bool loadStateFresh(MelonInstance* console, const uint8_t* data, size_t size);
    void restoreOwnSave(const std::vector<uint8_t>& saveMemory);

    // consoles[sid] for every session id; the primary is consoles[selfSid]
    std::vector<std::shared_ptr<MelonInstance>> consoles;
    MelonInstance* primary = nullptr;
    int selfSid = 0;
    DeterministicMP* mp = nullptr; // owned by melonDS::MPInterface

    std::atomic<bool> active { false };
    std::atomic<int> focus { 0 };

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t framesRequested = 0;
    uint64_t framesDone = 0; // summed over all workers
    uint64_t workerCount = 0;
    bool waitedThisFrame = false;
    bool quit = false;

    // Netplay state (emulator thread, except the request fields)
    std::vector<Peer> peers;
    std::mutex netRequestMutex;
    std::atomic<bool> netStopRequested { false };
    std::atomic<NetPhase> netPhase { NetPhase::Off };
    int netDelay = 2;
    uint32_t netFrame = 0;
    uint32_t netLocalLast = 0;      // newest local input frame produced (valid when netLocalCount > 0)
    uint32_t netLocalCount = 0;
    std::vector<InputSlot> netLocal;
    std::vector<InputSlot> netRemote; // [sid * kInputRing + slot]
    std::deque<HashEntry> netLocalHashes;
    uint32_t netDesyncs = 0;
    uint32_t netHashChecks = 0;
    uint64_t netStallMs = 0;
    uint32_t netFramesSinceLog = 0, netLateFramesSinceLog = 0;
    uint64_t netMaxWaitSinceLog = 0;
    uint32_t netLateBySid[16] = {};
    double netComputeMsSum = 0, netComputeMsMax = 0, netPrimaryMsSum = 0; uint32_t netComputeFrames = 0;
    std::atomic<uint32_t> netFrameShown { 0 };
    std::atomic<uint32_t> netDesyncsShown { 0 };
    std::atomic<bool> netStalled { false }; // waiting for another device's input
    std::string netMessage; // last end reason or error (netRequestMutex)
    bool netAutoStop = false;
    std::vector<melonDS::ARCode> savedCheats;
};

}

#endif // MIRRORSESSION_H
