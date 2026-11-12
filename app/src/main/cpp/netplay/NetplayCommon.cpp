#include "NetplayCommon.h"

#include <cstring>
#include "NDS.h"

using namespace melonDS;

namespace MelonDSAndroid
{

std::atomic<bool> forceSilentMic { false };

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
}

u64 hashConsoleState(NDS* nds)
{
    u64 h = kFnvOffset;
    h = hashBytes(h, nds->MainRAM, nds->MainRAMMask + 1);
    h = hashBytes(h, nds->SharedWRAM, nds->SharedWRAMSize);
    h = hashBytes(h, nds->ARM7WRAM, nds->ARM7WRAMSize);
    h = hashBytes(h, nds->ARM9.DTCM, melonDS::DTCMPhysicalSize);   // ARM9 stack
    h = hashBytes(h, nds->ARM9.R_FIQ, 20 * sizeof(u32));            // banked registers + SPSRs
    h = hashBytes(h, nds->ARM7.R_FIQ, 20 * sizeof(u32));
    h = hashBytes(h, nds->ARM9.R, sizeof(nds->ARM9.R));
    h = hashBytes(h, &nds->ARM9.CPSR, sizeof(nds->ARM9.CPSR));
    h = hashBytes(h, nds->ARM7.R, sizeof(nds->ARM7.R));
    h = hashBytes(h, &nds->ARM7.CPSR, sizeof(nds->ARM7.CPSR));
    return h;
}

}
