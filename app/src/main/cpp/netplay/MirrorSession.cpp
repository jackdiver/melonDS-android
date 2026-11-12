#include "MirrorSession.h"

#include <android/log.h>
#include <pthread.h>
#include "DeterministicMP.h"
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

    // Note: JIT *with fast memory off* leaked host data into emulated RAM (non-deterministic across devices),
    // so the default (fast memory on) is kept. Fast memory faults are dispatched via the per-thread NDS::Current.
    // The threaded software renderer's timing can leak into emulated state (seen as replay mismatches on
    // the S23 from frame 0), so netplay renders 3D on the emulation thread
    setThreadedRendering(primaryInstance->getNds(), false);

    if (!mirrorInstance->cloneFrom(*primaryInstance))
    {
        LOGE("failed to clone the primary console into the mirror");
        return false;
    }

    primary = std::move(primaryInstance);
    mirror = std::move(mirrorInstance);
    setThreadedRendering(mirror->getNds(), false);

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
    primary->setInputHook([this](FrameInput& input) { inputHook(0, input); });
    mirror->setInputHook([this](FrameInput& input) { inputHook(1, input); });

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
    forceSilentMic = false;
    primary->setInputHook(nullptr);
    primary->setPresentationSource(nullptr);
    primary->clearInput();
    MPInterface::Set(MPInterface_Dummy);
    mp = nullptr;
    mirror.reset();
    primary.reset();
    LOGI("mirror session stopped");
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
    link->registerInstance(primary->getInstanceId(), primary->getNds());
    link->registerInstance(mirror->getInstanceId(), mirror->getNds());
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

    // Consoles run one after the other: running them concurrently has a timing race that makes replays
    // unrepeatable (see docs/netplay-plan.md). DEBUG flag 32 brings back the concurrent mode.
    bool serial = (debugFlags & 32) == 0;
    if (!serial)
        kickMirror();

    mp->setAtBarrier(primary->getInstanceId(), false);
    u32 lines = primary->runFrame();
    if (serial)
    {
        mp->setAtBarrier(primary->getInstanceId(), true);
        kickMirror();
    }
    waitForMirror();
    if (capture)
        melonDS::JitTraceHook9 = nullptr;

    if (recMode.load() != RecMode::Off && hashFile)
    {
        char line[128];
        snprintf(line, sizeof(line), "%u %016llx %016llx", recFrame,
                 (unsigned long long) DeterminismHarness::hashState(primary->getNds()),
                 (unsigned long long) DeterminismHarness::hashState(mirror->getNds()));
        fprintf(hashFile, "%s\n", line);

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

}
