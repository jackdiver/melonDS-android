#ifndef MIRRORSESSION_H
#define MIRRORSESSION_H

#include <atomic>
#include <condition_variable>
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
 * Runs two consoles on one device in lockstep frames, linked by DeterministicMP (docs/netplay-plan.md):
 * the device's own console (primary, shown and controlled here) and the other player's console (mirror), whose
 * input arrives over the network. The mirror runs on its own thread because local wireless needs both consoles
 * to progress concurrently. Software rendering only: the picture must be identical on every device.
 */
class MirrorSession
{
public:
    ~MirrorSession();

    // Emulator thread, at a frame boundary. The primary must use the software renderer.
    bool start(std::shared_ptr<MelonInstance> primary, std::shared_ptr<MelonInstance> mirror);
    void stop();
    bool isActive() const { return active; }

    // Emulator thread: runs one frame on both consoles.
    melonDS::u32 runFrame();

    // Manual start from the Local multiplayer screen. Host = console 0, joiner = console 1.
    void requestNetHost(int port, int inputDelay);
    void requestNetJoin(const std::string& address, int port, int inputDelay);
    void requestNetStop();
    // Lobby path: both devices already exchanged their own console state + firmware. Emulator thread, at a
    // frame boundary. The session ends (and the mirror is dropped) when the connection ends: see shouldEnd().
    bool startNetSession(std::shared_ptr<MelonInstance> primary, std::shared_ptr<MelonInstance> mirror,
                         std::unique_ptr<NetplayLink> link, bool isHost, int inputDelay,
                         const std::vector<uint8_t>& ownState,
                         const std::vector<uint8_t>& remoteFirmware, const std::vector<uint8_t>& remoteState);
    bool shouldEnd() const { return active && netAutoStop && !net; }
    // Thread-safe summary for the UI: "<phase> <frame> <desyncs> <message>"
    std::string netStatus();

    void setFocus(int index);
    int getFocus() const { return focus.load(); }
    MelonInstance* inputTarget();
    MelonInstance* getInstance(int index);
    MelonInstance* getInstanceUnchecked(int index);

private:
    static void setThreadedRendering(melonDS::NDS* nds, bool threaded);
    void installLink();
    void inputHook(int index, FrameInput& input);
    void workerLoop();
    void waitForMirror();

    // Netplay
    enum class NetPhase { Off, Listening, Connecting, AwaitState, AwaitReady, Running };
    struct InputSlot { uint32_t frame = 0xFFFFFFFF; FrameInput input {}; };
    static constexpr int kInputRing = 4096;
    void handleNetRequest();
    bool netBeforeFrame();
    void netAfterFrame();
    void netInputHook(int index, FrameInput& input);
    void netOnPacket(const uint8_t* data, size_t size);
    void netSendInputs();
    void netEnd(const char* reason);
    void netBeginRunning();
    bool netSyncAsHost();
    bool saveStateTo(int index, std::vector<uint8_t>& out);
    bool loadStateFresh(int index, const uint8_t* data, size_t size);
    uint32_t romHash();

    std::shared_ptr<MelonInstance> primary;
    std::shared_ptr<MelonInstance> mirror;
    DeterministicMP* mp = nullptr; // owned by melonDS::MPInterface

    std::atomic<bool> active { false };
    std::atomic<int> focus { 0 };

    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t framesRequested = 0;
    uint64_t framesDone = 0;
    bool waitedThisFrame = false;
    bool quit = false;

    // Netplay state (emulator thread, except the request fields)
    std::unique_ptr<NetplayLink> net;
    std::mutex netRequestMutex;
    std::atomic<int> netRequest { 0 }; // 0 none, 1 host, 2 join, 3 stop
    std::string netRequestAddress;
    int netRequestPort = 7064;
    int netRequestDelay = 2;
    std::atomic<NetPhase> netPhase { NetPhase::Off };
    bool netIsHost = false;
    int netLocalIndex = 0;
    int netDelay = 2;
    uint32_t netFrame = 0;
    uint32_t netLocalLast = 0;      // newest local input frame produced (valid when netLocalCount > 0)
    uint32_t netLocalCount = 0;
    InputSlot netLocal[kInputRing];
    InputSlot netRemote[kInputRing];
    bool netHelloOk = false;
    bool netReady = false;
    std::vector<uint8_t> netStateRx;
    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> netLocalHashes;
    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> netRemoteHashes;
    uint32_t netDesyncs = 0;
    uint32_t netHashChecks = 0;
    uint64_t netStallMs = 0;
    std::atomic<uint32_t> netFrameShown { 0 };
    std::atomic<uint32_t> netDesyncsShown { 0 };
    std::string netMessage; // last end reason or error (netRequestMutex)
    // Session ids (host console = 0). Default 0/1; a joiner started from the lobby runs its own console as 1.
    int sidPrimary = 0;
    int sidMirror = 1;
    bool netAutoStop = false;
    std::vector<melonDS::ARCode> savedCheats;
    MelonInstance* consoleBySession(int sid) { return sid == sidPrimary ? primary.get() : mirror.get(); }
};

}

#endif // MIRRORSESSION_H
