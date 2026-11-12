#include "MirrorSession.h"

#include <android/log.h>
#include <pthread.h>
#include "DeterministicMP.h"
#include "NetplayLink.h"
#include "NDSCart.h"
#include "NetplayCommon.h"
#include <time.h>
#include <zlib.h>
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

    if (primaryInstance->getCurrentRenderer() != Renderer::Software)
    {
        LOGE("mirror session needs the software renderer (3D output must be identical on every device)");
        return false;
    }

    // Threaded 3D rendering is deterministic (replays match across phones); the mismatches once blamed on it
    // came from the divider/sqrt savestate gap. It roughly halves frame time during races.
    setThreadedRendering(primaryInstance->getNds(), true);

    if (!mirrorInstance->cloneFrom(*primaryInstance))
    {
        LOGE("failed to clone the primary console into the mirror");
        return false;
    }

    primary = std::move(primaryInstance);
    mirror = std::move(mirrorInstance);
    setThreadedRendering(mirror->getNds(), true);

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
    if (net && !netBeforeFrame())
        return 263; // connecting or syncing: hold the consoles still

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
enum NetMsg : uint8_t { MsgHello = 1, MsgState = 2, MsgReady = 3, MsgInput = 4, MsgHash = 5 };
constexpr int kInputRedundancy = 16;
constexpr uint32_t kHashInterval = 60;
constexpr uint64_t kStallGiveUpMs = 20000;
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

void MirrorSession::requestNetHost(int port, int inputDelay)
{
    std::lock_guard<std::mutex> lock(netRequestMutex);
    netRequestPort = port > 0 ? port : 7064;
    netRequestDelay = inputDelay > 0 ? inputDelay : 2;
    netRequest = 1;
}

void MirrorSession::requestNetJoin(const std::string& address, int port, int inputDelay)
{
    std::lock_guard<std::mutex> lock(netRequestMutex);
    netRequestAddress = address;
    netRequestPort = port > 0 ? port : 7064;
    netRequestDelay = inputDelay > 0 ? inputDelay : 2;
    netRequest = 2;
}

void MirrorSession::requestNetStop()
{
    netRequest = 3;
}

uint32_t MirrorSession::romHash()
{
    const NDSCart::CartCommon* cart = primary->getNds()->NDSCartSlot.GetCart();
    if (!cart) return 0;
    uint32_t h = 2166136261u;
    const u8* rom = cart->GetROM();
    for (u32 i = 0; i < cart->GetROMLength(); i++)
        h = (h ^ rom[i]) * 16777619u;
    return h;
}

void MirrorSession::handleNetRequest()
{
    int request = netRequest.exchange(0);
    if (request == 0)
        return;

    netEnd("new request");
    if (request == 3)
        return;

    std::string address;
    int port, delay;
    {
        std::lock_guard<std::mutex> lock(netRequestMutex);
        address = netRequestAddress;
        port = netRequestPort;
        delay = netRequestDelay;
    }

    net = std::make_unique<NetplayLink>();
    netIsHost = request == 1;
    netLocalIndex = netIsHost ? 0 : 1;
    netDelay = delay;
    netHelloOk = false;
    netReady = false;
    netStateRx.clear();

    bool ok = netIsHost ? net->host((uint16_t) port) : net->join(address, (uint16_t) port);
    if (!ok)
    {
        net.reset();
        LOGE("netplay: could not %s", netIsHost ? "host" : "join");
        std::lock_guard<std::mutex> lock(netRequestMutex);
        netMessage = netIsHost ? "could not open the port" : "bad address";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(netRequestMutex);
        netMessage.clear();
    }
    netFrameShown = 0;
    netDesyncsShown = 0;
    netPhase = netIsHost ? NetPhase::Listening : NetPhase::Connecting;
    LOGI("netplay: %s %s (port %d, input delay %d)", netIsHost ? "hosting" : "joining", address.c_str(), port, delay);
}

