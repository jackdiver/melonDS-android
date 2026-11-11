#ifndef MELONINSTANCE_H
#define MELONINSTANCE_H

#include <atomic>
#include <functional>
#include <string>
#include "Args.h"
#include "Configuration.h"
#include "NDS.h"
#include "MelonDS.h"
#include "SaveManager.h"
#include "RewindManager.h"
#include "renderer/FrameQueue.h"
#include "renderer/Renderer.h"
#include "renderer/ScreenshotRenderer.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"
#include "netplay/DeterminismHarness.h"

using namespace melonDS;

namespace MelonDSAndroid
{

// Points melonDS's per-thread "current console" at `nds` for the scope. The JIT (memory fault handling, invalidation)
// acts on NDS::Current, so any work on a console from a thread that is not running it (reset, savestate load) must
// set it, or it corrupts the console that thread normally runs.
struct ScopedCurrentNds
{
    melonDS::NDS* previous;
    explicit ScopedCurrentNds(melonDS::NDS* nds) : previous(melonDS::NDS::Current) { melonDS::NDS::Current = nds; }
    ~ScopedCurrentNds() { melonDS::NDS::Current = previous; }
    ScopedCurrentNds(const ScopedCurrentNds&) = delete;
    ScopedCurrentNds& operator=(const ScopedCurrentNds&) = delete;
};

class MelonInstance
{

public:
    MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType);
    ~MelonInstance();

    int getInstanceId() { return instanceId; };

    bool loadRom(std::string romPath, std::string sramPath);
    bool loadGbaRom(std::string romPath, std::string sramPath);
    void loadRumblePak();
    void loadGbaMemoryExpansion();
    void loadMotionPakHomebrew();
    void loadMotionPakRetail();
    bool bootFirmware();
    void start();
    void reset();
    melonDS::u32 runFrame();
    void stop();

    // Netplay mirror support (see docs/netplay-plan.md)
    // Runs one frame without touching OpenGL, saves, rewind or RetroAchievements. Safe on a non-GL thread.
    melonDS::u32 runFrameHeadless();
    // Turns this instance into an exact copy of `source` (cart + emulated state) and disables all save writes.
    bool cloneFrom(MelonInstance& source);
    // Neutral input (no keys, no touch)
    void clearInput();
    // When set, runFrame() displays this console's screens instead of its own (software renderer only)
    void setPresentationSource(std::function<melonDS::NDS*()> source) { presentationSource = std::move(source); }
    // When set, called with each frame's input before it is applied (may record it or replace it)
    void setInputHook(std::function<void(FrameInput&)> hook) { inputHook = std::move(hook); }
    melonDS::NDS* getNds() { return nds; }
    Renderer getCurrentRenderer() const { return currentRenderer; }

    void updateMotionData(float ax, float ay, float az, float rx, float ry, float rz);
    float getMotionData(MotionQueryType type);
    void touchScreen(u16 x, u16 y);
    void releaseScreen();
    void pressKey(u32 key);
    void releaseKey(u32 key);
    void syncRtcToSystem();
    int readAudioOutput(s16* buffer, int length);
    void setAudioOutputSkew(double skew);
    bool takeScreenshot();
    void loadCheats(std::list<Cheat> cheats);
    int sendNetPacket(u8* data, int length);
    int receiveNetPacket(u8* data);

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);

    void updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration);
    void requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    bool saveState(Savestate* state);
    bool loadState(Savestate* state);
    RewindWindow getRewindWindow();
    bool loadRewindState(RewindSaveState rewindSaveState);
    void setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript
    );
    void unloadRetroAchievementsData();
    std::string getRichPresenceStatus();
    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements();

private:
    void updateRenderer();
    void setBatteryLevels();
    void setDateTime();
    void applyFrameInput(bool useHarness = true);
    void saveRewindState(RewindSaveState* rewindSaveState);

private:
    int instanceId;
    int consoleType;
    NDS* nds;
    std::shared_ptr<Net> net;

    std::atomic<float> motionData[6] = { 0.0f, 0.0f, 9.80665f, 0.0f, 0.0f, 0.0f };

    std::unique_ptr<RetroAchievements::RetroAchievementsManager> retroAchievementsManager;
    std::unique_ptr<SaveManager> ndsSave;
    std::unique_ptr<SaveManager> gbaSave;
    std::unique_ptr<SaveManager> firmwareSave;

    // Live input from the UI thread. Latched and applied once per frame at the start of runFrame(),
    // so that input is frame-aligned (required for netplay determinism and recording).
    std::atomic<u32> inputMask;
    // bit 31 = touching, bits 16..27 = x, bits 0..11 = y
    std::atomic<u32> touchState;
    std::atomic<bool> lidClosed;
    bool appliedLidClosed;
    std::function<melonDS::NDS*()> presentationSource;
    std::function<void(FrameInput&)> inputHook;

    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    FrameQueue frameQueue;
    std::unique_ptr<ScreenshotRenderer> screenshotRenderer;
    RewindManager rewindManager;
    Renderer currentRenderer;
    bool isRenderConfigurationDirty;
    int frame;
};

}

#endif
