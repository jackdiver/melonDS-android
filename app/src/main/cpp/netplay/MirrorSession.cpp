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

namespace
{
enum NetMsg : uint8_t { MsgInput = 4, MsgHash = 5, MsgBye = 6 };
constexpr int kInputRedundancy = 16;
constexpr uint32_t kHashInterval = 60;
constexpr uint64_t kStallGiveUpMs = 600000; // another device may be paused; a lost connection is detected by the link itself
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

bool MirrorSession::begin(std::shared_ptr<MelonInstance> primaryInstance, std::vector<std::shared_ptr<MelonInstance>> mirrors, int self)
{
    if (active)
        stop();

    int count = (int) mirrors.size() + 1;
    if (count < 2 || count > kMaxPlayers || self < 0 || self >= count)
        return false;

    // Threaded 3D rendering is deterministic (replays match across phones); the mismatches once blamed on it
    // came from the divider/sqrt savestate gap. It roughly halves frame time during races.
    setThreadedRendering(primaryInstance->getNds(), true);

    for (auto& mirror : mirrors)
    {
        if (!mirror->cloneFrom(*primaryInstance))
        {
            LOGE("failed to clone the primary console into a mirror");
            return false;
        }
    }

    primaryInstance->setRewindBlocked(true); // rewinding would change one console behind the other devices' backs
    selfSid = self;
    consoles.clear();
    size_t next = 0;
    for (int sid = 0; sid < count; sid++)
        consoles.push_back(sid == self ? primaryInstance : mirrors[next++]);
    primary = primaryInstance.get();
    for (int sid = 0; sid < count; sid++)
        if (sid != self)
            consoles[sid]->useMirrorRenderer(); // draws nothing unless the game captures the screen into VRAM

    // GBA slot accessories (Motion Pak, rumble...) read live phone sensors: never allowed in netplay
    for (auto& console : consoles)
    {
        NDS* nds = console->getNds();
        if (nds->GetGBACart())
            nds->EjectGBACart();
    }

    installLink();
    forceSilentMic = true;
    for (int sid = 0; sid < count; sid++)
        consoles[sid]->setInputHook([this, sid](FrameInput& input) { inputHook(sid, input); });

    primary->setPresentationSource([this]() -> NDS* {
        int f = focus.load();
        if (f != 0)
        {
            // Show another console: its frame must be complete before its framebuffer is uploaded
            waitForMirrors();
            MelonInstance* shown = getInstanceUnchecked(f);
            return (shown ? shown : primary)->getNds();
        }
        return primary->getNds();
    });

    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = false;
        framesRequested = 0;
        framesDone = 0;
        workerCount = count - 1;
    }
    for (int sid = 0; sid < count; sid++)
    {
        if (sid == self)
            continue;
        workers.emplace_back(&MirrorSession::workerLoop, this, consoles[sid].get());
        pthread_setname_np(workers.back().native_handle(), "MirrorEmuThread");
    }

    focus = 0;
    active = true;
    LOGI("mirror session started (%d consoles, own session id %d)", count, self);
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
    for (auto& worker : workers)
        if (worker.joinable())
            worker.join();
    workers.clear();

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
    consoles.clear(); // the primary instance stays alive: the caller holds it
    primary = nullptr;
    LOGI("mirror session stopped");
    selfSid = 0;
    netAutoStop = false;
}

void MirrorSession::workerLoop(MelonInstance* console)
{
    int id = console->getInstanceId();
    uint64_t done = 0;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return quit || framesRequested > done; });
            if (quit)
                return;
        }

        mp->setAtBarrier(id, false);
        console->runFrameHeadless();
        mp->setAtBarrier(id, true);
        done++;

        {
            std::lock_guard<std::mutex> lock(mutex);
            framesDone++;
        }
        cv.notify_all();
    }
}

