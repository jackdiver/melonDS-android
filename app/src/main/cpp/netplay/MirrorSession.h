#ifndef MIRRORSESSION_H
#define MIRRORSESSION_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "types.h"
#include "DeterminismHarness.h"

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
};

}

#endif // MIRRORSESSION_H
