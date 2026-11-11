#include "DeterministicMP.h"

#include <algorithm>
#include <android/log.h>
#include <cstring>
#include <limits>
#include "NDS.h"

#define TAG "NetplayMP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

using namespace melonDS;

namespace MelonDSAndroid
{

namespace
{
constexpr int kMaxFrameSize = 0x948; // same limit as LocalMP

template <typename P>
bool packetBefore(const P& a, const P& b)
{
    if (a.time != b.time) return a.time < b.time;
    if (a.sender != b.sender) return a.sender < b.sender;
    return a.seq < b.seq;
}
}

DeterministicMP::DeterministicMP() = default;

DeterministicMP::~DeterministicMP()
{
    shutdown();
}

void DeterministicMP::registerInstance(int inst, NDS* nds)
{
    std::lock_guard<std::mutex> lock(mutex);
    Instance& i = instances[inst];
    i = Instance();
    i.registered = true;
    i.nds = nds;
    i.base = nds->GetSysClockCycles(0);
    i.published = 0;
    i.state = State::Barrier;
    changedLocked();
}

void DeterministicMP::setAtBarrier(int inst, bool atBarrier)
{
    std::lock_guard<std::mutex> lock(mutex);
    Instance& i = instances[inst];
    if (!i.registered)
        return;

    if (atBarrier)
    {
        // At a frame boundary; publish the exact time so others can proceed up to here
        publishLocked(inst, i.nds->GetSysClockCycles(0) - i.base);
        i.state = State::Barrier;
        changedLocked();
        resolveDeadlockLocked();
    }
    else
    {
        i.state = State::Running;
        changedLocked();
    }
}

void DeterministicMP::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex);
    shuttingDown = true;
    changedLocked();
}

uint64_t DeterministicMP::now(int inst)
{
    // Only ever called by the thread running this instance
    Instance& i = instances[inst];
    return i.nds->GetSysClockCycles(0) - i.base;
}

void DeterministicMP::publishLocked(int inst, uint64_t time)
{
    Instance& i = instances[inst];
    if (time > i.published)
    {
        i.published = time;
        changedLocked();
    }
}

void DeterministicMP::changedLocked()
{
    epoch++;
    cv.notify_all();
}

bool DeterministicMP::othersAtLeastLocked(int inst, uint64_t time) const
{
    for (int j = 0; j < kMaxInstances; j++)
    {
        if (j == inst || !instances[j].registered)
            continue;
        if (instances[j].published < time)
            return false;
    }
    return true;
}

bool DeterministicMP::connectedAtLocked(int inst, uint64_t time) const
{
    const Instance& i = instances[inst];
    return i.registered && i.everBegan && i.beginTime <= time && time < i.endTime;
}

bool DeterministicMP::repliedLocked(int inst) const
{
    for (const Packet& p : replies)
        if (p.sender == inst && p.time >= cmdTime)
            return true;
    return false;
}

bool DeterministicMP::hostWaitSatisfiedLocked(int inst)
{
    // Mirrors the wait condition in RecvHostPacket
    const Instance& me = instances[inst];
    int index = earliestPacketLocked(inst, false, 0);
    if (index >= 0 && othersAtLeastLocked(inst, me.inbox[index].time))
        return true;
    return othersAtLeastLocked(inst, me.waitTime + kHostPacketTimeout);
}

void DeterministicMP::resolveDeadlockLocked()
{
    // Deadlock: nobody is running, and every waiter has evaluated its condition at the current epoch.
    int candidate = -1;
    for (int j = 0; j < kMaxInstances; j++)
    {
        const Instance& i = instances[j];
        if (!i.registered)
            continue;
        if (i.state == State::Running)
            return;
        if (i.state == State::Waiting)
        {
            if (i.forced || i.checkedEpoch != epoch)
                return;
            if (candidate < 0 || i.deadline < instances[candidate].deadline)
                candidate = j; // ties keep the lowest instance id
        }
    }

    if (candidate >= 0)
    {
        instances[candidate].forced = true;
        changedLocked();
    }
}

