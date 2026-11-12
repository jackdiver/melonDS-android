#include <cstring>
#include <utility>
#include <android/asset_manager.h>
#include <oboe/Oboe.h>
#include "EmulatorArgsBuilder.h"
#include "MelonDS.h"
#include "MelonDSAudio.h"
#include "OboeCallback.h"
#include "MicInputOboeCallback.h"
#include "OpenGLContext.h"
#include "mic_blow.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"
#include "GBACart.h"
#include "SPU.h"
#include "Platform.h"
#include "Savestate.h"
#include "MelonInstance.h"
#include "RewindManager.h"
#include "ROMManager.h"
#include "MPInterface.h"
#include "AndroidCameraHandler.h"
#include "renderer/ScreenshotRenderer.h"
#include "renderer/FrameQueue.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"
#include "net/Net_Slirp.h"
#include <fstream>
#include "netplay/MirrorSession.h"

namespace MelonDSAndroid
{
    OpenGLContext *openGlContext;
    AndroidFileHandler* fileHandler;
    AndroidCameraHandler* cameraHandler;
    std::string internalFilesDir;
    std::shared_ptr<MelonEventMessenger> eventMessenger;
    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    std::shared_ptr<Net> net;

    std::shared_ptr<MelonInstance> instance;
    MirrorSession mirrorSession;
    std::atomic<int> pendingMirrorRequest { 0 }; // 1 = start, 2 = stop
    int mirrorDebugFlags = 0; // DEBUG: 1 = mirror uses interpreter, 2 = run consoles one after the other

    bool setupOpenGlContext();
    void cleanupOpenGlContext();

