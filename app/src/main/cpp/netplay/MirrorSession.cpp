#include "MirrorSession.h"

#include <android/log.h>
#include <pthread.h>
#include "DeterministicMP.h"
#include "NetplayLink.h"
#include "NDSCart.h"
#include "NetplayCommon.h"
#include <time.h>
#include "MelonInstance.h"
#include "net/MPInterface.h"
#include "Savestate.h"
#include "GPU3D_Soft.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#define TAG "NetplayMirror"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace melonDS;

namespace MelonDSAndroid
{

MirrorSession::~MirrorSession()
{
    stop();
}

void MirrorSession::setThreadedRendering(NDS* nds, bool threaded)
{
    if (nds->GPU.GetRenderer3D().Accelerated)
        return;
    static_cast<SoftRenderer&>(nds->GPU.GetRenderer3D()).SetThreaded(threaded, nds->GPU);
}

bool MirrorSession::start(std::shared_ptr<MelonInstance> primaryInstance, std::shared_ptr<MelonInstance> mirrorInstance)
{
    if (active)
        stop();

    // Threaded 3D rendering is deterministic (replays match across phones); the mismatches once blamed on it
    // came from the divider/sqrt savestate gap. It roughly halves frame time during races.
    setThreadedRendering(primaryInstance->getNds(), true);

    if (!mirrorInstance->cloneFrom(*primaryInstance))
    {
        LOGE("failed to clone the primary console into the mirror");
        return false;
    }

    primaryInstance->setRewindBlocked(true); // rewinding would change one console behind the other device's back
    primary = std::move(primaryInstance);
    mirror = std::move(mirrorInstance);
    mirror->useMirrorRenderer(); // draws nothing unless the game captures the screen into VRAM

    // GBA slot accessories (Motion Pak, rumble...) read live phone sensors: never allowed in netplay
    for (int i = 0; i < 2; i++)
    {
        NDS* nds = getInstanceUnchecked(i)->getNds();
        auto* gbaCart = nds->GetGBACart();
        if (gbaCart)
            nds->EjectGBACart();
    }

    installLink();
    forceSilentMic = true;
    primary->setInputHook([this](FrameInput& input) { inputHook(sidPrimary, input); });
    mirror->setInputHook([this](FrameInput& input) { inputHook(sidMirror, input); });

    primary->setPresentationSource([this]() -> NDS* {
        if (focus.load() == 1)
        {
            // Show the mirror: its frame must be complete before its framebuffer is uploaded
            waitForMirror();
            return mirror->getNds();
        }
        return primary->getNds();
    });

    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = false;
        framesRequested = 0;
        framesDone = 0;
    }
    worker = std::thread(&MirrorSession::workerLoop, this);
    pthread_setname_np(worker.native_handle(), "MirrorEmuThread");

    focus = 0;
    active = true;
    LOGI("mirror session started (primary inst %d, mirror inst %d)", primary->getInstanceId(), mirror->getInstanceId());
    return true;
}

void MirrorSession::stop()
{
    if (!active)
        return;

    active = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
    }
    if (mp)
        mp->shutdown();
    cv.notify_all();
    if (worker.joinable())
        worker.join();

    netEnd("session stopped");
    if (netAutoStop && primary)
        primary->getNds()->AREngine.Cheats = savedCheats;
    forceSilentMic = false;
    primary->setRewindBlocked(false);
    primary->setInputHook(nullptr);
    primary->setPresentationSource(nullptr);
    primary->clearInput();
    MPInterface::Set(MPInterface_Dummy);
    mp = nullptr;
    mirror.reset();
    primary.reset();
    LOGI("mirror session stopped");
    sidPrimary = 0;
    sidMirror = 1;
    netAutoStop = false;
}

void MirrorSession::workerLoop()
{
    int mirrorId = mirror->getInstanceId();
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return quit || framesRequested > framesDone; });
            if (quit)
                return;
        }

        mp->setAtBarrier(mirrorId, false);
        mirror->runFrameHeadless();
        mp->setAtBarrier(mirrorId, true);

        {
            std::lock_guard<std::mutex> lock(mutex);
            framesDone++;
        }
        cv.notify_all();
    }
}