template <typename Condition>
bool DeterministicMP::waitFor(std::unique_lock<std::mutex>& lock, int inst, uint64_t deadline, Condition condition)
{
    Instance& me = instances[inst];
    if (condition())
        return true;

    me.state = State::Waiting;
    me.deadline = deadline;
    me.forced = false;
    changedLocked();

    for (;;)
    {
        if (condition())
        {
            me.state = State::Running;
            changedLocked();
            return true;
        }
        if (me.forced || shuttingDown)
        {
            me.forced = false;
            me.state = State::Running;
            changedLocked();
            return false;
        }

        me.checkedEpoch = epoch;
        resolveDeadlockLocked();
        if (me.forced)
            continue;

        cv.wait(lock);
    }
}

int DeterministicMP::earliestPacketLocked(int inst, bool limitTime, uint64_t before)
{
    Instance& me = instances[inst];
    int best = -1;
    for (int k = 0; k < (int) me.inbox.size(); k++)
    {
        const Packet& p = me.inbox[k];
        if (p.time < me.beginTime)
            continue;
        if (limitTime && p.time >= before)
            continue;
        if (best < 0 || packetBefore(p, me.inbox[best]))
            best = k;
    }
    return best;
}

int DeterministicMP::deliverLocked(int inst, int index, u8* data, u64* timestamp)
{
    Instance& me = instances[inst];
    Packet p = std::move(me.inbox[index]);
    me.inbox.erase(me.inbox.begin() + index);

    // Drop packets that can never be delivered (older than this instance's Begin)
    me.inbox.erase(std::remove_if(me.inbox.begin(), me.inbox.end(), [&](const Packet& q) { return q.time < me.beginTime; }), me.inbox.end());

    if (!p.data.empty())
    {
        memcpy(data, p.data.data(), p.data.size());
        if ((p.type & 0xFFFF) == 1)
            me.lastHostId = p.sender;
    }
    if (timestamp)
        *timestamp = p.wifiTimestamp;
    return (int) p.data.size();
}

void DeterministicMP::Begin(int inst)
{
    std::lock_guard<std::mutex> lock(mutex);
    Instance& me = instances[inst];
    if (!me.registered)
        return;
    uint64_t t = now(inst);
    publishLocked(inst, t);
    me.connected = true;
    me.everBegan = true;
    me.beginTime = t;
    me.endTime = UINT64_MAX;
    me.lastHostId = -1;
    me.inbox.erase(std::remove_if(me.inbox.begin(), me.inbox.end(), [&](const Packet& q) { return q.time < t; }), me.inbox.end());
    changedLocked();
    LOGI("inst %d begin at %llu", inst, (unsigned long long) t);
}

void DeterministicMP::End(int inst)
{
    std::lock_guard<std::mutex> lock(mutex);
    Instance& me = instances[inst];
    if (!me.registered)
        return;
    uint64_t t = now(inst);
    publishLocked(inst, t);
    me.connected = false;
    me.endTime = t;
    me.inbox.clear();
    changedLocked();
    LOGI("inst %d end", inst);
}

int DeterministicMP::sendGeneric(int inst, uint32_t type, u8* data, int len, u64 timestamp)
{
    if (len > kMaxFrameSize)
        return 0;

    std::lock_guard<std::mutex> lock(mutex);
    Instance& me = instances[inst];
    if (!me.registered)
        return 0;

    uint64_t t = now(inst);
    publishLocked(inst, t);

    Packet p;
    p.time = t;
    p.sender = inst;
    p.seq = me.sendSeq++;
    p.type = type;
    p.wifiTimestamp = timestamp;
    p.data.assign(data, data + len);

    uint32_t kind = type & 0xFFFF;
    if (kind == 2)
    {
        replies.push_back(std::move(p));
    }
    else
    {
        if (kind == 1)
        {
            hostInst = inst;
            cmdTime = t;
            // Replies older than this CMD belong to earlier rounds (decided by time, not arrival order)
            replies.erase(std::remove_if(replies.begin(), replies.end(), [&](const Packet& q) { return q.time < t; }), replies.end());
        }

        for (int j = 0; j < kMaxInstances; j++)
        {
            Instance& other = instances[j];
            if (j == inst || !other.registered)
                continue;
            // Deliver to instances that are listening, or that are not yet past this time and may start listening.
            // Delivery itself filters by the receiver's Begin time.
            if (other.connected || other.published <= t)
                other.inbox.push_back(p);
        }
    }

    changedLocked();
    return len;
}