void MirrorSession::waitForMirrors()
{
    if (waitedThisFrame)
        return;

    // Park the primary at the frame barrier so the mirrors can finish (and so deadlocks resolve)
    mp->setAtBarrier(primary->getInstanceId(), true);
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return quit || framesDone >= framesRequested * workerCount; });
    waitedThisFrame = true;
}

void MirrorSession::installLink()
{
    // A fresh link: its state (inboxes, connection times) is not part of savestates, so every session starts
    // from an empty one
    auto link = std::make_unique<DeterministicMP>();
    for (int sid = 0; sid < (int) consoles.size(); sid++)
    {
        MelonInstance* console = consoles[sid].get();
        link->mapInstance(console->getInstanceId(), sid);
        link->registerInstance(console->getInstanceId(), console->getNds());
    }
    for (auto& console : consoles)
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
    if (!peers.empty())
        netBeforeFrame();
    if (!active)
        return 263;

    // All consoles are marked running before any starts: otherwise one can briefly look parked at the
    // previous frame end and the link's deadlock rule would release another early, depending on thread timing
    for (auto& console : consoles)
        mp->setAtBarrier(console->getInstanceId(), false);
    {
        std::lock_guard<std::mutex> lock(mutex);
        framesRequested++;
        waitedThisFrame = false;
    }
    cv.notify_all();

    uint64_t computeStart = nowMs();
    timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    u32 lines = primary->runFrame();
    timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1);
    waitForMirrors();
    {
        timespec ts2; clock_gettime(CLOCK_MONOTONIC, &ts2);
        double primaryMs = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
        double totalMs = (ts2.tv_sec - ts0.tv_sec) * 1000.0 + (ts2.tv_nsec - ts0.tv_nsec) / 1e6;
        netPrimaryMsSum += primaryMs; netComputeMsSum += totalMs; netComputeFrames++;
        if (totalMs > netComputeMsMax) netComputeMsMax = totalMs;
    }
    (void) computeStart;

    if (netPhase.load() == NetPhase::Running)
        netAfterFrame();
    return lines;
}

void MirrorSession::setFocus(int index)
{
    int count = (int) consoles.size();
    int wanted = (index >= 0 && index < count) ? index : 0;
    int previous = focus.exchange(wanted);
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
    if (index <= 0 || consoles.empty())
        return primary;
    // k-th other console in session id order
    int seen = 0;
    for (int sid = 0; sid < (int) consoles.size(); sid++)
    {
        if (sid == selfSid)
            continue;
        if (++seen == index)
            return consoles[sid].get();
    }
    return primary;
}

MelonInstance* MirrorSession::getInstance(int index)
{
    if (!active)
        return nullptr;
    return getInstanceUnchecked(index);
}


// ---------------------------------------------------------------------------------------------------------------
// Netplay: every device runs every console; only inputs travel over the network, over a full mesh of links (one per
// other device). Each device's local input is delayed by `netDelay` frames so the others normally have it before it is
// needed; if not, the frame waits (stall).
// ---------------------------------------------------------------------------------------------------------------

void MirrorSession::requestNetStop()
{
    netStopRequested = true;
}

bool MirrorSession::netLinksUp() const
{
    return !peers.empty();
}

void MirrorSession::handleNetRequest()
{
    if (netStopRequested.exchange(false))
        netEnd("stopped");
}

void MirrorSession::netEnd(const char* reason)
{
    if (netPhase.load() == NetPhase::Off && peers.empty())
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
    // Tell everyone, so they end at once instead of waiting for a timeout
    std::vector<uint8_t> bye { MsgBye };
    for (Peer& peer : peers)
    {
        if (peer.link && peer.link->isConnected())
        {
            peer.link->send(bye.data(), bye.size(), true);
            peer.link->flush();
        }
    }
    for (Peer& peer : peers)
        if (peer.link)
            peer.link->close();
    peers.clear();
    netPhase = NetPhase::Off;
}