void MirrorSession::netEnd(const char* reason)
{
    if (netPhase.load() == NetPhase::Off && !net)
        return;
    if (strcmp(reason, "new request") != 0)
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

bool MirrorSession::saveStateTo(int index, std::vector<uint8_t>& out)
{
    NDS* nds = getInstanceUnchecked(index)->getNds();
    ScopedCurrentNds currentGuard(nds);
    Savestate state;
    if (state.Error || !nds->DoSavestate(&state) || state.Error)
        return false;
    auto* bytes = static_cast<const u8*>(state.Buffer());
    out.assign(bytes, bytes + state.Length());
    return true;
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

bool MirrorSession::netSyncAsHost()
{
    std::vector<uint8_t> states[2];
    for (int i = 0; i < 2; i++)
        if (!saveStateTo(i, states[i]))
            return false;
    // The host reloads too, so both devices start from the same freshly loaded consoles
    for (int i = 0; i < 2; i++)
        if (!loadStateFresh(i, states[i].data(), states[i].size()))
            return false;
    installLink();

    // Savestates are mostly zeros: compress (39 MB raw -> a few MB)
    std::vector<uint8_t> raw;
    put(raw, (uint32_t) states[0].size());
    put(raw, (uint32_t) states[1].size());
    raw.insert(raw.end(), states[0].begin(), states[0].end());
    raw.insert(raw.end(), states[1].begin(), states[1].end());
    uLongf packed = compressBound(raw.size());
    std::vector<uint8_t> msg(1 + 4 + packed);
    msg[0] = MsgState;
    uint32_t rawSize = (uint32_t) raw.size();
    memcpy(&msg[1], &rawSize, 4);
    if (compress2(&msg[5], &packed, raw.data(), raw.size(), 1) != Z_OK)
        return false;
    msg.resize(5 + packed);
    net->send(msg.data(), msg.size(), true);
    net->flush();
    LOGI("netplay: sent state (%zu + %zu bytes, %zu compressed)", states[0].size(), states[1].size(), msg.size());
    return true;
}

void MirrorSession::netOnPacket(const uint8_t* data, size_t size)
{
    if (size < 1) return;
    const uint8_t* p = data + 1;
    const uint8_t* end = data + size;
    switch (data[0])
    {
    case MsgHello:
    {
        uint32_t hash = 0;
        get(p, end, hash);
        netHelloOk = hash == romHash();
        if (!netHelloOk)
            LOGE("netplay: the other device has a different ROM (%08x vs %08x)", hash, romHash());
        if (!netHelloOk)
        {
            std::lock_guard<std::mutex> lock(netRequestMutex);
            netMessage = "the other device is running a different game";
        }
        break;
    }
    case MsgState:
        netStateRx.assign(data, data + size);
        break;
    case MsgReady:
        netReady = true;
        break;
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

// Emulator thread, before the frame. Returns false when this frame must not run (still connecting/syncing).
bool MirrorSession::netBeforeFrame()
{
    auto onPacket = [this](const uint8_t* d, size_t s) { netOnPacket(d, s); };
    NetPhase phase = netPhase.load();

    if (phase != NetPhase::Running)
    {
        NetplayLink::Event ev = net->service(0, onPacket);
        if (ev == NetplayLink::Event::Disconnected)
        {
            netEnd("peer disconnected");
            return true;
        }
        if (ev == NetplayLink::Event::Connected && !netIsHost)
        {
            std::vector<uint8_t> msg { MsgHello };
            put(msg, romHash());
            net->send(msg.data(), msg.size(), true);
            net->flush();
            netPhase = phase = NetPhase::AwaitState;
        }

        switch (phase)
        {
        case NetPhase::Listening:
            if (!netHelloOk)
                return true; // keep playing locally until someone joins
            if (!netSyncAsHost())
            {
                netEnd("state sync failed");
                return true;
            }
            netPhase = NetPhase::AwaitReady;
            return false;
        case NetPhase::Connecting:
            return false;
        case NetPhase::AwaitState:
        {
            if (netStateRx.empty())
                return false;
            uint32_t rawSize = 0;
            if (netStateRx.size() > 5) memcpy(&rawSize, &netStateRx[1], 4);
            std::vector<uint8_t> raw(rawSize);
            uLongf rawLen = rawSize;
            if (rawSize == 0 || uncompress(raw.data(), &rawLen, &netStateRx[5], netStateRx.size() - 5) != Z_OK)
            {
                netEnd("bad state from host");
                return false;
            }
            const uint8_t* p = raw.data();
            const uint8_t* end = raw.data() + rawLen;
            uint32_t len0, len1;
            if (!get(p, end, len0) || !get(p, end, len1) || (size_t) (end - p) < (size_t) len0 + len1
                || !loadStateFresh(0, p, len0) || !loadStateFresh(1, p + len0, len1))
            {
                netEnd("bad state from host");
                return false;
            }
            netStateRx.clear();
            installLink();
            setFocus(1);
            std::vector<uint8_t> msg { MsgReady };
            net->send(msg.data(), msg.size(), true);
            net->flush();
            break; // start running below
        }
        case NetPhase::AwaitReady:
            if (!netReady)
                return false;
            break;
        default:
            return true;
        }

        netBeginRunning();
    }

    // Running: wait until the other device's input for this frame is here
    uint64_t start = 0, lastSend = 0;
    for (;;)
    {
        NetplayLink::Event ev = net->service(0, onPacket);
        if (ev == NetplayLink::Event::Disconnected)
        {
            netEnd("peer disconnected");
            return true;
        }
        if (netRemote[netFrame % kInputRing].frame == netFrame)
            break;

        uint64_t now = nowMs();
        if (start == 0) start = lastSend = now;
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
        net->service(2, onPacket);
    }
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
    case NetPhase::Listening: phase = "waiting"; break;
    case NetPhase::Connecting: phase = "connecting"; break;
    case NetPhase::AwaitState:
    case NetPhase::AwaitReady: phase = "syncing"; break;
    case NetPhase::Running: phase = "running"; break;
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
    if (!loadStateFresh(0, ownState.data(), ownState.size()) || !loadStateFresh(1, remoteState.data(), remoteState.size()))
    {
        LOGE("netplay: could not load the session states");
        stop();
        return false;
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
