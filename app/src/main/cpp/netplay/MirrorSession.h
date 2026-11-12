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
#include "DeterminismHarness.h"
#include "NetplayLink.h"

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

class MelonInstance;
class DeterministicMP;

/**
 * Runs the primary console plus a hidden "mirror" console in lockstep frames, linked by DeterministicMP.
 *
 * Phase 2 of docs/netplay-plan.md: both consoles live on one device. The mirror runs on its own thread
 * (local wireless needs both to progress concurrently) and always renders in software.
 * A focus switch decides which console is shown and receives touch/button input; the other gets
 * neutral input. In phase 3 the mirror's input will come from the network instead.
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

    // Two-console determinism check (thread-safe requests, executed at the next frame boundary).
    // Record: snapshot both consoles, reload them (cold JIT), reset the link, then log both inputs + hashes.
    // Replay: load both snapshots, reset the link, feed the recorded inputs and log hashes.
    void requestRecord(const std::string& directory);
    void requestReplay(const std::string& directory, const std::string& tag);
    void requestStopRecording();

    // Phase 3: two-device netplay (debug commands for now). Host = console 0, joiner = console 1.
    void requestNetHost(int port, int inputDelay);
    void requestNetJoin(const std::string& address, int port, int inputDelay);
    void requestNetStop();
    // Thread-safe summary for the UI: "<phase> <frame> <desyncs> <message>"
    std::string netStatus();

    void setDebugFlags(int flags) { debugFlags = flags; }
    void setFocus(int index);
    int getFocus() const { return focus.load(); }
    MelonInstance* inputTarget();
    MelonInstance* getInstance(int index);
    MelonInstance* getInstanceUnchecked(int index);

private:
    static void setFastMemory(melonDS::NDS* nds, bool enabled);
    static void setThreadedRendering(melonDS::NDS* nds, bool threaded);
    void installLink();
    void handleRecordingRequest();
    bool snapshotAndReload(int index, const std::string& path, bool save);
    void inputHook(int index, FrameInput& input);
    void stopRecording(const char* reason);
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
    bool netSyncAsHost();
    bool saveStateTo(int index, std::vector<uint8_t>& out);
    bool loadStateFresh(int index, const uint8_t* data, size_t size);
    uint32_t romHash();

    std::shared_ptr<MelonInstance> primary;
    std::shared_ptr<MelonInstance> mirror;
    DeterministicMP* mp = nullptr; // owned by melonDS::MPInterface

    std::atomic<bool> active { false };
    std::atomic<int> focus { 0 };
    std::atomic<int> debugFlags { 0 };

    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t framesRequested = 0;
    uint64_t framesDone = 0;
    bool waitedThisFrame = false;
    bool quit = false;

    // Recording / replay (two-console determinism check)
    enum class RecMode { Off, Record, Replay };
    enum class RecRequest { None, Record, Replay, Stop };
    std::mutex recRequestMutex;
    std::atomic<RecRequest> recRequest { RecRequest::None };
    std::string recRequestDir;
    std::string recRequestTag;
    std::atomic<RecMode> recMode { RecMode::Off };
    FILE* inputFiles[2] = { nullptr, nullptr };
    FILE* hashFile = nullptr;
    std::atomic<bool> replayEnded { false };
    uint32_t recFrame = 0;
    std::string recDir;
    std::string recTag;
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

    std::vector<std::string> refHashes;   // DEBUG capture
    std::vector<uint32_t> dumpFrames;
    int capturesWritten = 0;
};

}

#endif // MIRRORSESSION_H