bool MirrorSession::loadStateFresh(MelonInstance* console, const uint8_t* data, size_t size)
{
    // Same path as snapshotAndReload: reset to launch defaults, then load (cold JIT, no leftover state)
    NDS* nds = console->getNds();
    ScopedCurrentNds currentGuard(nds);
    nds->Reset();
    std::string romName;
    nds->SetupDirectBoot(romName);
    nds->Start();
    std::vector<u8> copy(data, data + size);
    Savestate load(copy.data(), (u32) copy.size(), false);
    bool ok = !load.Error && nds->DoSavestate(&load) && !load.Error;
    console->clearInput();
    return ok;
}

void MirrorSession::netOnPacket(Peer& peer, const uint8_t* data, size_t size)
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
            InputSlot& slot = remoteSlot(peer.sid, first + i);
            slot.frame = first + i;
            slot.input = in;
        }
        break;
    }
    case MsgHash:
    {
        HashEntry entry; uint8_t count;
        if (!get(p, end, entry.frame) || !get(p, end, count)) return;
        for (uint8_t i = 0; i < count; i++)
        {
            uint64_t h;
            if (!get(p, end, h)) return;
            entry.hashes.push_back(h);
        }
        peer.hashes.push_back(std::move(entry));
        break;
    }
    case MsgBye:
        peer.byeReceived = true;
        break;
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
    for (Peer& peer : peers)
    {
        peer.link->send(msg.data(), msg.size(), false);
        peer.link->flush();
    }
}

// Services every link without waiting longer than `timeoutMs` in total on the first one. Returns false when a peer is
// gone (the session has been ended).
bool MirrorSession::servicePeers(int timeoutMs)
{
    for (size_t i = 0; i < peers.size(); i++)
    {
        Peer& peer = peers[i];
        NetplayLink::Event ev = peer.link->service(i == 0 ? timeoutMs : 0, [&](const uint8_t* d, size_t s) { netOnPacket(peer, d, s); });
        if (peer.byeReceived)
        {
            netEnd("another player stopped");
            return false;
        }
        if (ev == NetplayLink::Event::Disconnected || !peer.link->isConnected())
        {
            netEnd("another player disconnected");
            return false;
        }
    }
    return true;
}

// Emulator thread, before the frame: waits until every other device's input for this frame has arrived.
bool MirrorSession::netBeforeFrame()
{
    uint64_t start = 0, lastSend = 0;
    for (;;)
    {
        if (!servicePeers(0))
            return true;
        bool missing = false;
        for (Peer& peer : peers)
            if (remoteSlot(peer.sid, netFrame).frame != netFrame)
                missing = true;
        if (!missing)
            break;
        if (netStopRequested.exchange(false))
        {
            netEnd("stopped");
            return true;
        }

        uint64_t now = nowMs();
        if (start == 0)
        {
            start = lastSend = now;
            for (Peer& peer : peers)
                if (remoteSlot(peer.sid, netFrame).frame != netFrame)
                    netLateBySid[peer.sid]++;
        }
        if (now - start > 1000)
            netStalled = true;
        if (now - lastSend >= 16)
        {
            netSendInputs(); // resend in case packets were lost
            lastSend = now;
        }
        if (now - start > kStallGiveUpMs)
        {
            netEnd("another device stopped sending input");
            return true;
        }
        // Short sleep until something arrives. A disconnect reported here is picked up by the next servicePeers().
        for (Peer& peer : peers)
        {
            if (remoteSlot(peer.sid, netFrame).frame != netFrame)
            {
                peer.link->service(2, [&](const uint8_t* d, size_t s) { netOnPacket(peer, d, s); });
                break;
            }
        }
    }
    netFramesSinceLog++;
    netStalled = false;
    if (start != 0)
    {
        uint64_t stalled = nowMs() - start;
        netStallMs += stalled;
        netLateFramesSinceLog++;
        if (stalled > netMaxWaitSinceLog) netMaxWaitSinceLog = stalled;
        if (stalled > 100)
            LOGI("netplay: waited %llu ms for input of frame %u", (unsigned long long) stalled, netFrame);
    }
    return true;
}

