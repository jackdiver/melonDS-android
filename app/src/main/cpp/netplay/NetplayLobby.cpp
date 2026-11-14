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
constexpr uint16_t kSlotPortBase = 7080;   // one listening port per joiner slot (host)
constexpr uint16_t kMeshPortBase = 7200;   // joiner-to-joiner links: base + a * 8 + b
constexpr uint64_t kAutoStartMs = 8000;    // the host starts this long after the last joiner arrived
constexpr uint16_t kDiscoveryPort = 7065;
constexpr uint64_t kBeaconTimeoutMs = 1500;   // no beacon for this long: the game closed its group
constexpr uint64_t kScanDelayMs = 1000;       // wireless on without beacons for this long: the game is searching
constexpr uint64_t kAdvertIntervalMs = 500;
constexpr uint64_t kConnectTimeoutMs = 5000;
constexpr uint64_t kExchangeTimeoutMs = 30000;
const char kMagic[4] = { 'M', 'D', 'N', 'P' };

enum LobbyMsg : uint8_t { MsgHello = 'H', MsgState = 'S', MsgReject = 'R', MsgWelcome = 'W', MsgStart = 'G' };

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
    case Phase::Joined:
    case Phase::Exchanging: name = "joining"; break;
    }
    std::lock_guard<std::mutex> lock(messageMutex);
    return std::string(name) + " 0 0 " + message;
}

void NetplayLobby::reset()
{
    joiners.clear();
    listener.reset();
    rejected.reset();
    hostLink.reset();
    links.clear();
    closeUdp();
    welcomed = false;
    stateSent.clear();
    stateGot.clear();
    peerFirmware.clear();
    peerState.clear();
    ownState.clear();
    ownFirmware.clear();
    rejectUntilMs = 0;
    lastJoinMs = 0;
    startRequested = false;
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
    return true;
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


// Collects every packet that arrived on a link (in order)
static NetplayLink::Event pollLink(NetplayLink& link, int timeoutMs, std::vector<std::vector<uint8_t>>& messages)
{
    messages.clear();
    return link.service(timeoutMs, [&](const uint8_t* d, size_t s) { messages.emplace_back(d, d + s); });
}

static uint16_t meshPort(int a, int b)
{
    return (uint16_t) (kMeshPortBase + a * 8 + b);
}

void NetplayLobby::advertise(uint32_t gameCode)
{
    uint64_t now = nowMs();
    if (udpSocket < 0 || !listener || now - lastAdvertMs < kAdvertIntervalMs)
        return;
    lastAdvertMs = now;

    std::vector<uint8_t> msg(kMagic, kMagic + 4);
    msg.push_back(2); // version
    put(msg, gameCode);
    put(msg, (uint16_t) (kSlotPortBase + listenerSlot));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(udpSocket, msg.data(), msg.size(), 0, (sockaddr*) &addr, sizeof(addr));
}

bool NetplayLobby::findHost(uint32_t gameCode, std::string& address, uint16_t& port)
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
        uint32_t code; uint16_t advertisedPort;
        if (n < 5 || memcmp(buffer, kMagic, 4) != 0 || buffer[4] != 2 || !get(p, end, code) || !get(p, end, advertisedPort))
            continue;
        if (code != gameCode)
            continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        address = text;
        port = advertisedPort;
        found = true;
    }
    return found;
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
    hostLink->send(msg.data(), msg.size(), true);
    hostLink->flush();
}

void NetplayLobby::sendState(NetplayLink& to, const std::vector<uint8_t>& state, const std::vector<uint8_t>& firmware)
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
    to.send(msg.data(), msg.size(), true);
    to.flush();
    LOGI("sent own console (%zu bytes firmware, %zu bytes state, %zu compressed)", firmware.size(), state.size(), msg.size());
}

