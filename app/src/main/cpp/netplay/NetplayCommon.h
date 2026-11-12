#ifndef NETPLAYCOMMON_H
#define NETPLAYCOMMON_H

#include <atomic>
#include "types.h"

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

// Input applied to a console for one frame; the unit that travels between devices in a netplay session.
struct FrameInput
{
    melonDS::u32 keyMask;
    melonDS::u16 touchX;
    melonDS::u16 touchY;
    melonDS::u8 touching;
    melonDS::u8 lidClosed;
    melonDS::u8 reserved[2];
};
static_assert(sizeof(FrameInput) == 12, "FrameInput is sent over the network as-is");

// Set while a netplay session runs: the microphone must be silent (real microphone input differs between devices)
extern std::atomic<bool> forceSilentMic;

// Hash of the emulated state that decides what a game does next (RAM, ARM9 stack, CPU registers).
// Two devices running the same session must produce the same hash for the same frame; a difference is a desync.
melonDS::u64 hashConsoleState(melonDS::NDS* nds);

}

#endif // NETPLAYCOMMON_H
