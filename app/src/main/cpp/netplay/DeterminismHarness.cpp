#include "DeterminismHarness.h"

#include <android/log.h>
#include <cstring>
#include <vector>
#include "NDS.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "SPI.h"

#define TAG "NetplayTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace melonDS;

namespace MelonDSAndroid
{

DeterminismHarness determinismHarness;
std::atomic<bool> forceSilentMic { false };
std::atomic<bool> unthrottleForReplay { false };

namespace
{
constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL;
constexpr u64 kFnvPrime = 0x100000001b3ULL;

// Word-wise FNV-style hash. Not cryptographic; only used to detect divergence.
u64 hashBytes(u64 h, const void* data, size_t length)
{
    const u8* bytes = static_cast<const u8*>(data);
    size_t i = 0;
    for (; i + 8 <= length; i += 8)
    {
        u64 word;
        memcpy(&word, bytes + i, 8);
        h = (h ^ word) * kFnvPrime;
    }
    for (; i < length; i++)
        h = (h ^ bytes[i]) * kFnvPrime;
    return h;
}

std::string join(const std::string& dir, const char* name)
{
    if (!dir.empty() && dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}
}

void DeterminismHarness::requestRecord(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(requestMutex);
    requestDirectory = dir;
    requestTag = "record";
    pendingRequest = Request::Record;
}

void DeterminismHarness::requestReplay(const std::string& dir, const std::string& tag)
{
    std::lock_guard<std::mutex> lock(requestMutex);
    requestDirectory = dir;
    requestTag = tag.empty() ? "replay" : tag;
    pendingRequest = Request::Replay;
}

void DeterminismHarness::requestStop()
{
    pendingRequest = Request::Stop;
}

void DeterminismHarness::requestSelfTest(int frames)
{
    std::lock_guard<std::mutex> lock(requestMutex);
    selfTestRequestedFrames = frames > 0 ? frames : 300;
    pendingRequest = Request::SelfTest;
}

void DeterminismHarness::applyInput(NDS* nds, const FrameInput& input)
{
    nds->SetKeyMask(input.keyMask);
    if (input.touching)
        nds->TouchScreen(input.touchX, input.touchY);
    else
        nds->ReleaseScreen();
}

u64 DeterminismHarness::hashState(NDS* nds)
{
    u64 h = kFnvOffset;
    // Hash all of main RAM. (An earlier version skipped 16 bytes at 0x0216FF98; they turned out to be a saved
    // CPU context the game reads back, so differences there are real. See docs/netplay-plan.md.)
    h = hashBytes(h, nds->MainRAM, nds->MainRAMMask + 1);
    h = hashBytes(h, nds->SharedWRAM, nds->SharedWRAMSize);
    h = hashBytes(h, nds->ARM7WRAM, nds->ARM7WRAMSize);
    h = hashBytes(h, nds->ARM9.DTCM, melonDS::DTCMPhysicalSize);          // ARM9 stack
    h = hashBytes(h, nds->ARM9.R_FIQ, 20 * sizeof(u32));                   // banked registers + SPSRs
    h = hashBytes(h, nds->ARM7.R_FIQ, 20 * sizeof(u32));
    h = hashBytes(h, nds->ARM9.R, sizeof(nds->ARM9.R));
    h = hashBytes(h, &nds->ARM9.CPSR, sizeof(nds->ARM9.CPSR));
    h = hashBytes(h, nds->ARM7.R, sizeof(nds->ARM7.R));
    h = hashBytes(h, &nds->ARM7.CPSR, sizeof(nds->ARM7.CPSR));
    return h;
}

void DeterminismHarness::writeEnvironment(NDS* nds, const std::string& tag)
{
    std::string path = join(directory, ("env-" + tag + ".txt").c_str());
    FILE* f = fopen(path.c_str(), "w");
    if (!f)
    {
        LOGE("cannot write %s", path.c_str());
        return;
    }

    const auto& arm9bios = nds->GetARM9BIOS();
    const auto& arm7bios = nds->GetARM7BIOS();
    const auto& firmware = nds->SPI.GetFirmware();
    const NDSCart::CartCommon* cart = nds->NDSCartSlot.GetCart();

    fprintf(f, "arm9bios %016llx\n", (unsigned long long) hashBytes(kFnvOffset, arm9bios.data(), arm9bios.size()));
    fprintf(f, "arm7bios %016llx\n", (unsigned long long) hashBytes(kFnvOffset, arm7bios.data(), arm7bios.size()));
    fprintf(f, "firmware %016llx\n", (unsigned long long) hashBytes(kFnvOffset, firmware.Buffer(), firmware.Length()));
    {
        const auto& header = firmware.GetHeader();
        const auto& user = firmware.GetEffectiveUserData();
        fprintf(f, "fw.header   %016llx\n", (unsigned long long) hashBytes(kFnvOffset, &header, sizeof(header)));
        fprintf(f, "fw.user     %016llx\n", (unsigned long long) hashBytes(kFnvOffset, &user, sizeof(user)));
        fprintf(f, "fw.mac      %02x:%02x:%02x:%02x:%02x:%02x\n", header.MacAddr[0], header.MacAddr[1], header.MacAddr[2], header.MacAddr[3], header.MacAddr[4], header.MacAddr[5]);
        fprintf(f, "fw.settings %04x birthday %u/%u color %u namelen %u\n", user.Settings, user.BirthdayMonth, user.BirthdayDay, user.FavoriteColor, user.NameLength);
        // Hash of everything after the user data area (Wi-Fi access point settings etc.)
        u32 apStart = firmware.GetWifiAccessPointOffset();
        if (apStart < firmware.Length())
            fprintf(f, "fw.wifi-ap  %016llx\n", (unsigned long long) hashBytes(kFnvOffset, firmware.Buffer() + apStart, firmware.Length() - apStart));
    }
    if (cart)
        fprintf(f, "rom      %016llx\n", (unsigned long long) hashBytes(kFnvOffset, cart->GetROM(), cart->GetROMLength()));
    fprintf(f, "state0   %016llx\n", (unsigned long long) hashState(nds));
    fclose(f);
    LOGI("environment written to %s", path.c_str());
}

void DeterminismHarness::startRecord(NDS* nds)
{
    Savestate state;
    if (state.Error || !nds->DoSavestate(&state) || state.Error)
    {
        LOGE("record: failed to create savestate");
        return;
    }

    std::string statePath = join(directory, "state.bin");
    FILE* f = fopen(statePath.c_str(), "wb");
    if (!f || fwrite(state.Buffer(), 1, state.Length(), f) != state.Length())
    {
        LOGE("record: failed to write %s", statePath.c_str());
        if (f) fclose(f);
        return;
    }
    fclose(f);

    inputFile = fopen(join(directory, "inputs.bin").c_str(), "wb");
    hashFile = fopen(join(directory, "hashes-record.txt").c_str(), "w");
    if (!inputFile || !hashFile)
    {
        stop("record: cannot open output files");
        return;
    }

    writeEnvironment(nds, "record");
    frameIndex = 0;
    mode = Mode::Record;
    LOGI("record: started (state %u bytes)", state.Length());
}

void DeterminismHarness::startReplay(NDS* nds)
{
    std::string statePath = join(directory, "state.bin");
    FILE* f = fopen(statePath.c_str(), "rb");
    if (!f)
    {
        LOGE("replay: missing %s", statePath.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<u8> buffer(size);
    bool ok = fread(buffer.data(), 1, size, f) == (size_t) size;
    fclose(f);
    if (!ok)
    {
        LOGE("replay: failed to read state");
        return;
    }

    Savestate state(buffer.data(), (u32) size, false);
    // Load directly into the core: unlike MelonInstance::loadState this does NOT reset the RTC to the
    // device clock, which would make the replay diverge.
    if (state.Error || !nds->DoSavestate(&state) || state.Error)
    {
        LOGE("replay: failed to load state");
        return;
    }

    inputFile = fopen(join(directory, "inputs.bin").c_str(), "rb");
    hashFile = fopen(join(directory, ("hashes-" + requestTag + ".txt").c_str()).c_str(), "w");
    if (!inputFile || !hashFile)
    {
        stop("replay: cannot open files");
        return;
    }

    writeEnvironment(nds, requestTag);
    frameIndex = 0;
    mode = Mode::Replay;
    LOGI("replay: started (%s)", requestTag.c_str());
}

void DeterminismHarness::stop(const char* reason)
{
    if (inputFile) { fclose(inputFile); inputFile = nullptr; }
    if (hashFile) { fclose(hashFile); hashFile = nullptr; }
    if (mode != Mode::Off)
        LOGI("stopped after %u frames: %s", frameIndex, reason);
    mode = Mode::Off;
}

void DeterminismHarness::beforeFrame(NDS* nds, FrameInput& input)
{
    Request request = pendingRequest.exchange(Request::None);
    if (request != Request::None)
    {
        stop("new request");
        if (request != Request::Stop)
        {
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                directory = requestDirectory;
            }
            if (request == Request::Record)
                startRecord(nds);
            else if (request == Request::Replay)
                startReplay(nds);
            else
            {
                selfTestFramesLeft = selfTestRequestedFrames;
                selfTestMismatches = 0;
                frameIndex = 0;
                mode = Mode::SelfTest;
                LOGI("selftest: started for %d frames", selfTestFramesLeft);
            }
        }
    }

    switch (mode.load())
    {
    case Mode::Record:
        fwrite(&input, sizeof(FrameInput), 1, inputFile);
        break;
    case Mode::Replay:
        if (fread(&input, sizeof(FrameInput), 1, inputFile) != 1)
        {
            stop("end of recorded input");
            // Neutral input for the rest of this frame
            input = FrameInput { 0xFFF, 0, 0, 0, 0, {0, 0} };
        }
        break;
    case Mode::SelfTest:
    {
        Savestate state;
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
        {
            stop("selftest: snapshot failed");
            break;
        }
        auto* bytes = static_cast<const u8*>(state.Buffer());
        selfTestSnapshot.assign(bytes, bytes + state.Length());
        selfTestInput = input;
        break;
    }
    case Mode::Off:
        break;
    }
}

void DeterminismHarness::afterFrame(NDS* nds)
{
    if (mode.load() == Mode::Off)
        return;

    if (mode.load() == Mode::SelfTest)
    {
        u64 first = hashState(nds);

        // Rewind to the snapshot taken before this frame and run the same frame again
        Savestate state(selfTestSnapshot.data(), (u32) selfTestSnapshot.size(), false);
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
        {
            stop("selftest: rewind failed");
            return;
        }
        applyInput(nds, selfTestInput);
        nds->RunFrame();
        u64 second = hashState(nds);

        if (first != second)
        {
            selfTestMismatches++;
            if (selfTestMismatches <= 10)
                LOGI("selftest: frame %u MISMATCH %016llx vs %016llx", frameIndex, (unsigned long long) first, (unsigned long long) second);
        }

        frameIndex++;
        if (--selfTestFramesLeft <= 0)
        {
            LOGI("selftest: RESULT %d/%u frames mismatched", selfTestMismatches, frameIndex);
            stop("selftest finished");
        }
        return;
    }

    u64 h = hashState(nds);
    fprintf(hashFile, "%u %016llx\n", frameIndex, (unsigned long long) h);
    if ((frameIndex % 600) == 0)
    {
        fflush(hashFile);
        LOGI("frame %u hash %016llx", frameIndex, (unsigned long long) h);
    }
    frameIndex++;
}

}
