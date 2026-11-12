#include "NetplayLobby.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "MelonInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "net/MPInterface.h"

#define TAG "NetplayLobby"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace melonDS;

namespace MelonDSAndroid
{

namespace
{
constexpr uint16_t kSessionPort = 7064;
constexpr uint16_t kDiscoveryPort = 7065;
constexpr uint64_t kBeaconTimeoutMs = 1500;   // no beacon for this long: the game closed its group
constexpr uint64_t kScanDelayMs = 1000;       // wireless on without beacons for this long: the game is searching
constexpr uint64_t kAdvertIntervalMs = 500;
constexpr uint64_t kConnectTimeoutMs = 5000;
constexpr uint64_t kExchangeTimeoutMs = 15000;
const char kMagic[4] = { 'M', 'D', 'N', 'P' };

enum LobbyMsg : uint8_t { MsgHello = 'H', MsgState = 'S', MsgReject = 'R' };

uint64_t nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t fnv(const uint8_t* data, size_t size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i++)
        h = (h ^ data[i]) * 16777619u;
    return h;
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

struct GameId
{
    uint32_t gameCode = 0;
    uint16_t headerCrc = 0;
    uint32_t romLength = 0;
};

GameId gameIdOf(NDS* nds)
{
    GameId id;
    const NDSCart::CartCommon* cart = nds->NDSCartSlot.GetCart();
    if (cart)
    {
        id.gameCode = cart->GetHeader().GameCodeAsU32();
        id.headerCrc = cart->GetHeader().HeaderCRC16;
        id.romLength = cart->GetROMLength();
    }
    return id;
}
}

/**
 * Wireless interface used while no session runs: nothing is ever received, but what the game sends tells the
 * lobby whether it hosts a group (beacons) or searches for one.
 */
class LobbyMP : public MPInterface
{
public:
    static std::atomic<LobbyMP*> live;

    LobbyMP() { live = this; }
    ~LobbyMP() override
    {
        LobbyMP* self = this;
        live.compare_exchange_strong(self, nullptr);
    }

    std::atomic<bool> wirelessOn { false };
    std::atomic<uint64_t> beginMs { 0 };
    std::atomic<uint64_t> lastBeaconMs { 0 };

    void Process() override {}
    void Begin(int inst) override
    {
        if (inst != 0) return;
        wirelessOn = true;
        beginMs = nowMs();
    }
    void End(int inst) override
    {
        if (inst != 0) return;
        wirelessOn = false;
    }
    int SendPacket(int inst, u8* data, int len, u64 timestamp) override
    {
        // TX header (12 bytes), then the 802.11 frame; frame control 0x80 = beacon
        if (inst == 0 && len > 13 && data[12] == 0x80)
            lastBeaconMs = nowMs();
        return 0;
    }
    int RecvPacket(int inst, u8* data, u64* timestamp) override { return 0; }
    int SendCmd(int inst, u8* data, int len, u64 timestamp) override { return 0; }
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) override { return 0; }
    int SendAck(int inst, u8* data, int len, u64 timestamp) override { return 0; }
    int RecvHostPacket(int inst, u8* data, u64* timestamp) override { return 0; }
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) override { return 0; }
};

std::atomic<LobbyMP*> LobbyMP::live { nullptr };

NetplayLobby::NetplayLobby() = default;

NetplayLobby::~NetplayLobby()
{
    reset();
}

void NetplayLobby::installMp()
{
    LobbyMP* current = LobbyMP::live.load();
    if (current && &MPInterface::Get() == current)
    {
        mp = current;
        return;
    }
    auto lobbyMp = std::make_unique<LobbyMP>();
    mp = lobbyMp.get();
    MPInterface::SetCustom(std::move(lobbyMp), MPInterface_Dummy);
}

void NetplayLobby::setMessage(const std::string& text)
{
    std::lock_guard<std::mutex> lock(messageMutex);
    message = text;
}

void NetplayLobby::setPhase(Phase newPhase)
{
    phase = newPhase;
    shownPhase = (int) newPhase;
}

std::string NetplayLobby::status()
{
    const char* name = "idle";
    switch ((Phase) shownPhase.load())
    {
    case Phase::Idle: name = "idle"; break;
    case Phase::Hosting: name = "hosting"; break;
    case Phase::Searching: name = "searching"; break;
    case Phase::Exchanging: name = "joining"; break;
    }
    std::lock_guard<std::mutex> lock(messageMutex);
    return std::string(name) + " 0 0 " + message;
}