int DeterministicMP::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    return sendGeneric(inst, 0, data, len, timestamp);
}

int DeterministicMP::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    return sendGeneric(inst, 1, data, len, timestamp);
}

int DeterministicMP::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    return sendGeneric(inst, 2 | (aid << 16), data, len, timestamp);
}

int DeterministicMP::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    return sendGeneric(inst, 3, data, len, timestamp);
}

int DeterministicMP::RecvPacket(int inst, u8* data, u64* timestamp)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!instances[inst].registered)
        return 0;

    uint64_t t = now(inst);
    publishLocked(inst, t);
    waitFor(lock, inst, t, [&] { return othersAtLeastLocked(inst, t); });

    int index = earliestPacketLocked(inst, true, t);
    if (index < 0)
        return 0;
    return deliverLocked(inst, index, data, timestamp);
}

int DeterministicMP::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    std::unique_lock<std::mutex> lock(mutex);
    Instance& me = instances[inst];
    if (!me.registered)
        return 0;

    uint64_t t = now(inst);
    publishLocked(inst, t);

    if (me.lastHostId != -1)
    {
        // Did the host leave before now (in emulated time)? Wait until that is known.
        int host = me.lastHostId;
        waitFor(lock, inst, t, [&] {
            return instances[host].published >= t || instances[host].endTime != UINT64_MAX;
        });
        if (instances[host].endTime <= t)
            return -1;
    }

    uint64_t deadline = t + kHostPacketTimeout;

    me.waitingForHost = true;
    me.waitTime = t;
    waitFor(lock, inst, deadline, [&] { return hostWaitSatisfiedLocked(inst); });
    me.waitingForHost = false;

    int index = earliestPacketLocked(inst, false, 0);
    if (index >= 0 && othersAtLeastLocked(inst, me.inbox[index].time))
        return deliverLocked(inst, index, data, timestamp);
    return 0;
}

u16 DeterministicMP::RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!instances[inst].registered)
        return 0;

    uint64_t t = now(inst);
    publishLocked(inst, t);
    uint64_t deadline = t + kReplyWindow;
    uint64_t roundTime = cmdTime;

    waitFor(lock, inst, deadline, [&] {
        for (int j = 0; j < kMaxInstances; j++)
        {
            Instance& other = instances[j];
            if (j == inst || !other.registered)
                continue;
            // Membership at the time of the CMD is only known once that console has reached it
            if (other.published < roundTime && other.endTime == UINT64_MAX)
                return false;
            if (!connectedAtLocked(j, roundTime))
                continue;
            if (repliedLocked(j))
                continue;
            if (other.published >= deadline)
                continue;
            // Parked waiting for the next host frame after this CMD, with nothing that would wake it: no reply
            if (other.state == State::Waiting && other.waitingForHost && other.waitTime >= roundTime &&
                !hostWaitSatisfiedLocked(j))
                continue;
            return false;
        }
        return true;
    });

    std::vector<const Packet*> ordered;
    for (const Packet& p : replies)
        if (p.time >= roundTime)
            ordered.push_back(&p);
    std::sort(ordered.begin(), ordered.end(), [](const Packet* a, const Packet* b) { return packetBefore(*a, *b); });

    u16 ret = 0;
    for (const Packet* p : ordered)
    {
        if (p->sender == inst || p->wifiTimestamp + 32 < timestamp)
            continue; // own or stale
        u32 aid = p->type >> 16;
        if (aid < 1 || aid > 15 || p->data.size() > 1024)
            continue;
        memcpy(&data[(aid - 1) * 1024], p->data.data(), p->data.size());
        ret |= (1 << aid);
    }
    return ret;
}

}