void MirrorSession::waitForMirror()
{
    if (waitedThisFrame)
        return;

    // Park the primary at the frame barrier so the mirror can finish (and so deadlocks resolve)
    mp->setAtBarrier(primary->getInstanceId(), true);
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return quit || framesDone >= framesRequested; });
    waitedThisFrame = true;
}

void MirrorSession::installLink()
{
    // A fresh link: its state (inboxes, connection times) is not part of savestates, so every session starts
    // from an empty one
    auto link = std::make_unique<DeterministicMP>();
    link->mapInstance(primary->getInstanceId(), sidPrimary);
    link->mapInstance(mirror->getInstanceId(), sidMirror);
    link->registerInstance(primary->getInstanceId(), primary->getNds());
    link->registerInstance(mirror->getInstanceId(), mirror->getNds());
    for (MelonInstance* console : { primary.get(), mirror.get() })
        if (console->getNds()->Wifi.IsPowerOn())
            link->markConnected(console->getInstanceId());
    mp = link.get();
    MPInterface::SetCustom(std::move(link), MPInterface_Netplay);
}

// Runs on the thread of console `sid` at the start of its frame
void MirrorSession::inputHook(int sid, FrameInput& input)
{
    if (netPhase.load() == NetPhase::Running)
        netInputHook(sid, input);
}

u32 MirrorSession::runFrame()
{
    handleNetRequest();
    if (net)
        netBeforeFrame();

    auto kickMirror = [this] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            framesRequested++;
            waitedThisFrame = false;
        }
        cv.notify_all();
    };

    // Both consoles are marked running before either starts: otherwise one can briefly look parked at the
    // previous frame end and the link's deadlock rule would release the other early, depending on thread timing
    mp->setAtBarrier(primary->getInstanceId(), false);
    mp->setAtBarrier(mirror->getInstanceId(), false);
    kickMirror();

    u32 lines = primary->runFrame();
    waitForMirror();

    if (netPhase.load() == NetPhase::Running)
        netAfterFrame();
    return lines;
}

void MirrorSession::setFocus(int index)
{
    int previous = focus.exchange(index == 1 ? 1 : 0);
    if (previous != focus.load())
    {
        // Release anything held on the console that loses focus
        MelonInstance* old = getInstance(previous);
        if (old)
            old->clearInput();
        LOGI("focus -> console %d", focus.load());
    }
}

MelonInstance* MirrorSession::inputTarget()
{
    return getInstance(focus.load());
}

MelonInstance* MirrorSession::getInstanceUnchecked(int index)
{
    return index == 1 ? mirror.get() : primary.get();
}

MelonInstance* MirrorSession::getInstance(int index)
{
    if (!active)
        return nullptr;
    return index == 1 ? mirror.get() : primary.get();
}


// ---------------------------------------------------------------------------------------------------------------
// Phase 3: two-device netplay. Both devices run both consoles; only inputs travel over the network.
// Host = console 0, joiner = console 1. Each side's local input is delayed by `netDelay` frames so the other side
// normally has it before it is needed; if not, the frame waits (stall).
// ---------------------------------------------------------------------------------------------------------------
namespace
{
enum NetMsg : uint8_t { MsgInput = 4, MsgHash = 5 };
constexpr int kInputRedundancy = 16;
constexpr uint32_t kHashInterval = 60;
constexpr uint64_t kStallGiveUpMs = 600000; // the other device may be paused; a lost connection is detected by the link itself
const FrameInput kNeutralInput { 0xFFF, 0, 0, 0, 0, {0, 0} };

uint64_t nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

template <typename T> void put(std::vector<uint8_t>& out, const T& value)
{
    const uint8_t* p = (const uint8_t*) &value;
    out.insert(out.end(), p, p + sizeof(T));
}
template <typename T> bool get(const uint8_t*& p, const uint8_t* end, T& value)
{
    if ((size_t) (end - p) < sizeof(T)) return false;
    memcpy(&value, p, sizeof(T));
    p += sizeof(T);
    return true;
}
}

void MirrorSession::requestNetStop()
{
    netStopRequested = true;
}

void MirrorSession::handleNetRequest()
{
    if (netStopRequested.exchange(false))
        netEnd("stopped");
}

