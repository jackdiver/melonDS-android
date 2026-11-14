#ifndef NETPLAYLINK_H
#define NETPLAYLINK_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace MelonDSAndroid
{

/**
 * Thin ENet wrapper for a two-player netplay session (phase 3 of docs/netplay-plan.md).
 * Channel 0 is reliable (session control, state, hashes), channel 1 unreliable (inputs, sent redundantly).
 * Not thread-safe: used from the emulator thread only.
 */
class NetplayLink
{
public:
    enum class Event { None, Connected, Disconnected };

    ~NetplayLink();

    bool host(uint16_t port);
    bool join(const std::string& address, uint16_t port);
    void close();

    bool isConnected() const { return peer != nullptr && connected; }
    uint32_t rttMs() const;     // ENet smoothed round trip time
    uint32_t rttVarianceMs() const;
    std::string peerAddress() const; // IPv4 text of the connected peer, empty if none

    // Waits up to timeoutMs for traffic; calls onPacket for each packet received. Returns the last
    // connection event seen.
    Event service(int timeoutMs, const std::function<void(const uint8_t* data, size_t size)>& onPacket);

    void send(const void* data, size_t size, bool reliable);
    void flush();

private:
    ENetHost* enetHost = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false;
};

}

#endif // NETPLAYLINK_H
