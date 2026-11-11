#ifndef DETERMINISTICMP_H
#define DETERMINISTICMP_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include "net/MPInterface.h"

namespace melonDS { class NDS; }

namespace MelonDSAndroid
{

/**
 * In-process local-wireless link between several emulated consoles that is deterministic:
 * the outcome depends only on emulated time, never on thread scheduling. Required for lockstep netplay,
 * where every device runs every player's console and they must all stay identical.
 *
 * Rules (all times are "session time" = console system clock cycles since the session started):
 * - A packet sent at time t is visible to a poll at time T only if t < T, and the poll first waits until
 *   every other console has published a time >= T (so no earlier packet can still appear).
 * - Packets are delivered in (time, sender, sequence) order.
 * - Blocking waits (host packets, MP replies) time out in emulated time, not wall-clock time.
 * - If every console is waiting (or parked at the frame barrier), the waiter with the earliest deadline
 *   is released with a timeout. This rule is deterministic, so all devices resolve it the same way.
 *
 * Supports up to 16 instances; written for the 2-player case first (see docs/netplay-plan.md).
 */
class DeterministicMP : public melonDS::MPInterface
{
public:
    DeterministicMP();
    ~DeterministicMP() override;

    // Called on the emulator thread at a frame boundary, before the instance's thread starts running frames.
    void registerInstance(int inst, melonDS::NDS* nds);
    // Frame barrier bookkeeping: an instance parked at the barrier cannot advance until the next frame.
    void setAtBarrier(int inst, bool atBarrier);
    // Wake and release every waiter (session teardown).
    void shutdown();

    void Process() override {}

    void Begin(int inst) override;
    void End(int inst) override;

    int SendPacket(int inst, melonDS::u8* data, int len, melonDS::u64 timestamp) override;
    int RecvPacket(int inst, melonDS::u8* data, melonDS::u64* timestamp) override;
    int SendCmd(int inst, melonDS::u8* data, int len, melonDS::u64 timestamp) override;
    int SendReply(int inst, melonDS::u8* data, int len, melonDS::u64 timestamp, melonDS::u16 aid) override;
    int SendAck(int inst, melonDS::u8* data, int len, melonDS::u64 timestamp) override;
    int RecvHostPacket(int inst, melonDS::u8* data, melonDS::u64* timestamp) override;
    melonDS::u16 RecvReplies(int inst, melonDS::u8* data, melonDS::u64 timestamp, melonDS::u16 aidmask) override;

private:
    static constexpr int kMaxInstances = 16;
    // System clock is 33.513982 MHz
    static constexpr uint64_t kReplyWindow = 67028;        // 2 ms
    static constexpr uint64_t kHostPacketTimeout = 837850; // 25 ms, same as LocalMP's wall-clock timeout

    struct Packet
    {
        uint64_t time;
        int sender;
        uint32_t seq;
        uint32_t type;      // low 16 bits: 0=regular 1=CMD 2=reply 3=ack; high 16 bits: aid for replies
        uint64_t wifiTimestamp;
        std::vector<melonDS::u8> data;
    };

    enum class State { Running, Waiting, Barrier };

    struct Instance
    {
        bool registered = false;
        melonDS::NDS* nds = nullptr;
        uint64_t base = 0;
        uint64_t published = 0;
        State state = State::Barrier;
        uint64_t deadline = 0;
        uint64_t checkedEpoch = 0;
        bool forced = false;
        bool waitingForHost = false;
        uint64_t waitTime = 0;
        bool connected = false;
        bool everBegan = false;
        uint64_t beginTime = 0;
        uint64_t endTime = UINT64_MAX;
        uint32_t sendSeq = 0;
        int lastHostId = -1;
        std::deque<Packet> inbox;
    };

    template <typename Condition>
    bool waitFor(std::unique_lock<std::mutex>& lock, int inst, uint64_t deadline, Condition condition);
    void resolveDeadlockLocked();
    void changedLocked();

    uint64_t now(int inst);
    void publishLocked(int inst, uint64_t time);
    bool othersAtLeastLocked(int inst, uint64_t time) const;
    // Connection state as of an emulated time (only valid once that instance has published >= time)
    bool connectedAtLocked(int inst, uint64_t time) const;
    // True if instance `inst`, waiting in RecvHostPacket, will be released by data/time already available
    bool hostWaitSatisfiedLocked(int inst);
    bool repliedLocked(int inst) const;
    // Returns index into inbox of the earliest deliverable packet (time >= beginTime), or -1
    int earliestPacketLocked(int inst, bool limitTime, uint64_t before);
    int sendGeneric(int inst, uint32_t type, melonDS::u8* data, int len, melonDS::u64 timestamp);
    int deliverLocked(int inst, int index, melonDS::u8* data, melonDS::u64* timestamp);

    std::mutex mutex;
    std::condition_variable cv;
    uint64_t epoch = 0;
    bool shuttingDown = false;
    Instance instances[kMaxInstances];

    // Current MP round (last CMD sent)
    int hostInst = -1;
    uint64_t cmdTime = 0;
    // Replies are matched to a CMD by emulated time (reply.time >= cmdTime), never by arrival order
    std::vector<Packet> replies;
};

}

#endif // DETERMINISTICMP_H