void MirrorSession::netEnd(const char* reason)
{
    if (netPhase.load() == NetPhase::Off && !net)
        return;
    {
        std::lock_guard<std::mutex> lock(netRequestMutex);
        netMessage = reason;
    }
    if (netPhase.load() == NetPhase::Running)
        LOGI("netplay ended at frame %u: %s (hash checks %u, desyncs %u, total stall %llu ms)", netFrame, reason,
             netHashChecks, netDesyncs, (unsigned long long) netStallMs);
    else
        LOGI("netplay ended: %s", reason);
    if (net)
    {
        net->close();
        net.reset();
    }
    netPhase = NetPhase::Off;
}

bool MirrorSession::loadStateFresh(int index, const uint8_t* data, size_t size)
{
    // Same path as snapshotAndReload: reset to launch defaults, then load (cold JIT, no leftover state)
    NDS* nds = getInstanceUnchecked(index)->getNds();
    ScopedCurrentNds currentGuard(nds);
    nds->Reset();
    std::string romName;
    nds->SetupDirectBoot(romName);
    nds->Start();
    std::vector<u8> copy(data, data + size);
    Savestate load(copy.data(), (u32) copy.size(), false);
    bool ok = !load.Error && nds->DoSavestate(&load) && !load.Error;
    getInstanceUnchecked(index)->clearInput();
    return ok;
}

void MirrorSession::netOnPacket(const uint8_t* data, size_t size)
{
    if (size < 1) return;
    const uint8_t* p = data + 1;
    const uint8_t* end = data + size;
    switch (data[0])
    {
    case MsgInput:
    {
        uint32_t first; uint8_t count;
        if (!get(p, end, first) || !get(p, end, count)) return;
        for (uint32_t i = 0; i < count; i++)
        {
            FrameInput in;
            if (!get(p, end, in)) return;
            InputSlot& slot = netRemote[(first + i) % kInputRing];
            slot.frame = first + i;
            slot.input = in;
        }
        break;
    }
    case MsgHash:
    {
        uint32_t frame; uint64_t h0, h1;
        if (!get(p, end, frame) || !get(p, end, h0) || !get(p, end, h1)) return;
        netRemoteHashes.push_back({ frame, { h0, h1 } });
        break;
    }
    default:
        break;
    }
}

void MirrorSession::netSendInputs()
{
    if (netLocalCount == 0) return;
    uint32_t count = std::min<uint32_t>(netLocalCount, kInputRedundancy);
    uint32_t first = netLocalLast + 1 - count;
    std::vector<uint8_t> msg;
    msg.push_back(MsgInput);
    put(msg, first);
    put(msg, (uint8_t) count);
    for (uint32_t f = first; f <= netLocalLast; f++)
        put(msg, netLocal[f % kInputRing].input);
    net->send(msg.data(), msg.size(), false);
    net->flush();
}

// Emulator thread, before the frame: waits until the other device's input for this frame has arrived.
// Returns true when the frame can run (also when the session just ended).
bool MirrorSession::netBeforeFrame()
{
    auto onPacket = [this](const uint8_t* d, size_t s) { netOnPacket(d, s); };

    uint64_t start = 0, lastSend = 0;
    for (;;)
    {
        NetplayLink::Event ev = net->service(0, onPacket);
        if (ev == NetplayLink::Event::Disconnected || !net->isConnected())
        {
            netEnd("peer disconnected");
            return true;
        }
        if (netRemote[netFrame % kInputRing].frame == netFrame)
            break;
        if (netStopRequested.exchange(false))
        {
            netEnd("stopped");
            return true;
        }

        uint64_t now = nowMs();
        if (start == 0) start = lastSend = now;
        if (now - start > 1000)
            netStalled = true;
        if (now - lastSend >= 16)
        {
            netSendInputs(); // resend in case packets were lost
            lastSend = now;
        }
        if (now - start > kStallGiveUpMs)
        {
            netEnd("other device stopped sending input");
            return true;
        }
        // Short sleep until something arrives. A disconnect reported here must not be lost: it is picked up by the
        // isConnected() check at the top of the loop.
        net->service(2, onPacket);
    }
    netStalled = false;
    if (start != 0)
    {
        uint64_t stalled = nowMs() - start;
        netStallMs += stalled;
        if (stalled > 100)
            LOGI("netplay: waited %llu ms for input of frame %u", (unsigned long long) stalled, netFrame);
    }
    return true;
}