bool NetplayLobby::openListener()
{
    bool used[8] = {};
    for (const Joiner& j : joiners)
        used[j.slot] = true;
    if ((int) joiners.size() + 1 >= maxPlayers)
        return false; // full
    int slot = -1;
    for (int s = 0; s < 7; s++)
        if (!used[s]) { slot = s; break; }
    if (slot < 0)
        return false;
    listener = std::make_unique<NetplayLink>();
    if (!listener->host(kSlotPortBase + slot))
    {
        listener.reset();
        return false;
    }
    listenerSlot = slot;
    return true;
}

void NetplayLobby::beginExchange(int self, int count)
{
    selfSid = self;
    playerCount = count;
    links.clear();
    links.resize(count);
    stateSent.assign(count, 0);
    stateGot.assign(count, 0);
    stateSent[self] = stateGot[self] = 1;
    peerFirmware.assign(count, {});
    peerState.assign(count, {});
}

NetplayLobby::Action NetplayLobby::startAsHost(NDS* nds, uint64_t now)
{
    int count = (int) joiners.size() + 1;
    std::vector<std::string> ips;
    for (Joiner& j : joiners)
        ips.push_back(j.link->peerAddress());
    for (const std::string& ip : ips)
    {
        in_addr a {};
        if (inet_pton(AF_INET, ip.c_str(), &a) != 1)
        {
            setMessage("could not read a player's network address");
            reset();
            return Action::RunFrame;
        }
    }
    if (!snapshot(nds, ownState, ownFirmware))
    {
        reset();
        return Action::RunFrame;
    }
    for (size_t k = 0; k < joiners.size(); k++)
    {
        std::vector<uint8_t> msg { MsgStart, (uint8_t) count, (uint8_t) (k + 1) };
        for (const std::string& ip : ips)
        {
            in_addr a {};
            inet_pton(AF_INET, ip.c_str(), &a);
            put(msg, a.s_addr);
        }
        joiners[k].link->send(msg.data(), msg.size(), true);
        joiners[k].link->flush();
    }
    listener.reset();
    rejected.reset();
    closeUdp();
    std::vector<Joiner> mine = std::move(joiners);
    joiners.clear();
    beginExchange(0, count);
    for (size_t k = 0; k < mine.size(); k++)
        links[k + 1] = std::move(mine[k].link);
    setPhase(Phase::Exchanging);
    exchangeStartMs = now;
    LOGI("starting with %d players (host)", count);
    return Action::HoldFrame;
}