// Runs on the thread of console `sid` at the start of its frame
void MirrorSession::netInputHook(int sid, FrameInput& input)
{
    if (sid == selfSid)
    {
        // The live input is applied `netDelay` frames from now (on every device)
        uint32_t target = netFrame + netDelay;
        netLocal[target % kInputRing] = InputSlot { target, input };
        netLocalLast = target;
        netLocalCount++;
        const InputSlot& slot = netLocal[netFrame % kInputRing];
        input = slot.frame == netFrame ? slot.input : kNeutralInput;
    }
    else
    {
        const InputSlot& slot = remoteSlot(sid, netFrame);
        input = slot.frame == netFrame ? slot.input : kNeutralInput;
    }
}

void MirrorSession::netCompareHashes()
{
    for (Peer& peer : peers)
    {
        while (!peer.hashes.empty())
        {
            HashEntry& r = peer.hashes.front();
            auto local = std::find_if(netLocalHashes.begin(), netLocalHashes.end(), [&](const HashEntry& e) { return e.frame == r.frame; });
            if (local == netLocalHashes.end())
            {
                if (netLocalHashes.empty() || r.frame > netLocalHashes.back().frame)
                    break; // our own hash for that frame is not ready yet
                peer.hashes.pop_front(); // too old, nothing to compare with
                continue;
            }
            netHashChecks++;
            if (local->hashes != r.hashes)
            {
                netDesyncs++;
                netDesyncsShown = netDesyncs;
                std::string differing;
                for (size_t c = 0; c < local->hashes.size() && c < r.hashes.size(); c++)
                    if (local->hashes[c] != r.hashes[c])
                        differing += " " + std::to_string(c);
                LOGE("netplay: DESYNC at frame %u against player %d, console(s) differing:%s", r.frame, peer.sid, differing.c_str());
            }
            else if (netHashChecks % (10 * peers.size()) == 0)
            {
                LOGI("netplay: frame %u in sync (%u checks, %u desyncs, stall %llu ms); input late in %u of %u frames, worst wait %llu ms; rtt %u ms +-%u",
                     r.frame, netHashChecks, netDesyncs, (unsigned long long) netStallMs, netLateFramesSinceLog, netFramesSinceLog,
                     (unsigned long long) netMaxWaitSinceLog, peer.link->rttMs(), peer.link->rttVarianceMs());
                if (netComputeFrames) { LOGI("compute per frame: own console %.1f ms, with hidden consoles %.1f ms avg, %.1f ms max (%u frames)", netPrimaryMsSum / netComputeFrames, netComputeMsSum / netComputeFrames, netComputeMsMax, netComputeFrames); netPrimaryMsSum = netComputeMsSum = netComputeMsMax = 0; netComputeFrames = 0; }
                { std::string late; for (int s = 0; s < 16; s++) if (netLateBySid[s]) late += " p" + std::to_string(s) + ":" + std::to_string(netLateBySid[s]); LOGI("late by peer:%s", late.c_str()); for (auto& c : netLateBySid) c = 0; }
                netFramesSinceLog = netLateFramesSinceLog = 0;
                netMaxWaitSinceLog = 0;
            }
            peer.hashes.pop_front();
        }
    }
    while (netLocalHashes.size() > 16)
        netLocalHashes.pop_front();
}

void MirrorSession::netAfterFrame()
{
    netSendInputs();

    if (netFrame % kHashInterval == 0)
    {
        HashEntry entry;
        entry.frame = netFrame;
        for (auto& console : consoles)
            entry.hashes.push_back(hashConsoleState(console->getNds()));
        std::vector<uint8_t> msg { MsgHash };
        put(msg, netFrame);
        put(msg, (uint8_t) entry.hashes.size());
        for (uint64_t h : entry.hashes)
            put(msg, h);
        netLocalHashes.push_back(std::move(entry));
        for (Peer& peer : peers)
            peer.link->send(msg.data(), msg.size(), true);
    }

    netCompareHashes();

    netFrame++;
    netFrameShown = netFrame;
}

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