// Runs on the thread of console `index` at the start of its frame
void MirrorSession::netInputHook(int index, FrameInput& input)
{
    if (index == netLocalIndex)
    {
        // The live input is applied `netDelay` frames from now (on both devices)
        uint32_t target = netFrame + netDelay;
        netLocal[target % kInputRing] = InputSlot { target, input };
        netLocalLast = target;
        netLocalCount++;
        const InputSlot& slot = netLocal[netFrame % kInputRing];
        input = slot.frame == netFrame ? slot.input : kNeutralInput;
    }
    else
    {
        const InputSlot& slot = netRemote[netFrame % kInputRing];
        input = slot.frame == netFrame ? slot.input : kNeutralInput;
    }
}

void MirrorSession::netAfterFrame()
{
    netSendInputs();

    if (netFrame % kHashInterval == 0)
    {
        uint64_t h0 = hashConsoleState(consoleBySession(0)->getNds());
        uint64_t h1 = hashConsoleState(consoleBySession(1)->getNds());
        netLocalHashes.push_back({ netFrame, { h0, h1 } });
        std::vector<uint8_t> msg { MsgHash };
        put(msg, netFrame);
        put(msg, h0);
        put(msg, h1);
        net->send(msg.data(), msg.size(), true);
    }

    // Compare whatever hashes both sides have
    while (!netLocalHashes.empty() && !netRemoteHashes.empty())
    {
        auto& l = netLocalHashes.front();
        auto& r = netRemoteHashes.front();
        if (l.first < r.first) { netLocalHashes.erase(netLocalHashes.begin()); continue; }
        if (r.first < l.first) { netRemoteHashes.erase(netRemoteHashes.begin()); continue; }
        netHashChecks++;
        if (l.second != r.second)
        {
            netDesyncs++;
            netDesyncsShown = netDesyncs;
            LOGE("netplay: DESYNC at frame %u (console 0 %s, console 1 %s)", l.first,
                 l.second.first == r.second.first ? "ok" : "differs", l.second.second == r.second.second ? "ok" : "differs");
        }
        else if (netHashChecks % 10 == 0)
            LOGI("netplay: frame %u in sync (%u checks, %u desyncs, stall %llu ms)", l.first, netHashChecks, netDesyncs,
                 (unsigned long long) netStallMs);
        netLocalHashes.erase(netLocalHashes.begin());
        netRemoteHashes.erase(netRemoteHashes.begin());
    }

    netFrame++;
    netFrameShown = netFrame;
}

}

namespace MelonDSAndroid
{
std::string MirrorSession::netStatus()
{
    const char* phase = "off";
    switch (netPhase.load())
    {
    case NetPhase::Off: phase = "off"; break;
    case NetPhase::Running: phase = netStalled.load() ? "stalled" : "running"; break;
    }
    std::lock_guard<std::mutex> lock(netRequestMutex);
    return std::string(phase) + " " + std::to_string(netFrameShown.load()) + " " + std::to_string(netDesyncsShown.load()) + " " + netMessage;
}
}