void NetplayLobby::reset()
{
    if (link)
    {
        link->close();
        link.reset();
    }
    closeUdp();
    helloReceived = false;
    stateSent = false;
    receivedPacket.clear();
    ownState.clear();
    ownFirmware.clear();
    setPhase(Phase::Idle);
}

bool NetplayLobby::openUdp(bool listen)
{
    closeUdp();
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket < 0)
        return false;
    int yes = 1;
    setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(udpSocket, F_SETFL, fcntl(udpSocket, F_GETFL, 0) | O_NONBLOCK);
    if (listen)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kDiscoveryPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(udpSocket, (sockaddr*) &addr, sizeof(addr)) != 0)
        {
            LOGE("cannot listen for hosts on port %u", kDiscoveryPort);
            closeUdp();
            return false;
        }
    }
    return true;
}

void NetplayLobby::closeUdp()
{
    if (udpSocket >= 0)
        ::close(udpSocket);
    udpSocket = -1;
}

void NetplayLobby::advertise(uint32_t gameCode)
{
    uint64_t now = nowMs();
    if (udpSocket < 0 || now - lastAdvertMs < kAdvertIntervalMs)
        return;
    lastAdvertMs = now;

    std::vector<uint8_t> msg(kMagic, kMagic + 4);
    msg.push_back(1); // version
    put(msg, gameCode);
    put(msg, kSessionPort);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(udpSocket, msg.data(), msg.size(), 0, (sockaddr*) &addr, sizeof(addr));
}

bool NetplayLobby::findHost(uint32_t gameCode, std::string& address)
{
    if (udpSocket < 0)
        return false;
    bool found = false;
    uint8_t buffer[64];
    for (;;)
    {
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (sockaddr*) &from, &fromLen);
        if (n <= 0)
            break;
        const uint8_t* p = buffer + 5;
        const uint8_t* end = buffer + n;
        uint32_t code; uint16_t port;
        if (n < 5 || memcmp(buffer, kMagic, 4) != 0 || buffer[4] != 1 || !get(p, end, code) || !get(p, end, port))
            continue;
        if (code != gameCode)
            continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        address = text;
        found = true;
    }
    return found;
}

bool NetplayLobby::snapshot(NDS* nds, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware)
{
    ScopedCurrentNds guard(nds);
    Savestate save;
    if (save.Error || !nds->DoSavestate(&save) || save.Error)
        return false;
    auto* bytes = static_cast<const u8*>(save.Buffer());
    state.assign(bytes, bytes + save.Length());
    const Firmware& fw = nds->GetFirmware();
    firmware.assign(fw.Buffer(), fw.Buffer() + fw.Length());
    return true;
}

void NetplayLobby::sendHello(NDS* nds)
{
    GameId id = gameIdOf(nds);
    std::vector<uint8_t> msg { MsgHello };
    put(msg, id.gameCode);
    put(msg, id.headerCrc);
    put(msg, id.romLength);
    put(msg, fnv(nds->GetARM9BIOS().data(), nds->GetARM9BIOS().size()));
    put(msg, fnv(nds->GetARM7BIOS().data(), nds->GetARM7BIOS().size()));
    const auto& mac = nds->GetFirmware().GetHeader().MacAddr;
    msg.insert(msg.end(), mac.begin(), mac.end());
    link->send(msg.data(), msg.size(), true);
    link->flush();
}

bool NetplayLobby::checkHello(NDS* nds, const uint8_t* data, size_t size, std::string& error)
{
    const uint8_t* p = data + 1;
    const uint8_t* end = data + size;
    GameId theirs;
    uint32_t bios9, bios7;
    uint8_t mac[6];
    if (!get(p, end, theirs.gameCode) || !get(p, end, theirs.headerCrc) || !get(p, end, theirs.romLength)
        || !get(p, end, bios9) || !get(p, end, bios7) || (size_t) (end - p) < 6)
    {
        error = "bad request from the other device";
        return false;
    }
    memcpy(mac, p, 6);

    GameId ours = gameIdOf(nds);
    if (theirs.gameCode != ours.gameCode || theirs.headerCrc != ours.headerCrc || theirs.romLength != ours.romLength)
    {
        error = "the other device is running a different game or version";
        return false;
    }
    if (bios9 != fnv(nds->GetARM9BIOS().data(), nds->GetARM9BIOS().size())
        || bios7 != fnv(nds->GetARM7BIOS().data(), nds->GetARM7BIOS().size()))
    {
        error = "the two devices use different BIOS files (both need the same BIOS setting)";
        return false;
    }
    const auto& ourMac = nds->GetFirmware().GetHeader().MacAddr;
    if (memcmp(mac, ourMac.data(), 6) == 0)
    {
        error = "both devices have the same MAC address; change one in Settings > Firmware";
        return false;
    }
    return true;
}