// All roles: every device holds its console still, links up with every other device and trades console states
NetplayLobby::Action NetplayLobby::exchange(uint64_t now)
{
    auto abort = [&](const char* text) {
        setMessage(text);
        LOGE("lobby: %s", text);
        reset();
        return Action::RunFrame;
    };
    if (now - exchangeStartMs > kExchangeTimeoutMs)
        return abort("the other devices did not send their consoles in time");

    bool allDone = true;
    bool waited = false;
    std::vector<std::vector<uint8_t>> messages;
    for (int sid = 0; sid < playerCount; sid++)
    {
        if (sid == selfSid)
            continue;
        NetplayLink* link = links[sid].get();
        if (!link)
            return abort("a player link is missing");
        NetplayLink::Event ev = pollLink(*link, waited || stateGot[sid] ? 0 : 2, messages);
        if (!stateGot[sid])
            waited = true;
        for (auto& m : messages)
        {
            if (!m.empty() && m[0] == MsgState && !stateGot[sid])
            {
                if (!readState(m.data(), m.size(), peerState[sid], peerFirmware[sid]))
                    return abort("could not read another player's console");
                stateGot[sid] = 1;
            }
        }
        if (ev == NetplayLink::Event::Disconnected && !stateGot[sid])
            return abort("another player disconnected");
        if (link->isConnected() && !stateSent[sid])
        {
            sendState(*link, ownState, ownFirmware);
            stateSent[sid] = 1;
        }
        if (!stateGot[sid] || !stateSent[sid])
            allDone = false;
    }
    if (!allDone)
        return Action::HoldFrame;

    pending = SessionStart();
    pending.selfSid = selfSid;
    pending.count = playerCount;
    pending.links = std::move(links);
    pending.ownState = std::move(ownState);
    pending.firmware = std::move(peerFirmware);
    pending.states = std::move(peerState);
    links.clear();
    reset();
    return Action::StartSession;
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
    std::vector<std::vector<uint8_t>> messages;

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
            joiners.clear();
            if (!openListener() || !openUdp(false))
            {
                setMessage("could not open the network ports");
                reset();
                return Action::RunFrame;
            }
            isHost = true;
            setMessage("");
            startRequested = false;
            lastJoinMs = 0;
            setPhase(Phase::Hosting);
            LOGI("game created a group: hosting");
        }
        else if (searching && softwareRenderer)
        {
            if (!openUdp(true))
                return Action::RunFrame;
            isHost = false;
            welcomed = false;
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

        // Joiners that are already in: drop the ones that left
        for (size_t i = 0; i < joiners.size();)
        {
            NetplayLink::Event ev = pollLink(*joiners[i].link, 0, messages);
            if (ev == NetplayLink::Event::Disconnected || !joiners[i].link->isConnected())
            {
                LOGI("a joiner left before the start");
                joiners.erase(joiners.begin() + i);
                setMessage(joiners.empty() ? "" : std::to_string(joiners.size() + 1) + " players");
                if (!listener && !rejected)
                    openListener();
                continue;
            }
            i++;
        }

        if (rejected && now >= rejectUntilMs)
            rejected.reset();
        else if (rejected)
            pollLink(*rejected, 0, messages);
        if (!listener && !rejected)
            openListener();

        if (listener)
        {
            NetplayLink::Event ev = pollLink(*listener, 0, messages);
            if (!messages.empty() && !messages[0].empty() && messages[0][0] == MsgHello)
            {
                std::string error;
                if (!checkHello(nds, messages[0].data(), messages[0].size(), error))
                {
                    setMessage(error);
                    std::vector<uint8_t> msg { MsgReject };
                    msg.insert(msg.end(), error.begin(), error.end());
                    listener->send(msg.data(), msg.size(), true);
                    listener->flush();
                    // Keep the connection open briefly so the rejection actually reaches the joiner
                    rejected = std::move(listener);
                    rejectUntilMs = now + 1000;
                }
                else
                {
                    uint8_t welcome = MsgWelcome;
                    listener->send(&welcome, 1, true);
                    listener->flush();
                    Joiner joiner;
                    joiner.link = std::move(listener);
                    joiner.slot = listenerSlot;
                    joiners.push_back(std::move(joiner));
                    lastJoinMs = now;
                    setMessage(std::to_string(joiners.size() + 1) + " players");
                    LOGI("a player joined the lobby (%zu joiners)", joiners.size());
                }
            }
            else if (ev == NetplayLink::Event::Disconnected)
            {
                listener.reset();
            }
        }

        bool requested = startRequested.exchange(false);
        if (!joiners.empty() && (requested || now - lastJoinMs >= kAutoStartMs || (int) joiners.size() + 1 >= maxPlayers))
            return startAsHost(nds, now);
        return Action::RunFrame;
    }

    case Phase::Searching:
    {
        // The game left the group list and created its own group: become the host instead
        if (beaconing && !hostLink)
        {
            reset();
            return Action::RunFrame;
        }
        if (!mp->wirelessOn.load())
        {
            reset();
            return Action::RunFrame;
        }
        if (!hostLink)
        {
            std::string address;
            uint16_t port = 0;
            if (findHost(gameCode, address, port))
            {
                hostLink = std::make_unique<NetplayLink>();
                if (!hostLink->join(address, port))
                {
                    hostLink.reset();
                    return Action::RunFrame;
                }
                hostAddress = address;
                hostPort = port;
                lastConnectMs = now;
                LOGI("found a host at %s:%u", address.c_str(), port);
            }
            return Action::RunFrame;
        }

        NetplayLink::Event ev = pollLink(*hostLink, 0, messages);
        if (ev == NetplayLink::Event::Connected)
            sendHello(nds);
        bool rejectedHere = false, welcomedHere = false;
        for (auto& m : messages)
        {
            if (m.empty()) continue;
            if (m[0] == MsgReject)
            {
                setMessage(std::string(m.begin() + 1, m.end()));
                rejectedHere = true;
            }
            else if (m[0] == MsgWelcome)
                welcomedHere = true;
        }
        if (rejectedHere)
        {
            reset();
            return Action::RunFrame;
        }
        if (welcomedHere)
        {
            welcomed = true;
            setPhase(Phase::Joined);
            LOGI("joined the lobby of %s", hostAddress.c_str());
            return Action::RunFrame;
        }
        // (a rejection can arrive together with the disconnect: handled above first)
        if (ev == NetplayLink::Event::Disconnected || (!hostLink->isConnected() && now - lastConnectMs > kConnectTimeoutMs))
        {
            // A full slot or a race with another joiner: look at the adverts again
            hostLink.reset();
            return Action::RunFrame;
        }
        return Action::RunFrame;
    }

    case Phase::Joined:
    {
        if (!mp->wirelessOn.load() || beaconing)
        {
            reset(); // the game left the group list
            return Action::RunFrame;
        }
        NetplayLink::Event ev = pollLink(*hostLink, 0, messages);
        for (size_t i = 0; i < messages.size(); i++)
        {
            const std::vector<uint8_t>& m = messages[i];
            if (m.size() < 3 || m[0] != MsgStart)
                continue;
            int count = m[1], sid = m[2];
            if (count < 2 || count > 8 || sid < 1 || sid >= count || m.size() < (size_t) (3 + 4 * (count - 1)))
            {
                setMessage("bad start message from the host");
                reset();
                return Action::RunFrame;
            }
            if (count > maxPlayers)
            {
                setMessage("this device cannot run that many players");
                reset();
                return Action::RunFrame;
            }
            if (!snapshot(nds, ownState, ownFirmware))
            {
                reset();
                return Action::RunFrame;
            }
            std::vector<std::string> ips(count); // by session id, host (0) unused
            for (int s = 1; s < count; s++)
            {
                in_addr a {};
                memcpy(&a.s_addr, &m[3 + 4 * (s - 1)], 4);
                char text[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &a, text, sizeof(text));
                ips[s] = text;
            }
            beginExchange(sid, count);
            links[0] = std::move(hostLink);
            for (int a = 1; a < sid; a++)
            {
                links[a] = std::make_unique<NetplayLink>();
                if (!links[a]->join(ips[a], meshPort(a, sid)))
                {
                    setMessage("could not connect to another player");
                    reset();
                    return Action::RunFrame;
                }
            }
            for (int b = sid + 1; b < count; b++)
            {
                links[b] = std::make_unique<NetplayLink>();
                if (!links[b]->host(meshPort(sid, b)))
                {
                    setMessage("could not open a port for another player");
                    reset();
                    return Action::RunFrame;
                }
            }
            // Whatever followed the start message on the host link (its console state) is handled now
            for (size_t k = i + 1; k < messages.size(); k++)
                if (!messages[k].empty() && messages[k][0] == MsgState && !stateGot[0])
                {
                    if (!readState(messages[k].data(), messages[k].size(), peerState[0], peerFirmware[0]))
                    {
                        setMessage("could not read the host's console");
                        reset();
                        return Action::RunFrame;
                    }
                    stateGot[0] = 1;
                }
            closeUdp();
            setPhase(Phase::Exchanging);
            exchangeStartMs = now;
            LOGI("starting with %d players (own session id %d)", count, sid);
            return Action::HoldFrame;
        }
        if (ev == NetplayLink::Event::Disconnected || !hostLink->isConnected())
        {
            setMessage("the host closed the group");
            reset();
            return Action::RunFrame;
        }
        return Action::RunFrame;
    }

    case Phase::Exchanging:
        return exchange(now);
    }
    return Action::RunFrame;
}

}