void MirrorSession::netBeginRunning()
{
    // Session starts at frame 0 on every device
    netFrame = 0;
    netLocal.assign(kInputRing, InputSlot());
    netRemote.assign((size_t) consoles.size() * kInputRing, InputSlot());
    // The first `netDelay` frames use neutral input everywhere
    for (uint32_t f = 0; f < (uint32_t) netDelay; f++)
    {
        netLocal[f % kInputRing] = InputSlot { f, kNeutralInput };
        for (int sid = 0; sid < (int) consoles.size(); sid++)
            remoteSlot(sid, f) = InputSlot { f, kNeutralInput };
    }
    netLocalLast = netDelay - 1;
    netLocalCount = netDelay;
    netLocalHashes.clear();
    for (Peer& peer : peers)
        peer.hashes.clear();
    netDesyncs = netHashChecks = 0;
    netStallMs = 0;
    netFrameShown = 0;
    netDesyncsShown = 0;
    netPhase = NetPhase::Running;
    LOGI("netplay: running (%d players, console %d is local)", (int) consoles.size(), selfSid);
}

bool MirrorSession::startNetSession(std::shared_ptr<MelonInstance> primaryInstance, std::vector<std::shared_ptr<MelonInstance>> mirrors,
                                    int self, std::vector<std::unique_ptr<NetplayLink>> links, int inputDelay,
                                    const std::vector<uint8_t>& ownState,
                                    const std::vector<std::vector<uint8_t>>& remoteFirmware,
                                    const std::vector<std::vector<uint8_t>>& remoteStates)
{
    // Each device keeps its own console as the primary instance; the other players' consoles are the mirrors.
    // Session ids are the same on every device: host console = 0, joiners in join order.
    int count = (int) mirrors.size() + 1;
    if ((int) links.size() != count || (int) remoteFirmware.size() != count || (int) remoteStates.size() != count)
        return false;
    if (!begin(std::move(primaryInstance), std::move(mirrors), self))
        return false;

    for (int sid = 0; sid < count; sid++)
    {
        if (sid == selfSid)
            continue;
        // Firmware (MAC, nickname, settings) is not part of savestates: every other console needs its owner's
        ScopedCurrentNds guard(consoles[sid]->getNds());
        consoles[sid]->getNds()->SetFirmware(Firmware(remoteFirmware[sid].data(), (u32) remoteFirmware[sid].size()));
    }
    // This device's own console keeps this device's own save: loading a state writes the state's save data back
    // to the save file, so remember the save memory first and verify it is unchanged afterwards.
    std::vector<uint8_t> ownSaveBefore;
    {
        const NDSCart::CartCommon* cart = primary->getNds()->NDSCartSlot.GetCart();
        if (cart && cart->GetSaveMemory())
            ownSaveBefore.assign(cart->GetSaveMemory(), cart->GetSaveMemory() + cart->GetSaveMemoryLength());
    }
    bool loaded = loadStateFresh(primary, ownState.data(), ownState.size());
    for (int sid = 0; sid < count && loaded; sid++)
        if (sid != selfSid)
            loaded = loadStateFresh(consoles[sid].get(), remoteStates[sid].data(), remoteStates[sid].size());
    if (!loaded)
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
    for (auto& console : consoles)
        console->getNds()->AREngine.Cheats.clear();

    installLink();

    peers.clear();
    for (int sid = 0; sid < count; sid++)
    {
        if (sid == selfSid)
            continue;
        Peer peer;
        peer.sid = sid;
        peer.link = std::move(links[sid]);
        peers.push_back(std::move(peer));
    }
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

void MirrorSession::servicePaused()
{
    handleNetRequest();
    if (peers.empty())
        return;
    servicePeers(0);
}

}