namespace MelonDSAndroid
{
void MirrorSession::netBeginRunning()
{
    // Session starts at frame 0 on both devices
    netFrame = 0;
    for (InputSlot& s : netLocal) s = InputSlot();
    for (InputSlot& s : netRemote) s = InputSlot();
    // The first `netDelay` frames use neutral input on both sides
    for (uint32_t f = 0; f < (uint32_t) netDelay; f++)
    {
        netLocal[f % kInputRing] = InputSlot { f, FrameInput { 0xFFF, 0, 0, 0, 0, {0, 0} } };
        netRemote[f % kInputRing] = InputSlot { f, FrameInput { 0xFFF, 0, 0, 0, 0, {0, 0} } };
    }
    netLocalLast = netDelay - 1;
    netLocalCount = netDelay;
    netLocalHashes.clear();
    netRemoteHashes.clear();
    netDesyncs = netHashChecks = 0;
    netStallMs = 0;
    netFrameShown = 0;
    netDesyncsShown = 0;
    netPhase = NetPhase::Running;
    LOGI("netplay: running (%s, console %d is local)", netIsHost ? "host" : "client", netLocalIndex);
}

bool MirrorSession::startNetSession(std::shared_ptr<MelonInstance> primaryInstance, std::shared_ptr<MelonInstance> mirrorInstance,
                                    std::unique_ptr<NetplayLink> link, bool isHost, int inputDelay,
                                    const std::vector<uint8_t>& ownState,
                                    const std::vector<uint8_t>& remoteFirmware, const std::vector<uint8_t>& remoteState)
{
    // Each device keeps its own console as the primary instance; the other player's console is the mirror.
    // Session ids are the same on both devices: host console = 0, joiner console = 1.
    sidPrimary = isHost ? 0 : 1;
    sidMirror = 1 - sidPrimary;
    if (!start(std::move(primaryInstance), std::move(mirrorInstance)))
    {
        sidPrimary = 0;
        sidMirror = 1;
        return false;
    }

    {
        // Firmware (MAC, nickname, settings) is not part of savestates: the other player's console needs theirs
        ScopedCurrentNds guard(mirror->getNds());
        mirror->getNds()->SetFirmware(Firmware(remoteFirmware.data(), (u32) remoteFirmware.size()));
    }
    // This device's own console keeps this device's own save: loading a state writes the state's save data back
    // to the save file, so remember the save memory first and verify it is unchanged afterwards.
    std::vector<uint8_t> ownSaveBefore;
    {
        const NDSCart::CartCommon* cart = primary->getNds()->NDSCartSlot.GetCart();
        if (cart && cart->GetSaveMemory())
            ownSaveBefore.assign(cart->GetSaveMemory(), cart->GetSaveMemory() + cart->GetSaveMemoryLength());
    }
    if (!loadStateFresh(0, ownState.data(), ownState.size()) || !loadStateFresh(1, remoteState.data(), remoteState.size()))
    {
        LOGE("netplay: could not load the session states");
        restoreOwnSave(ownSaveBefore);
        stop();
        return false;
    }
    {
        const NDSCart::CartCommon* cart = primary->getNds()->NDSCartSlot.GetCart();
        bool unchanged = ownSaveBefore.empty() || (cart && cart->GetSaveMemory() && cart->GetSaveMemoryLength() == ownSaveBefore.size()
                                                  && memcmp(cart->GetSaveMemory(), ownSaveBefore.data(), ownSaveBefore.size()) == 0);
        if (!unchanged)
        {
            LOGE("netplay: this device's save changed while loading the session; restoring it and not starting");
            restoreOwnSave(ownSaveBefore);
            stop();
            return false;
        }
    }
    // Cheats are per device (only this device's console has them): never active during a session
    savedCheats = primary->getNds()->AREngine.Cheats;
    primary->getNds()->AREngine.Cheats.clear();
    mirror->getNds()->AREngine.Cheats.clear();

    installLink();

    net = std::move(link);
    netIsHost = isHost;
    netLocalIndex = sidPrimary;
    netDelay = inputDelay;
    netAutoStop = true;
    {
        std::lock_guard<std::mutex> lock(netRequestMutex);
        netMessage.clear();
    }
    setFocus(0);
    netBeginRunning();
    return true;
}
}

namespace MelonDSAndroid
{
// Puts the given save memory back into this device's own console (and, through the cartridge, its save file)
void MirrorSession::restoreOwnSave(const std::vector<uint8_t>& saveMemory)
{
    if (saveMemory.empty())
        return;
    NDS* nds = primary->getNds();
    ScopedCurrentNds guard(nds);
    NDSCart::CartCommon* cart = nds->NDSCartSlot.GetCart();
    if (cart)
        cart->SetSaveMemory(saveMemory.data(), (u32) saveMemory.size());
}
}

namespace MelonDSAndroid
{
void MirrorSession::servicePaused()
{
    handleNetRequest();
    if (!net)
        return;
    NetplayLink::Event ev = net->service(0, [this](const uint8_t* d, size_t s) { netOnPacket(d, s); });
    if (ev == NetplayLink::Event::Disconnected)
        netEnd("peer disconnected");
}
}