void NetplayLobby::sendState(const std::vector<uint8_t>& state, const std::vector<uint8_t>& firmware)
{
    std::vector<uint8_t> raw;
    put(raw, (uint32_t) firmware.size());
    raw.insert(raw.end(), firmware.begin(), firmware.end());
    put(raw, (uint32_t) state.size());
    raw.insert(raw.end(), state.begin(), state.end());

    uLongf packed = compressBound(raw.size());
    std::vector<uint8_t> msg(5 + packed);
    msg[0] = MsgState;
    uint32_t rawSize = (uint32_t) raw.size();
    memcpy(&msg[1], &rawSize, 4);
    if (compress2(&msg[5], &packed, raw.data(), raw.size(), 1) != Z_OK)
        return;
    msg.resize(5 + packed);
    link->send(msg.data(), msg.size(), true);
    link->flush();
    LOGI("sent own console (%zu bytes firmware, %zu bytes state, %zu compressed)", firmware.size(), state.size(), msg.size());
}

bool NetplayLobby::readState(const uint8_t* data, size_t size, std::vector<uint8_t>& state, std::vector<uint8_t>& firmware)
{
    if (size < 5) return false;
    uint32_t rawSize;
    memcpy(&rawSize, data + 1, 4);
    std::vector<uint8_t> raw(rawSize);
    uLongf rawLen = rawSize;
    if (rawSize == 0 || uncompress(raw.data(), &rawLen, data + 5, size - 5) != Z_OK)
        return false;
    const uint8_t* p = raw.data();
    const uint8_t* end = raw.data() + rawLen;
    uint32_t fwLen, stLen;
    if (!get(p, end, fwLen) || (size_t) (end - p) < fwLen) return false;
    firmware.assign(p, p + fwLen);
    p += fwLen;
    if (!get(p, end, stLen) || (size_t) (end - p) < stLen) return false;
    state.assign(p, p + stLen);
    return true;
}

