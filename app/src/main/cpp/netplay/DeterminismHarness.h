#ifndef DETERMINISMHARNESS_H
#define DETERMINISMHARNESS_H

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "types.h"

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

// Input applied to the emulated console for one frame. Recorded/replayed as-is.
struct FrameInput
{
    melonDS::u32 keyMask;
    melonDS::u16 touchX;
    melonDS::u16 touchY;
    melonDS::u8 touching;
    melonDS::u8 lidClosed;
    melonDS::u8 reserved[2];
};
static_assert(sizeof(FrameInput) == 12, "FrameInput must stay 12 bytes (file format)");

/**
 * Netplay phase 1 test tool: checks that emulation is deterministic across devices.
 *
 * Record: saves a savestate, then writes every frame's input and a hash of the console state.
 * Replay: loads that savestate, feeds the recorded inputs (ignoring live input) and writes hashes.
 * Identical hash files from two devices mean lockstep netplay is viable.
 *
 * Files in the chosen directory: state.bin, inputs.bin, env-<tag>.txt, hashes-<tag>.txt.
 */
class DeterminismHarness
{
public:
    // Thread-safe requests (UI/JNI thread). Executed at the start of the next emulated frame.
    void requestRecord(const std::string& directory);
    void requestReplay(const std::string& directory, const std::string& tag);
    void requestStop();
    // Same-device check: every frame, snapshot -> run -> hash A, then rewind to the snapshot,
    // run the same frame again -> hash B. A != B means savestates/emulation are not deterministic.
    void requestSelfTest(int frames);

    bool isActive() const { return mode.load() != Mode::Off; }

    // Emulator thread only.
    void beforeFrame(melonDS::NDS* nds, FrameInput& input);
    void afterFrame(melonDS::NDS* nds);

    static melonDS::u64 hashState(melonDS::NDS* nds);
    static void applyInput(melonDS::NDS* nds, const FrameInput& input);

private:
    enum class Mode { Off, Record, Replay, SelfTest };
    enum class Request { None, Record, Replay, Stop, SelfTest };

    std::vector<melonDS::u8> selfTestSnapshot;
    FrameInput selfTestInput {};
    int selfTestFramesLeft = 0;
    int selfTestRequestedFrames = 0;
    int selfTestMismatches = 0;

    void startRecord(melonDS::NDS* nds);
    void startReplay(melonDS::NDS* nds);
    void stop(const char* reason);
    void writeEnvironment(melonDS::NDS* nds, const std::string& tag);

    std::mutex requestMutex;
    std::atomic<Request> pendingRequest { Request::None };
    std::string requestDirectory;
    std::string requestTag;

    std::atomic<Mode> mode { Mode::Off };
    std::string directory;
    FILE* inputFile = nullptr;
    FILE* hashFile = nullptr;
    melonDS::u32 frameIndex = 0;
};

extern DeterminismHarness determinismHarness;

}

#endif // DETERMINISMHARNESS_H