    /**
     * Used to set the emulator's initial configuration, before boot. To update the configuration during runtime, use @updateEmulatorConfiguration.
     *
     * @param emulatorConfiguration The emulator configuration during the next emulator run
     */
    void setConfiguration(EmulatorConfiguration emulatorConfiguration) {
        currentConfiguration = std::make_shared<EmulatorConfiguration>(std::move(emulatorConfiguration));
        internalFilesDir = currentConfiguration->internalFilesDir;

        net = std::make_shared<Net>();
        net->SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net->RXEnqueue(data, len);
        }));
    }

    void setup(AndroidCameraHandler* androidCameraHandler, std::shared_ptr<MelonEventMessenger> androidEventMessenger, u32* screenshotBufferPointer, int instanceId)
    {
        cameraHandler = androidCameraHandler;
        eventMessenger = androidEventMessenger;
        RetroAchievements::RetroAchievementsManager::EventMessenger = androidEventMessenger;

        auto instanceArgs = BuildArgsFromConfiguration(*currentConfiguration, instanceId);
        if (!instanceArgs.has_value())
        {
            // TODO: Handle this somehow?
            instance = nullptr;
            return;
        }
        instance = std::make_shared<MelonInstance>(
            instanceId,
            currentConfiguration,
            std::move(instanceArgs.value()),
            net,
            std::make_unique<ScreenshotRenderer>(screenshotBufferPointer),
            currentConfiguration->consoleType
        );

        setupAudio(currentConfiguration->audioSettings);
        setAudioActiveInstance(instance);
    }

    void setCodeList(std::list<Cheat> cheats)
    {
        instance->loadCheats(std::move(cheats));
    }

    void setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript
    )
    {
        instance->setupAchievements(std::move(achievements), std::move(leaderboards), std::move(richPresenceScript));
    }

    void unloadRetroAchievementsData()
    {
        instance->unloadRetroAchievementsData();
    }

    std::string getRichPresenceStatus()
    {
        return instance->getRichPresenceStatus();
    }

    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements()
    {
        return instance->getRuntimeAchievements();
    }

    /**
     * Used to update the emulator's configuration during runtime. Will only update the configurations that can actually change during runtime without causing issues,
     *
     * @param emulatorConfiguration The new emulator configuration
     */
    void updateEmulatorConfiguration(std::unique_ptr<EmulatorConfiguration> emulatorConfiguration) {
        std::shared_ptr<EmulatorConfiguration> sharedConfig = std::move(emulatorConfiguration);
        instance->updateConfiguration(sharedConfig);
        updateAudioSettings(sharedConfig->audioSettings);

        currentConfiguration = sharedConfig;
    }

    int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
        if (!instance->loadRom(std::move(romPath), std::move(sramPath)))
            return 2;

        if (gbaSlotConfig->type == GBA_ROM)
        {
            RomGbaSlotConfigGbaRom* gbaRomConfig = (RomGbaSlotConfigGbaRom*) gbaSlotConfig;
            if (!instance->loadGbaRom(gbaRomConfig->romPath, gbaRomConfig->savePath))
                return 1;
        }
        else if (gbaSlotConfig->type == RUMBLE_PAK)
        {
            instance->loadRumblePak();
        }
        else if (gbaSlotConfig->type == MEMORY_EXPANSION)
        {
            instance->loadGbaMemoryExpansion();
        }
        else if (gbaSlotConfig->type == MOTION_PAK_HOMEBREW)
        {
            instance->loadMotionPakHomebrew();
        }
        else if (gbaSlotConfig->type == MOTION_PAK_RETAIL)
        {
            instance->loadMotionPakRetail();
        }

        return 0;
    }

    int bootFirmware()
    {
        // TODO: Maybe validate BIOS and firmware?
        if (instance->bootFirmware())
            return ROMManager::SUCCESS;
        else
            return ROMManager::FIRMWARE_NOT_BOOTABLE;
    }

    // Input goes to the focused console while a netplay mirror session runs
    MelonInstance* inputTarget()
    {
        if (mirrorSession.isActive())
            return mirrorSession.inputTarget();
        return instance.get();
    }

    void touchScreen(u16 x, u16 y)
    {
        if (auto* target = inputTarget())
            target->touchScreen(x, y);
    }

    void releaseScreen()
    {
        if (auto* target = inputTarget())
            target->releaseScreen();
    }

    void pressKey(u32 key)
    {
        if (auto* target = inputTarget())
            target->pressKey(key);
    }

    void releaseKey(u32 key)
    {
        if (auto* target = inputTarget())
            target->releaseKey(key);
    }

    void updateMotionData(float ax, float ay, float az, float rx, float ry, float rz)
    {
        if (instance)
            instance->updateMotionData(ax, ay, az, rx, ry, rz);
    }

    void start()
    {
        startAudio();
        setupOpenGlContext();

        instance->start();
    }

    void handleMirrorRequest()
    {
        int request = pendingMirrorRequest.exchange(0);
        if (request == 1 && !mirrorSession.isActive())
        {
            auto mirrorArgs = BuildArgsFromConfiguration(*currentConfiguration, 1);
            if (!mirrorArgs.has_value())
                return;
            // DEBUG experiment switch: mirror on the interpreter
            if (mirrorDebugFlags & 1)
                mirrorArgs.value()->JIT = std::nullopt;
            auto mirror = std::make_shared<MelonInstance>(
                1,
                currentConfiguration,
                std::move(mirrorArgs.value()),
                net,
                std::make_unique<ScreenshotRenderer>(nullptr),
                currentConfiguration->consoleType
            );
            mirrorSession.start(instance, mirror);
        }
        else if (request == 2)
        {
            mirrorSession.stop();
        }
    }

    void requestMirrorStart(int debugFlags) { mirrorDebugFlags = debugFlags; mirrorSession.setDebugFlags(debugFlags); pendingMirrorRequest = 1; }
    void requestMirrorStop() { pendingMirrorRequest = 2; }
    void setMirrorFocus(int index) { mirrorSession.setFocus(index); }
    void requestMirrorRecord(std::string directory) { mirrorSession.requestRecord(directory); }
    void requestMirrorReplay(std::string directory, std::string tag) { mirrorSession.requestReplay(directory, tag); }
    void requestMirrorRecordStop() { mirrorSession.requestStopRecording(); }
    void requestNetHost(int port, int inputDelay) { mirrorSession.requestNetHost(port, inputDelay); }
    void requestNetJoin(std::string address, int port, int inputDelay) { mirrorSession.requestNetJoin(address, port, inputDelay); }
    void requestNetStop() { mirrorSession.requestNetStop(); }

    u32 loop()
    {
        handleMirrorRequest();
        if (mirrorSession.isActive())
            return mirrorSession.runFrame();

        MPInterface::Get().Process();
        return instance->runFrame();
    }

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
    {
        if (!instance)
            return nullptr;

        return instance->getPresentationFrame(deadline);
    }

    void pause()
    {
        pauseAudio();
    }

    void resume()
    {
        if (instance && currentConfiguration && currentConfiguration->syncRtcOnResume)
            instance->syncRtcToSystem();

        startAudio();
    }

    void reset()
    {
        instance->reset();
    }

    bool saveState(const char* path)
    {
        Platform::FileHandle* saveStateFile = Platform::OpenFile(path, Platform::FileMode::Write);

        if (!saveStateFile)
            return false;

        Savestate state;
        if (state.Error)
        {
            Platform::CloseFile(saveStateFile);
            return false;
        }

        instance->saveState(&state);

        if (state.Error)
        {
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (Platform::FileWrite(state.Buffer(), state.Length(), 1, saveStateFile) == 0)
        {
            Platform::Log(Platform::Error, "Failed to write %d-byte savestate to %s\n", state.Length(), path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        Platform::CloseFile(saveStateFile);
        return true;
    }

    bool loadState(const char* path)
    {
        auto saveStateFile = Platform::OpenFile(path, Platform::FileMode::Read);
        if (!saveStateFile)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to open state file \"%s\"\n", path);
            return false;
        }

        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup\n");
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (!instance->saveState(backup.get()) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting load (from \"%s\")\n", path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        size_t size = Platform::FileLength(saveStateFile);

        // Allocate exactly as much memory as we need for the savestate
        std::vector<u8> buffer(size);
        if (Platform::FileRead(buffer.data(), size, 1, saveStateFile) == 0)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to read %u-byte state file \"%s\"\n", size, path);
            Platform::CloseFile(saveStateFile);
            return false;
        }
        Platform::CloseFile(saveStateFile);

        std::unique_ptr<Savestate> state = std::make_unique<Savestate>(buffer.data(), size, false);

        if (!instance->loadState(state.get()) || state->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load state file \"%s\" into emulator\n", path);
            // Restore backup
            if (!instance->loadState(backup.get()) || state->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state\n", path);
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded\n", path);

            return false;
        }
        return true;
    }

    bool loadRewindState(melonDS::RewindSaveState rewindSaveState)
    {
        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup");
            return false;
        }

        if (!instance->saveState(backup.get()) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting rewind state load");
            return false;
        }

        bool result = instance->loadRewindState(rewindSaveState);
        if (!result)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load rewind state");
            // Restore backup
            if (!instance->loadState(backup.get()) || backup->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state");
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded");
        }

        return result;
    }

    RewindWindow getRewindWindow()
    {
        return instance->getRewindWindow();
    }

    bool takeScreenshot()
    {
        if (instance)
            return instance->takeScreenshot();

        return false;
    }

    void stop()
    {
        mirrorSession.stop();
        instance->stop();
        cleanupOpenGlContext();
    }

    void cleanup()
    {
        cleanupAudio();

        instance = nullptr;
        eventMessenger = nullptr;
    }

    bool setupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return false;

        if (!openGlContext->Use())
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to use OpenGL context");
            return false;
        }

        return true;
    }

    void cleanupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return;

        openGlContext->Release();
    }
}
