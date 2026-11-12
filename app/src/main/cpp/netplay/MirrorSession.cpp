#include "MirrorSession.h"

#include <android/log.h>
#include <pthread.h>
#include "DeterministicMP.h"
#include "NetplayLink.h"
#include "NDSCart.h"
#include <time.h>
#include <chrono>
#include <zlib.h>
#include <sys/stat.h>
#include "MelonInstance.h"
#include "net/MPInterface.h"
#include "Savestate.h"
#include "GPU3D_Soft.h"
#include <cstdio>
#include <cstring>
#include "ARM.h"
#include <vector>
#include <algorithm>

#define TAG "NetplayMirror"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace melonDS;

namespace MelonDSAndroid
{

// DEBUG: frame time breakdown, logged every 300 frames
static double accNet = 0, accPrimary = 0, accMirror = 0;
// DEBUG experiment (flag 64): a call between JIT blocks that does nothing
static void noopHook(melonDS::ARM*, melonDS::u32) {}
static double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
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

void MirrorSession::setFastMemory(NDS* nds, bool enabled)
{
#ifdef JIT_ENABLED
    if (!nds->IsJITEnabled())
        return;
    JITArgs args {
        .MaxBlockSize = 32,
        .LiteralOptimizations = true,
        .BranchOptimizations = true,
        .FastMemory = enabled,
    };
    nds->SetJITArgs(args);
#endif
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
    setThreadedRendering(primaryInstance->getNds(), (debugFlags & 256) == 0); // threaded 3D is deterministic; flag 256 = render on the emulation thread

    if (!mirrorInstance->cloneFrom(*primaryInstance))
    {
        LOGE("failed to clone the primary console into the mirror");
        return false;
    }

    primary = std::move(primaryInstance);
    mirror = std::move(mirrorInstance);
    setThreadedRendering(mirror->getNds(), (debugFlags & 256) == 0);

#ifdef JIT_ENABLED
    // DEBUG experiment: JIT option variations (flags 4/8/16)
    int flags = debugFlags.load();
    if (flags & (4 | 8 | 16))
    {
        JITArgs args {
            .MaxBlockSize = (flags & 16) ? 1u : 32u,
            .LiteralOptimizations = (flags & 8) == 0,
            .BranchOptimizations = (flags & 4) == 0,
            .FastMemory = true,
        };
        for (int i = 0; i < 2; i++)
        {
            NDS* nds = getInstanceUnchecked(i)->getNds();
            ScopedCurrentNds guard(nds);
            if (nds->IsJITEnabled())
                nds->SetJITArgs(args);
        }
        LOGI("JIT experiment: maxBlock %u literal %d branch %d", args.MaxBlockSize, args.LiteralOptimizations, args.BranchOptimizations);
    }
#endif

    // GBA slot accessories (Motion Pak, rumble...) read live phone sensors: never allowed in netplay
    for (int i = 0; i < 2; i++)
    {
        NDS* nds = getInstanceUnchecked(i)->getNds();
        auto* gbaCart = nds->GetGBACart();
        LOGI("console %d GBA slot: %s (type %d), JIT %d", i, gbaCart ? "cart present" : "empty",
             gbaCart ? (int) gbaCart->Type() : -1, nds->IsJITEnabled() ? 1 : 0);
        if (gbaCart)
            nds->EjectGBACart();
    }

    installLink();
    forceSilentMic = true;
    melonDS::JitTraceHook9 = (debugFlags & 64) ? noopHook : nullptr;
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

    stopRecording("session stopped");
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
    // A fresh link: its state (inboxes, connection times) is not part of savestates, so recording and replay
    // must both start from an empty link
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

void MirrorSession::requestRecord(const std::string& directory)
{
    std::lock_guard<std::mutex> lock(recRequestMutex);
    recRequestDir = directory;
    recRequestTag = "record";
    recRequest = RecRequest::Record;
}

void MirrorSession::requestReplay(const std::string& directory, const std::string& tag)
{
    std::lock_guard<std::mutex> lock(recRequestMutex);
    recRequestDir = directory;
    recRequestTag = tag.empty() ? "replay" : tag;
    recRequest = RecRequest::Replay;
}

void MirrorSession::requestStopRecording()
{
    recRequest = RecRequest::Stop;
}

bool MirrorSession::snapshotAndReload(int index, const std::string& path, bool save)
{
    NDS* nds = getInstance(index)->getNds();
    ScopedCurrentNds currentGuard(nds);
    std::vector<u8> buffer;

    if (save)
    {
        Savestate state;
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
            return false;
        auto* bytes = static_cast<const u8*>(state.Buffer());
        buffer.assign(bytes, bytes + state.Length());
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(buffer.data(), 1, buffer.size(), f) == buffer.size();
        fclose(f);
        if (!ok) return false;
    }
    else
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buffer.resize(size);
        bool ok = fread(buffer.data(), 1, size, f) == (size_t) size;
        fclose(f);
        if (!ok) return false;
    }

    // Reset first: some emulated state (seen in the 3D engine) is not covered by savestates, so every console
    // must start from the same reset defaults before loading. Same path as a normal launch (see cloneFrom).
    nds->Reset();
    std::string romName;
    nds->SetupDirectBoot(romName);
    nds->Start();

    // Always (re)load: every recording and replay starts with a cold JIT cache
    Savestate load(buffer.data(), (u32) buffer.size(), false);
    return !load.Error && nds->DoSavestate(&load) && !load.Error;
}

void MirrorSession::stopRecording(const char* reason)
{
    for (FILE*& f : inputFiles)
        if (f) { fclose(f); f = nullptr; }
    if (hashFile) { fclose(hashFile); hashFile = nullptr; }
    unthrottleForReplay = false;
    if (recMode.load() == RecMode::Replay)
    {
        // Marker so a test script can tell the replay has finished
        if (FILE* f = fopen((recDir + "/done-" + recTag + ".txt").c_str(), "w")) { fprintf(f, "%u\n", recFrame); fclose(f); }
    }
    if (recMode.exchange(RecMode::Off) != RecMode::Off)
        LOGI("recording/replay stopped after %u frames: %s", recFrame, reason);
}

void MirrorSession::handleRecordingRequest()
{
    RecRequest request = recRequest.exchange(RecRequest::None);
    if (request == RecRequest::None)
        return;

    stopRecording("new request");
    if (request == RecRequest::Stop)
        return;

    std::string dir, tag;
    {
        std::lock_guard<std::mutex> lock(recRequestMutex);
        dir = recRequestDir;
        tag = recRequestTag;
    }
    bool record = request == RecRequest::Record;

    // Both consoles are parked between frames here (the worker is idle)
    for (int i = 0; i < 2; i++)
    {
        std::string path = dir + "/state" + std::to_string(i) + ".bin";
        if (!snapshotAndReload(i, path, record))
        {
            LOGE("%s: snapshot of console %d failed (%s)", record ? "record" : "replay", i, path.c_str());
            return;
        }
        getInstance(i)->clearInput();
    }
    installLink();

    for (int i = 0; i < 2; i++)
    {
        std::string path = dir + "/inputs" + std::to_string(i) + ".bin";
        inputFiles[i] = fopen(path.c_str(), record ? "wb" : "rb");
    }
    hashFile = fopen((dir + "/hashes-" + tag + ".txt").c_str(), "w");
    if (!inputFiles[0] || !inputFiles[1] || !hashFile)
    {
        stopRecording("cannot open files");
        return;
    }

    recFrame = 0;
    recDir = dir;
    recTag = tag;
    replayEnded = false;
    recMode = record ? RecMode::Record : RecMode::Replay;
    unthrottleForReplay = !record;
    remove((dir + "/done-" + tag + ".txt").c_str());
    LOGI("%s started (%s)", record ? "recording" : "replay", tag.c_str());
}

void MirrorSession::inputHook(int index, FrameInput& input)
{
    if (netPhase.load() == NetPhase::Running)
    {
        netInputHook(index, input);
        return;
    }
    // Runs on the thread of console `index`; each console has its own file
    switch (recMode.load())
    {
    case RecMode::Record:
        fwrite(&input, sizeof(FrameInput), 1, inputFiles[index]);
        break;
    case RecMode::Replay:
        if (fread(&input, sizeof(FrameInput), 1, inputFiles[index]) != 1)
        {
            replayEnded = true;
            input = FrameInput { 0xFFF, 0, 0, 0, 0, {0, 0} };
        }
        break;
    case RecMode::Off:
        break;
    }
}

// DEBUG (determinism investigation): main RAM, shared WRAM, ARM7 WRAM and both register files
static void appendRam(NDS* nds, std::vector<u8>& out)
{
    auto add = [&](const void* p, size_t n) { const u8* b = (const u8*) p; out.insert(out.end(), b, b + n); };
    add(nds->MainRAM, nds->MainRAMMask + 1);
    add(nds->SharedWRAM, nds->SharedWRAMSize);
    add(nds->ARM7WRAM, nds->ARM7WRAMSize);
    add(nds->ARM9.R, sizeof(nds->ARM9.R));
    add(nds->ARM7.R, sizeof(nds->ARM7.R));
    add(&nds->ARM9.CPSR, 4);
    add(&nds->ARM7.CPSR, 4);
    add(nds->ARM9.DTCM, melonDS::DTCMPhysicalSize);
    add(nds->ARM9.R_FIQ, 80);
    add(nds->ARM7.R_FIQ, 80);
}

static void writeFile(const std::string& path, const std::vector<u8>& data)
{
    if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}

// DEBUG: per-block ARM9 trace of both consoles, kept in memory for the current frame only.
// Record: block address, R0-R15, CPSR, banked registers (R_FIQ..R_UND, 20 words), 16 bytes at 0x0216FF98.
static constexpr int kTraceWords = 1 + 16 + 1 + 20 + 4;
static std::vector<u32> gTrace[2];
static melonDS::ARM* gTraceCpu[2] = { nullptr, nullptr };
static void traceHook(melonDS::ARM* cpu, u32 addr)
{
    int i = cpu == gTraceCpu[0] ? 0 : cpu == gTraceCpu[1] ? 1 : -1;
    if (i < 0) return;
    u32 rec[kTraceWords];
    rec[0] = addr;
    memcpy(&rec[1], cpu->R, 64);
    rec[17] = cpu->CPSR;
    memcpy(&rec[18], cpu->R_FIQ, 80);
    memcpy(&rec[38], cpu->NDS.MainRAM + 0x16FF98, 16);
    gTrace[i].insert(gTrace[i].end(), rec, rec + kTraceWords);
}


static std::vector<std::string> readLines(const std::string& path)
{
    std::vector<std::string> lines;
    if (FILE* f = fopen(path.c_str(), "r"))
    {
        char buf[256];
        while (fgets(buf, sizeof(buf), f))
        {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        }
        fclose(f);
    }
    return lines;
}

u32 MirrorSession::runFrame()
{
    handleRecordingRequest();
    handleNetRequest();
    auto frameStart = std::chrono::steady_clock::now();
    bool netGo = !net || netBeforeFrame();
    accNet += msSince(frameStart);
    if (!netGo)
        return 263; // connecting or syncing: hold the consoles still
    bool netRunning = netPhase.load() == NetPhase::Running;

    // DEBUG capture: during replay, every frame is traced; the trace and pre/post RAM of a frame are written
    // when its hashes differ from hashes-ref.txt (another device's run), or when it is listed in dumpframes.txt.
    // Opt-in: only active when one of those files exists (it slows replays to ~8 fps).
    if (recMode.load() == RecMode::Replay && recFrame == 0)
    {
        refHashes = readLines(recDir + "/hashes-ref.txt");
        dumpFrames.clear();
        for (const std::string& s : readLines(recDir + "/dumpframes.txt"))
            if (!s.empty()) dumpFrames.push_back((uint32_t) strtoul(s.c_str(), nullptr, 10));
        capturesWritten = 0;
    }
    bool capture = recMode.load() == RecMode::Replay && (!refHashes.empty() || !dumpFrames.empty());
    std::vector<u8> pre[2];
    if (capture)
    {
        MelonInstance* inst[2] = { primary.get(), mirror.get() };
        for (int i = 0; i < 2; i++)
        {
            appendRam(inst[i]->getNds(), pre[i]);
            gTraceCpu[i] = &inst[i]->getNds()->ARM9;
            gTrace[i].clear();
        }
        melonDS::JitTraceHook9 = traceHook;
    }

    auto kickMirror = [this] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            framesRequested++;
            waitedThisFrame = false;
        }
        cv.notify_all();
    };

    // Consoles run in parallel (repeatable since the link and savestate fixes, see docs/netplay-plan.md).
    // DEBUG flag 2 runs them one after the other instead.
    bool serial = (debugFlags & 2) != 0;
    // Both consoles are marked running before either starts: otherwise one can briefly look parked at the
    // previous frame end and the deadlock rule would release the other early, depending on thread timing
    mp->setAtBarrier(primary->getInstanceId(), false);
    if (!serial)
    {
        mp->setAtBarrier(mirror->getInstanceId(), false);
        kickMirror();
    }

    auto tPrimary = std::chrono::steady_clock::now();
    u32 lines = primary->runFrame();
    accPrimary += msSince(tPrimary);
    if (serial)
    {
        mp->setAtBarrier(primary->getInstanceId(), true);
        kickMirror();
    }
    { auto t = std::chrono::steady_clock::now(); waitForMirror(); accMirror += msSince(t); }
    if (capture)
        melonDS::JitTraceHook9 = (debugFlags & 64) ? noopHook : nullptr;

    if (recMode.load() != RecMode::Off && hashFile)
    {
        char line[128];
        snprintf(line, sizeof(line), "%u %016llx %016llx", recFrame,
                 (unsigned long long) DeterminismHarness::hashState(primary->getNds()),
                 (unsigned long long) DeterminismHarness::hashState(mirror->getNds()));
        fprintf(hashFile, "%s\n", line);

        // DEBUG flag 128: cheap post-frame dump of frames 0-2 (no tracing, no per-frame copies)
        if ((debugFlags & 128) && recFrame < 3 && recMode.load() == RecMode::Replay)
        {
            MelonInstance* inst[2] = { primary.get(), mirror.get() };
            for (int i = 0; i < 2; i++)
            {
                std::vector<u8> post;
                appendRam(inst[i]->getNds(), post);
                writeFile(recDir + "/ramq" + std::to_string(i) + "-" + std::to_string(recFrame) + "-" + recTag + ".bin", post);
            }
        }

        if (capture)
        {
            bool mismatch = recFrame < refHashes.size() && refHashes[recFrame] != line;
            bool listed = std::find(dumpFrames.begin(), dumpFrames.end(), recFrame) != dumpFrames.end();
            if ((mismatch && capturesWritten < 3) || listed)
            {
                if (mismatch) capturesWritten++;
                MelonInstance* inst[2] = { primary.get(), mirror.get() };
                std::string suffix = std::to_string(recFrame) + "-" + recTag + ".bin";
                for (int i = 0; i < 2; i++)
                {
                    std::vector<u8> post;
                    appendRam(inst[i]->getNds(), post);
                    writeFile(recDir + "/ram" + std::to_string(i) + "-pre-" + suffix, pre[i]);
                    writeFile(recDir + "/ram" + std::to_string(i) + "-post-" + suffix, post);
                    const u8* t = (const u8*) gTrace[i].data();
                    writeFile(recDir + "/trace9-" + std::to_string(i) + "-" + suffix,
                              std::vector<u8>(t, t + gTrace[i].size() * 4));
                }
                LOGI("capture written for frame %u (%s)", recFrame, mismatch ? "hash mismatch" : "listed");
            }
        }

        recFrame++;
        if (replayEnded)
            stopRecording("end of recorded input");
    }

    if (netRunning && netPhase.load() == NetPhase::Running)
        netAfterFrame();

    {
        static double accTotal = 0; static int accN = 0;
        accTotal += msSince(frameStart); accN++;
        if (accN == 300)
        {
            LOGI("timing (ms/frame avg of 300): total %.2f | net wait %.2f | primary %.2f | mirror %.2f | rest %.2f",
                 accTotal / 300, accNet / 300, accPrimary / 300, accMirror / 300, (accTotal - accNet - accPrimary - accMirror) / 300);
            uint32_t wc[2]; double wm[2];
            mp->takeWaitStats(wc, wm);
            LOGI("link waits per frame: console0 %.1f (%.2f ms)  console1 %.1f (%.2f ms)", wc[0] / 300.0, wm[0] / 300, wc[1] / 300.0, wm[1] / 300);
            accTotal = accNet = accPrimary = accMirror = 0; accN = 0;
        }
    }
    if ((framesRequested % 300) == 1)
        LOGI("frame %llu: primary NumFrames %u PC %08x keys %03x | mirror NumFrames %u PC %08x keys %03x running %d",
             (unsigned long long) framesRequested,
             primary->getNds()->NumFrames, primary->getNds()->ARM9.R[15], primary->getNds()->KeyInput & 0xFFF,
             mirror->getNds()->NumFrames, mirror->getNds()->ARM9.R[15], mirror->getNds()->KeyInput & 0xFFF,
             mirror->getNds()->IsRunning() ? 1 : 0);
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

    stopRecording("netplay");
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
    for (FILE*& f : netRecInputs)
        if (f) { fclose(f); f = nullptr; }
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
    netRecordStates(states[0].data(), states[0].size(), states[1].data(), states[1].size());
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
            netRecordStates(p, len0, p + len0, len1);
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
    if (netRecInputs[index])
        fwrite(&input, sizeof(FrameInput), 1, netRecInputs[index]);
}

