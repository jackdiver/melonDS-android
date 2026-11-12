#include "NetplayLink.h"

#include <android/log.h>
#include <enet/enet.h>

#define TAG "NetplayLink"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace MelonDSAndroid
{

static bool ensureEnet()
{
    static bool initialised = false;
    if (!initialised)
        initialised = enet_initialize() == 0;
    return initialised;
}

NetplayLink::~NetplayLink()
{
    close();
}

bool NetplayLink::host(uint16_t port)
{
    close();
    if (!ensureEnet())
        return false;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    enetHost = enet_host_create(&address, 1, 2, 0, 0);
    if (!enetHost)
    {
        LOGE("cannot listen on port %u", port);
        return false;
    }
    LOGI("hosting on port %u", port);
    return true;
}

bool NetplayLink::join(const std::string& hostAddress, uint16_t port)
{
    close();
    if (!ensureEnet())
        return false;

    enetHost = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!enetHost)
        return false;

    ENetAddress address;
    if (enet_address_set_host_ip(&address, hostAddress.c_str()) != 0)
    {
        LOGE("bad address %s", hostAddress.c_str());
        close();
        return false;
    }
    address.port = port;
    peer = enet_host_connect(enetHost, &address, 2, 0);
    if (!peer)
    {
        close();
        return false;
    }
    LOGI("connecting to %s:%u", hostAddress.c_str(), port);
    return true;
}

void NetplayLink::close()
{
    if (peer && connected)
    {
        enet_peer_disconnect_now(peer, 0);
    }
    peer = nullptr;
    connected = false;
    if (enetHost)
    {
        enet_host_flush(enetHost);
        enet_host_destroy(enetHost);
        enetHost = nullptr;
    }
}

NetplayLink::Event NetplayLink::service(int timeoutMs, const std::function<void(const uint8_t*, size_t)>& onPacket)
{
    Event result = Event::None;
    if (!enetHost)
        return result;

    ENetEvent event;
    int wait = timeoutMs;
    while (enet_host_service(enetHost, &event, (enet_uint32) wait) > 0)
    {
        wait = 0; // drain whatever else is queued without waiting again
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            peer = event.peer;
            connected = true;
            // Generous timeouts: Wi-Fi power save can delay packets by a second or more
            enet_peer_timeout(peer, 0, 5000, 15000);
            LOGI("peer connected");
            result = Event::Connected;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            onPacket(event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            LOGI("peer disconnected");
            connected = false;
            peer = nullptr;
            return Event::Disconnected;
        default:
            break;
        }
    }
    return result;
}

void NetplayLink::send(const void* data, size_t size, bool reliable)
{
    if (!isConnected())
        return;
    ENetPacket* packet = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
    enet_peer_send(peer, reliable ? 0 : 1, packet);
}

void NetplayLink::flush()
{
    if (enetHost)
        enet_host_flush(enetHost);
}

}