NetplayLobby::Action NetplayLobby::poll(MelonInstance& console, bool softwareRenderer)
{
    installMp();
    NDS* nds = console.getNds();
    uint64_t now = nowMs();
    uint64_t lastBeacon = mp->lastBeaconMs.load();
    bool beaconing = lastBeacon != 0 && now - lastBeacon < kBeaconTimeoutMs;
    bool searching = mp->wirelessOn.load() && !beaconing && now - mp->beginMs.load() > kScanDelayMs;
    uint32_t gameCode = gameIdOf(nds).gameCode;

    auto receive = [this](std::vector<uint8_t>& out) {
        out.clear();
        return link->service(0, [&](const uint8_t* d, size_t s) { if (out.empty()) out.assign(d, d + s); });
    };

    switch (phase)
    {
    case Phase::Idle:
        if (beaconing)
        {
            if (!softwareRenderer)
            {
                setMessage("local multiplayer needs the software video renderer (Settings > Video)");
                return Action::RunFrame;
            }
            link = std::make_unique<NetplayLink>();
            if (!link->host(kSessionPort) || !openUdp(false))
            {
                setMessage("could not open the network ports");
                reset();
                return Action::RunFrame;
            }
            isHost = true;
            setMessage("");
            setPhase(Phase::Hosting);
            LOGI("game created a group: hosting");
        }
        else if (searching && softwareRenderer)
        {
            if (!openUdp(true))
                return Action::RunFrame;
            isHost = false;
            setMessage("");
            setPhase(Phase::Searching);
            LOGI("game is looking for groups: searching for a host");
        }
        return Action::RunFrame;

    case Phase::Hosting:
    {
        if (!beaconing)
        {
            LOGI("group closed");
            reset();
            return Action::RunFrame;
        }
        advertise(gameCode);
        NetplayLink::Event ev = receive(receivedPacket);
        if (ev == NetplayLink::Event::Disconnected)
        {
            // Back to waiting for another joiner
            reset();
            return Action::RunFrame;
        }
        if (!receivedPacket.empty() && receivedPacket[0] == MsgHello)
        {
            std::string error;
            if (!checkHello(nds, receivedPacket.data(), receivedPacket.size(), error))
            {
                setMessage(error);
                std::vector<uint8_t> msg { MsgReject };
                msg.insert(msg.end(), error.begin(), error.end());
                link->send(msg.data(), msg.size(), true);
                link->flush();
                reset();
                return Action::RunFrame;
            }
            if (!snapshot(nds, ownState, ownFirmware))
            {
                reset();
                return Action::RunFrame;
            }
            sendState(ownState, ownFirmware);
            setPhase(Phase::Exchanging);
            exchangeStartMs = now;
            // Hold this console still until the joiner's state arrives: the session starts from the state just sent
            return Action::HoldFrame;
        }
        return Action::RunFrame;
    }

    case Phase::Searching:
    {
        // The game left the group list and created its own group: become the host instead
        if (beaconing && !link)
        {
            reset();
            return Action::RunFrame;
        }
        if (!mp->wirelessOn.load())
        {
            reset();
            return Action::RunFrame;
        }
        if (!link)
        {
            std::string address;
            if (findHost(gameCode, address))
            {
                link = std::make_unique<NetplayLink>();
                if (!link->join(address, kSessionPort))
                {
                    link.reset();
                    return Action::RunFrame;
                }
                hostAddress = address;
                lastConnectMs = now;
                LOGI("found a host at %s", address.c_str());
            }
            return Action::RunFrame;
        }

        NetplayLink::Event ev = receive(receivedPacket);
        if (ev == NetplayLink::Event::Connected)
            sendHello(nds);
        if (ev == NetplayLink::Event::Disconnected || (!link->isConnected() && now - lastConnectMs > kConnectTimeoutMs))
        {
            link.reset();
            return Action::RunFrame;
        }
        if (!receivedPacket.empty() && receivedPacket[0] == MsgReject)
        {
            setMessage(std::string(receivedPacket.begin() + 1, receivedPacket.end()));
            link.reset();
            return Action::RunFrame;
        }
        if (!receivedPacket.empty() && receivedPacket[0] == MsgState)
        {
            SessionStart start;
            if (!readState(receivedPacket.data(), receivedPacket.size(), start.remoteState, start.remoteFirmware)
                || !snapshot(nds, start.ownState, ownFirmware))
            {
                setMessage("could not read the host's console");
                reset();
                return Action::RunFrame;
            }
            sendState(start.ownState, ownFirmware);
            start.isHost = false;
            start.link = std::move(link);
            pending = std::move(start);
            reset();
            return Action::StartSession;
        }
        return Action::RunFrame;
    }

    case Phase::Exchanging:
    {
        if (now - exchangeStartMs > kExchangeTimeoutMs)
        {
            setMessage("the other device did not send its console in time");
            reset();
            return Action::RunFrame;
        }
        // This console is held still: spend part of the frame receiving (the joiner's console is a few MB)
        NetplayLink::Event ev = NetplayLink::Event::None;
        receivedPacket.clear();
        for (int i = 0; i < 4 && receivedPacket.empty() && ev != NetplayLink::Event::Disconnected; i++)
            ev = link->service(4, [&](const uint8_t* d, size_t s) { if (receivedPacket.empty()) receivedPacket.assign(d, d + s); });
        if (ev == NetplayLink::Event::Disconnected)
        {
            setMessage("the other device disconnected");
            reset();
            return Action::RunFrame;
        }
        if (!receivedPacket.empty() && receivedPacket[0] == MsgState)
        {
            SessionStart start;
            if (!readState(receivedPacket.data(), receivedPacket.size(), start.remoteState, start.remoteFirmware))
            {
                setMessage("could not read the joiner's console");
                reset();
                return Action::RunFrame;
            }
            start.isHost = true;
            start.ownState = std::move(ownState);
            start.link = std::move(link);
            pending = std::move(start);
            reset();
            return Action::StartSession;
        }
        return Action::HoldFrame;
    }
    }
    return Action::RunFrame;
}

}