void MirrorSession::netAfterFrame()
{
    netSendInputs();

    if (netFrame % kHashInterval == 0)
    {
        uint64_t h0 = DeterminismHarness::hashState(consoleBySession(0)->getNds());
        uint64_t h1 = DeterminismHarness::hashState(consoleBySession(1)->getNds());
        netLocalHashes.push_back({ netFrame, { h0, h1 } });
        // Evidence for desyncs: keep the last few snapshots of both consoles (written out on the first desync)
        NetDump dump;
        dump.frame = netFrame;
        appendRam(consoleBySession(0)->getNds(), dump.ram[0]);
        appendRam(consoleBySession(1)->getNds(), dump.ram[1]);
        // DEBUG: the first four checks are always written, so both devices have comparable snapshots
        if (netFrame <= 180 && !netRecDir.empty())
            for (int c = 0; c < 2; c++)
                writeFile(netRecDir + "/netrec/early-c" + std::to_string(c) + "-" + std::to_string(netFrame) + ".bin", dump.ram[c]);
        netDumps.push_back(std::move(dump));
        if (netDumps.size() > 4) netDumps.erase(netDumps.begin());
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
            if (netDesyncs == 1 && !netRecDir.empty())
            {
                for (const NetDump& d : netDumps)
                    if (d.frame == l.first)
                        for (int c = 0; c < 2; c++)
                            writeFile(netRecDir + "/netrec/desync-c" + std::to_string(c) + "-" + std::to_string(d.frame) + ".bin", d.ram[c]);
            }
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
// Every netplay session is recorded (start states + the inputs each console received) into <netRecDir>/netrec,
// in the same layout as mirror-record, so a session can be replayed offline with mirror-replay.
void MirrorSession::netRecordStates(const uint8_t* s0, size_t n0, const uint8_t* s1, size_t n1)
{
    if (netRecDir.empty()) return;
    std::string dir = netRecDir + "/netrec";
    mkdir(dir.c_str(), 0700);
    const uint8_t* data[2] = { s0, s1 };
    size_t size[2] = { n0, n1 };
    for (int i = 0; i < 2; i++)
        if (FILE* f = fopen((dir + "/state" + std::to_string(i) + ".bin").c_str(), "wb")) { fwrite(data[i], 1, size[i], f); fclose(f); }
}

void MirrorSession::netRecordOpenInputs()
{
    for (FILE*& f : netRecInputs)
        if (f) { fclose(f); f = nullptr; }
    if (netRecDir.empty()) return;
    for (int i = 0; i < 2; i++)
        netRecInputs[i] = fopen((netRecDir + "/netrec/inputs" + std::to_string(i) + ".bin").c_str(), "wb");
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
    netDumps.clear();
    netDesyncsShown = 0;
    netRecordOpenInputs();
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

    // Evidence for desync investigations: the firmware each console runs with
    if (!netRecDir.empty())
        mkdir((netRecDir + "/netrec").c_str(), 0700);
    if (!netRecDir.empty())
        for (int sid = 0; sid < 2; sid++)
        {
            const Firmware& fw = consoleBySession(sid)->getNds()->GetFirmware();
            writeFile(netRecDir + "/netrec/fw" + std::to_string(sid) + ".bin", std::vector<u8>(fw.Buffer(), fw.Buffer() + fw.Length()));
        }

    installLink();

    if (isHost)
        netRecordStates(ownState.data(), ownState.size(), remoteState.data(), remoteState.size());
    else
        netRecordStates(remoteState.data(), remoteState.size(), ownState.data(), ownState.size());

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
