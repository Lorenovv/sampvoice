/*
 * The legacy SampVoice control plug-in hooks RakNet and dereferences CNetGame.
 * This component uses only the public open.mp SDK instead.
 */

#include <bitstream.hpp>
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sampvoice_omp/protocol.hpp"

namespace
{
constexpr std::uint16_t ControlPort = 2020;
constexpr std::uint16_t VoicePort = 2020;
constexpr float VoiceDistance = 20.0f;
constexpr float VoiceDistanceSquared = VoiceDistance * VoiceDistance;
constexpr std::uint32_t ProximityChannel = 1;
constexpr std::uint32_t PhoneChannel = 2;
constexpr std::uint8_t VoicePushToTalkKey = 0x5A; // Z
constexpr std::uint16_t PhoneStreamFirst = 1024;
constexpr std::uint16_t PhoneStreamLimit = 4096;
constexpr std::uint8_t CommandPlayerCreate = 1;
constexpr std::uint8_t CommandPlayerSpeaker = 3;
constexpr std::uint8_t CommandPlayerAttachStream = 4;
constexpr std::uint8_t CommandPlayerDetachStream = 5;
constexpr std::uint8_t CommandPlayerDelete = 6;
constexpr std::uint8_t CommandStreamCreate = 7;
constexpr std::uint8_t CommandStreamTransiter = 8;
constexpr std::uint8_t CommandStreamAttachListener = 9;
constexpr std::uint8_t CommandStreamDetachListener = 10;
constexpr std::uint8_t CommandStreamDelete = 11;

constexpr std::array<std::uint8_t, 3> ManikularMagic { 'M', 'V', 1 };
constexpr std::uint8_t ManikularSnapshot = 1;
constexpr std::uint8_t ManikularSetMicroEnable = 1;
constexpr std::uint8_t ManikularSetMicroVolume = 2;
constexpr std::uint8_t ManikularSetSoundEnable = 3;
constexpr std::uint8_t ManikularSetSoundVolume = 4;
constexpr std::uint8_t ManikularSetSoundBalancer = 5;
constexpr std::uint8_t ManikularSetSoundFilter = 6;
constexpr std::uint8_t ManikularSetMicroDevice = 7;
constexpr std::uint8_t ManikularSetMicroCheck = 8;
constexpr std::uint8_t ManikularResetAudio = 9;
constexpr std::uint8_t ManikularBlacklistAdd = 10;
constexpr std::uint8_t ManikularBlacklistRemove = 11;

class CommandRelay final
{
public:
    ~CommandRelay()
    {
        closeConnection();
    }

    bool start()
    {
        return true;
    }

    bool tick()
    {
        bool connected = false;
        if (connection == -1)
        {
            const int candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (candidate != -1)
            {
                sockaddr_in address {};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(ControlPort);
                if (connect(candidate, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                    fcntl(candidate, F_SETFL, O_NONBLOCK) != -1 && shutdown(candidate, SHUT_RD) == 0)
                {
                    connection = candidate;
                    ++generation;
                    connected = true;
                }
                else
                {
                    close(candidate);
                }
            }
        }

        flush();
        return connected;
    }

    bool isConnected() const
    {
        return connection != -1;
    }

    std::uint64_t currentGeneration() const
    {
        return generation;
    }

    void enqueue(std::vector<std::uint8_t>&& command)
    {
        pending.insert(pending.end(), command.begin(), command.end());
    }

private:
    void flush()
    {
        while (connection != -1 && offset < pending.size())
        {
            const ssize_t sent = send(connection, pending.data() + offset, pending.size() - offset, MSG_NOSIGNAL);
            if (sent > 0)
            {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break;
            }
            closeConnection();
        }

        if (offset == pending.size())
        {
            pending.clear();
            offset = 0;
        }
    }

    void closeConnection()
    {
        if (connection != -1)
        {
            close(connection);
            connection = -1;
        }
        pending.clear();
        offset = 0;
    }

    int connection = -1;
    std::uint64_t generation = 0;
    std::vector<std::uint8_t> pending;
    std::size_t offset = 0;
};

struct Session
{
    sampvoice_omp::Handshake handshake {};
    std::uint32_t voiceKey = 0;
    std::uint64_t initializedGeneration = 0;
    std::unordered_set<int> listeners;
};

struct PhoneCall
{
    std::uint16_t stream;
    int firstPlayer;
    int secondPlayer;
};

struct SpatialCell
{
    int x;
    int y;
    int z;
    int world;
    unsigned interior;

    bool operator==(const SpatialCell& other) const
    {
        return x == other.x && y == other.y && z == other.z &&
            world == other.world && interior == other.interior;
    }
};

struct SpatialCellHash
{
    std::size_t operator()(const SpatialCell& cell) const
    {
        std::size_t value = static_cast<std::size_t>(cell.x);
        value = value * 31 + static_cast<std::size_t>(cell.y);
        value = value * 31 + static_cast<std::size_t>(cell.z);
        value = value * 31 + static_cast<std::size_t>(cell.world);
        return value * 31 + static_cast<std::size_t>(cell.interior);
    }
};

struct VoicePlayer
{
    IPlayer* player;
    Vector3 position;
    SpatialCell cell;
};

class SampVoiceOMPControl final : public IComponent, public SingleNetworkInEventHandler,
    public NetworkEventHandler, public CoreEventHandler, public PawnEventHandler
{
public:
    StringView componentName() const override
    {
        return "SampVoiceOMPControl";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(0, 3, 0, 0);
    }

    UID getUID() override
    {
        return UID(0x823e6cc27f7b9a31);
    }

    void onLoad(ICore* loadedCore) override
    {
        core = loadedCore;
        instance = this;
        relay.start();
        core->getEventDispatcher().addEventHandler(this);
    }

    void onInit(IComponentList* components) override
    {
        pawnComponent = components->queryComponent<IPawnComponent>();
        if (pawnComponent != nullptr)
        {
            pawnComponent->getEventDispatcher().addEventHandler(this);
            pawnHandlerRegistered = true;
        }
    }

    void onAmxLoad(IPawnScript& script) override
    {
        script.Register("MV_RequestVoiceSettings", NativeRequestVoiceSettings);
        script.Register("MV_SetVoiceOption", NativeSetVoiceOption);
        script.Register("MV_SetVoiceBlacklist", NativeSetVoiceBlacklist);
        script.Register("MV_IsVoiceReady", NativeIsVoiceReady);
        script.Register("MV_StartPhoneCall", NativeStartPhoneCall);
        script.Register("MV_StopPhoneCall", NativeStopPhoneCall);
    }

    void onAmxUnload(IPawnScript&) override
    {
    }

    void onReady() override
    {
        // LegacyNetwork is loaded after normal components. Registering here
        // ensures RPC 25 is attached to the real game network, not an empty
        // network list during component discovery.
        core->addPerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
        core->addPerPacketInEventHandler<sampvoice_omp::ControlPacket>(this);
        core->addNetworkEventHandler(this);
        networkHandlersRegistered = true;
    }

    void onTick(Microseconds, TimePoint) override
    {
        if (relay.tick())
        {
            synchronizeAll();
        }

        const TimePoint now = Time::now();
        if (now >= nextProximityUpdate)
        {
            nextProximityUpdate = now + Milliseconds(200);
            updateProximity();
        }
    }

    void onPeerDisconnect(IPlayer& peer, PeerDisconnectReason) override
    {
        const int playerId = peer.getID();
        stopPhoneCall(playerId);
        auto source = sessions.find(playerId);
        if (source != sessions.end())
        {
            for (const int listenerId : source->second.listeners)
            {
                detachListener(playerId, listenerId, true);
            }
            sessions.erase(source);
        }

        for (auto& entry : sessions)
        {
            if (entry.second.listeners.erase(playerId) != 0)
            {
                queueStreamListener(CommandStreamDetachListener, entry.first, playerId);
            }
        }

        if (relay.isConnected())
        {
            std::vector<std::uint8_t> command(3);
            command[0] = CommandPlayerDelete;
            sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(playerId));
            relay.enqueue(std::move(command));
        }
    }

    bool onReceive(IPlayer& peer, NetworkBitStream& stream) override
    {
        sampvoice_omp::Handshake handshake {};
        const auto byteCount = static_cast<std::size_t>((stream.GetNumberOfBitsUsed() + 7) / 8);
        const auto* data = stream.GetData();
        // Per-packet callbacks keep the packet ID in the backing buffer even
        // though their read cursor starts after it. The custom payload follows
        // the SampVoice control packet ID (222).
        if (data != nullptr && byteCount > 1 && data[0] == sampvoice_omp::ControlPacket &&
            isManikularSnapshot(data + 1, byteCount - 1))
        {
            handleManikularSnapshot(peer, data + 1, byteCount - 1);
            // Consume the custom raw packet before LegacyNetwork handles ID 222.
            return false;
        }
        if (sampvoice_omp::parseHandshake(data, byteCount, handshake))
        {
            Session& session = sessions[peer.getID()];
            session.handshake = handshake;
            initializePlayer(peer, session);
        }
        return true;
    }

    void free() override
    {
        delete this;
    }

    void reset() override
    {
        while (!phoneStreamByPlayer.empty())
        {
            stopPhoneCall(phoneStreamByPlayer.begin()->first);
        }
        sessions.clear();
        phoneCalls.clear();
        phoneStreamByPlayer.clear();
    }

    ~SampVoiceOMPControl() override
    {
        if (pawnComponent != nullptr && pawnHandlerRegistered)
        {
            pawnComponent->getEventDispatcher().removeEventHandler(this);
        }
        if (core)
        {
            if (networkHandlersRegistered)
            {
                core->removePerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
                core->removeNetworkEventHandler(this);
            }
            core->getEventDispatcher().removeEventHandler(this);
        }
        instance = nullptr;
    }

private:
    static std::string quoteJson(const std::string& value)
    {
        static constexpr char Hex[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');
        for (const unsigned char character : value)
        {
            switch (character)
            {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (character < 0x20)
                    {
                        result += "\\u00";
                        result.push_back(Hex[character >> 4]);
                        result.push_back(Hex[character & 0x0F]);
                    }
                    else result.push_back(static_cast<char>(character));
                    break;
            }
        }
        result.push_back('"');
        return result;
    }

    static bool readText(const std::uint8_t* data, std::size_t size, std::size_t& offset,
        std::size_t maximum, std::string& value)
    {
        if (offset >= size) return false;
        const std::size_t length = data[offset++];
        if (length > maximum || length > size - offset) return false;
        value.assign(reinterpret_cast<const char*>(data + offset), length);
        offset += length;
        return true;
    }

    void emitManikularSnapshot(const int playerId, const std::string& json)
    {
        if (pawnComponent == nullptr || pawnComponent->mainScript() == nullptr) return;
        pawnComponent->mainScript()->Call("MV_OnVoiceSettings", DefaultReturnValue_True,
            playerId, StringView(json.data(), json.size()));
    }

    bool isManikularSnapshot(const std::uint8_t* data, const std::size_t size) const
    {
        return data != nullptr && size >= ManikularMagic.size() + 1 &&
            std::equal(ManikularMagic.begin(), ManikularMagic.end(), data) &&
            data[ManikularMagic.size()] == ManikularSnapshot;
    }

    void handleManikularSnapshot(IPlayer& peer, const std::uint8_t* data, const std::size_t size)
    {
        if (data == nullptr || size < ManikularMagic.size() + 1) return;
        for (std::size_t start = 0; start + ManikularMagic.size() + 1 <= size; ++start)
        {
            if (!std::equal(ManikularMagic.begin(), ManikularMagic.end(), data + start) ||
                data[start + ManikularMagic.size()] != ManikularSnapshot)
                continue;

            std::size_t offset = start + ManikularMagic.size() + 1;
            if (size - offset < 5) return;
            const std::uint8_t flags = data[offset++];
            const std::uint8_t microVolume = data[offset++];
            const std::uint8_t soundVolume = data[offset++];
            const std::size_t deviceCount = data[offset++];
            const std::uint8_t selectedDevice = data[offset++];
            if (microVolume > 100 || soundVolume > 100 || deviceCount > 16) return;

            std::vector<std::string> devices;
            devices.reserve(deviceCount);
            for (std::size_t i = 0; i != deviceCount; ++i)
            {
                std::string name;
                if (!readText(data, size, offset, 120, name)) return;
                devices.push_back(std::move(name));
            }

            if (offset >= size) return;
            const std::size_t blacklistCount = data[offset++];
            if (blacklistCount > 32) return;
            std::vector<std::string> blacklist;
            blacklist.reserve(blacklistCount);
            for (std::size_t i = 0; i != blacklistCount; ++i)
            {
                std::string name;
                if (!readText(data, size, offset, 31, name)) return;
                blacklist.push_back(std::move(name));
            }

            std::string json = "{\"ready\":true,\"microEnabled\":";
            json += (flags & 1) ? "true" : "false";
            json += ",\"soundEnabled\":";
            json += (flags & 2) ? "true" : "false";
            json += ",\"balancer\":";
            json += (flags & 4) ? "true" : "false";
            json += ",\"filter\":";
            json += (flags & 8) ? "true" : "false";
            json += ",\"checking\":";
            json += (flags & 16) ? "true" : "false";
            json += ",\"microVolume\":" + std::to_string(microVolume);
            json += ",\"soundVolume\":" + std::to_string(soundVolume);
            json += ",\"selectedDevice\":" + std::to_string(selectedDevice);
            json += ",\"devices\":[";
            for (std::size_t i = 0; i != devices.size(); ++i)
            {
                if (i != 0) json.push_back(',');
                json += quoteJson(devices[i]);
            }
            json += "],\"blacklist\":[";
            for (std::size_t i = 0; i != blacklist.size(); ++i)
            {
                if (i != 0) json.push_back(',');
                json += quoteJson(blacklist[i]);
            }
            json += "]}";
            emitManikularSnapshot(peer.getID(), json);
            return;
        }
    }

    bool sendManikularRequest(const int playerId)
    {
        IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId);
        if (player == nullptr || sessions.find(playerId) == sessions.end()) return false;
        auto packet = sampvoice_omp::makeManikularSettingsRequest();
        player->sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
        return true;
    }

    bool sendManikularOption(const int playerId, const std::uint8_t action, const std::uint8_t value)
    {
        if (action < ManikularSetMicroEnable || action > ManikularResetAudio ||
            ((action == ManikularSetMicroVolume || action == ManikularSetSoundVolume) && value > 100))
            return false;
        IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId);
        if (player == nullptr || sessions.find(playerId) == sessions.end()) return false;
        auto packet = sampvoice_omp::makeManikularSettingsCommand(action, value);
        player->sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
        return true;
    }

    bool sendManikularBlacklist(const int playerId, const std::string& name, const bool add)
    {
        if (name.size() < 3 || name.size() > 24) return false;
        for (const unsigned char character : name)
        {
            if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') || character == '_' || character == '[' || character == ']'))
                return false;
        }
        IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId);
        if (player == nullptr || sessions.find(playerId) == sessions.end()) return false;
        auto packet = sampvoice_omp::makeManikularSettingsCommand(
            add ? ManikularBlacklistAdd : ManikularBlacklistRemove, 0, name);
        player->sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
        return true;
    }

    static cell NativeRequestVoiceSettings(AMX*, const cell* params)
    {
        if (instance == nullptr || params == nullptr || params[0] != sizeof(cell)) return 0;
        return instance->sendManikularRequest(static_cast<int>(params[1])) ? 1 : 0;
    }

    static cell NativeSetVoiceOption(AMX*, const cell* params)
    {
        if (instance == nullptr || params == nullptr || params[0] != 3 * sizeof(cell)) return 0;
        const cell action = params[2];
        const cell value = params[3];
        if (action < 0 || action > 255 || value < 0 || value > 255) return 0;
        return instance->sendManikularOption(static_cast<int>(params[1]),
            static_cast<std::uint8_t>(action), static_cast<std::uint8_t>(value)) ? 1 : 0;
    }

    static cell NativeSetVoiceBlacklist(AMX* amx, const cell* params)
    {
        if (instance == nullptr || instance->pawnComponent == nullptr || amx == nullptr || params == nullptr ||
            params[0] != 3 * sizeof(cell)) return 0;
        IPawnScript* script = instance->pawnComponent->getScript(amx);
        cell* address = nullptr;
        char name[32] {};
        if (script == nullptr || script->GetAddr(params[2], &address) != AMX_ERR_NONE || address == nullptr ||
            script->GetString(name, address, false, sizeof(name)) != AMX_ERR_NONE) return 0;
        return instance->sendManikularBlacklist(static_cast<int>(params[1]), name, params[3] != 0) ? 1 : 0;
    }

    static cell NativeIsVoiceReady(AMX*, const cell* params)
    {
        if (instance == nullptr || params == nullptr || params[0] != sizeof(cell)) return 0;
        return instance->isVoiceReady(static_cast<int>(params[1])) ? 1 : 0;
    }

    static cell NativeStartPhoneCall(AMX*, const cell* params)
    {
        if (instance == nullptr || params == nullptr || params[0] != 2 * sizeof(cell)) return 0;
        return instance->startPhoneCall(static_cast<int>(params[1]), static_cast<int>(params[2])) ? 1 : 0;
    }

    static cell NativeStopPhoneCall(AMX*, const cell* params)
    {
        if (instance == nullptr || params == nullptr || params[0] != sizeof(cell)) return 0;
        return instance->stopPhoneCall(static_cast<int>(params[1])) ? 1 : 0;
    }

    std::uint32_t nextVoiceKey()
    {
        // Avoid constructing std::random_device while the component is loaded:
        // it has caused loader crashes on some i386 libstdc++ builds. This key
        // is only a session nonce, not an authentication secret.
        keyState ^= keyState << 13;
        keyState ^= keyState >> 17;
        keyState ^= keyState << 5;
        return keyState == 0 ? ++keyState : keyState;
    }

    void initializePlayer(IPlayer& peer, Session& session)
    {
        if (!relay.isConnected() || session.initializedGeneration == relay.currentGeneration())
        {
            return;
        }

        session.voiceKey = nextVoiceKey();
        session.initializedGeneration = relay.currentGeneration();
        session.listeners.clear();

        std::vector<std::uint8_t> command(7);
        command[0] = CommandPlayerCreate;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(peer.getID()));
        sampvoice_omp::writeUInt32BE(command.data() + 3, session.voiceKey);
        relay.enqueue(std::move(command));
        queueStreamCreate(peer.getID());
        queuePlayerSpeaker(peer.getID(), ProximityChannel);
        queuePlayerStream(CommandPlayerAttachStream, peer.getID(), ProximityChannel, peer.getID());

        auto packet = sampvoice_omp::makeClientInitialize(session.voiceKey, 0, VoicePort,
            static_cast<std::uint16_t>(peer.getID()));
        peer.sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
        auto channels = sampvoice_omp::makeSpeakerActiveChannels(ProximityChannel);
        peer.sendPacket(Span<std::uint8_t>(channels.data(), channels.size() * 8), OrderingChannel_SyncRPC);
        auto key = sampvoice_omp::makeSpeakerSetKey(ProximityChannel | PhoneChannel, VoicePushToTalkKey);
        peer.sendPacket(Span<std::uint8_t>(key.data(), key.size() * 8), OrderingChannel_SyncRPC);
    }

    void synchronizeAll()
    {
        for (auto& entry : sessions)
        {
            if (IPlayer* player = core->getPlayers().get(entry.first))
            {
                entry.second.initializedGeneration = 0;
                initializePlayer(*player, entry.second);
            }
        }
        synchronizePhoneCalls();
    }

    static SpatialCell makeCell(const IPlayer& player)
    {
        const Vector3 position = player.getPosition();
        return {
            static_cast<int>(std::floor(position.x / VoiceDistance)),
            static_cast<int>(std::floor(position.y / VoiceDistance)),
            static_cast<int>(std::floor(position.z / VoiceDistance)),
            player.getVirtualWorld(),
            player.getInterior()
        };
    }

    void queuePlayerSpeaker(int playerId, std::uint32_t channels)
    {
        std::vector<std::uint8_t> command(7);
        command[0] = CommandPlayerSpeaker;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(playerId));
        sampvoice_omp::writeUInt32BE(command.data() + 3, channels);
        relay.enqueue(std::move(command));
    }

    void queuePlayerStream(std::uint8_t commandType, int playerId, std::uint32_t channels, int streamId)
    {
        std::vector<std::uint8_t> command(9);
        command[0] = commandType;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(playerId));
        sampvoice_omp::writeUInt32BE(command.data() + 3, channels);
        sampvoice_omp::writeUInt16BE(command.data() + 7, static_cast<std::uint16_t>(streamId));
        relay.enqueue(std::move(command));
    }

    void queueStreamCreate(int streamId)
    {
        std::vector<std::uint8_t> create(3);
        create[0] = CommandStreamCreate;
        sampvoice_omp::writeUInt16BE(create.data() + 1, static_cast<std::uint16_t>(streamId));
        relay.enqueue(std::move(create));

        std::vector<std::uint8_t> transiter(4);
        transiter[0] = CommandStreamTransiter;
        sampvoice_omp::writeUInt16BE(transiter.data() + 1, static_cast<std::uint16_t>(streamId));
        transiter[3] = 1;
        relay.enqueue(std::move(transiter));
    }

    void queueStreamListener(std::uint8_t commandType, int streamId, int playerId)
    {
        if (!relay.isConnected())
        {
            return;
        }
        std::vector<std::uint8_t> command(5);
        command[0] = commandType;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(streamId));
        sampvoice_omp::writeUInt16BE(command.data() + 3, static_cast<std::uint16_t>(playerId));
        relay.enqueue(std::move(command));
    }

    void queueStreamDelete(int streamId)
    {
        std::vector<std::uint8_t> command(3);
        command[0] = CommandStreamDelete;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(streamId));
        relay.enqueue(std::move(command));
    }

    template <std::size_t Size>
    static void sendControlPacket(IPlayer& player, std::array<std::uint8_t, Size>& packet)
    {
        player.sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
    }

    bool isVoiceReady(int playerId) const
    {
        const auto session = sessions.find(playerId);
        return relay.isConnected() && session != sessions.end() &&
            session->second.initializedGeneration == relay.currentGeneration();
    }

    void setPlayerChannel(int playerId, std::uint32_t channels)
    {
        if (relay.isConnected())
        {
            queuePlayerSpeaker(playerId, channels);
        }
        if (IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId))
        {
            auto packet = sampvoice_omp::makeSpeakerActiveChannels(channels);
            sendControlPacket(*player, packet);
        }
    }

    std::uint16_t allocatePhoneStream() const
    {
        for (std::uint16_t stream = PhoneStreamFirst; stream < PhoneStreamLimit; ++stream)
        {
            if (phoneCalls.find(stream) == phoneCalls.end()) return stream;
        }
        return 0;
    }

    void sendPhoneStreamCreate(int playerId, std::uint16_t streamId)
    {
        if (IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId))
        {
            auto packet = sampvoice_omp::makeStreamCreate(streamId, 0.0f);
            sendControlPacket(*player, packet);
        }
    }

    void sendPhoneStreamDelete(int playerId, std::uint16_t streamId)
    {
        if (IPlayer* player = core == nullptr ? nullptr : core->getPlayers().get(playerId))
        {
            auto packet = sampvoice_omp::makeStreamDelete(streamId);
            sendControlPacket(*player, packet);
        }
    }

    void synchronizePhoneCall(const PhoneCall& call)
    {
        if (!isVoiceReady(call.firstPlayer) || !isVoiceReady(call.secondPlayer)) return;

        queueStreamCreate(call.stream);
        for (const int playerId : { call.firstPlayer, call.secondPlayer })
        {
            queuePlayerStream(CommandPlayerAttachStream, playerId, PhoneChannel, call.stream);
            queueStreamListener(CommandStreamAttachListener, call.stream, playerId);
            sendPhoneStreamCreate(playerId, call.stream);
            setPlayerChannel(playerId, PhoneChannel);
        }
    }

    void synchronizePhoneCalls()
    {
        for (const auto& entry : phoneCalls)
        {
            synchronizePhoneCall(entry.second);
        }
    }

    bool startPhoneCall(int firstPlayer, int secondPlayer)
    {
        if (firstPlayer == secondPlayer || !isVoiceReady(firstPlayer) || !isVoiceReady(secondPlayer) ||
            phoneStreamByPlayer.find(firstPlayer) != phoneStreamByPlayer.end() ||
            phoneStreamByPlayer.find(secondPlayer) != phoneStreamByPlayer.end())
        {
            return false;
        }

        const std::uint16_t stream = allocatePhoneStream();
        if (stream == 0) return false;

        const PhoneCall call { stream, firstPlayer, secondPlayer };
        phoneCalls.emplace(stream, call);
        phoneStreamByPlayer.emplace(firstPlayer, stream);
        phoneStreamByPlayer.emplace(secondPlayer, stream);
        synchronizePhoneCall(call);
        return true;
    }

    bool stopPhoneCall(int playerId)
    {
        const auto playerCall = phoneStreamByPlayer.find(playerId);
        if (playerCall == phoneStreamByPlayer.end()) return false;

        const std::uint16_t stream = playerCall->second;
        const auto callEntry = phoneCalls.find(stream);
        if (callEntry == phoneCalls.end())
        {
            phoneStreamByPlayer.erase(playerCall);
            return false;
        }

        const PhoneCall call = callEntry->second;
        for (const int participantId : { call.firstPlayer, call.secondPlayer })
        {
            if (relay.isConnected())
            {
                queuePlayerStream(CommandPlayerDetachStream, participantId, PhoneChannel, stream);
                queueStreamListener(CommandStreamDetachListener, stream, participantId);
            }
            sendPhoneStreamDelete(participantId, stream);
            if (isVoiceReady(participantId)) setPlayerChannel(participantId, ProximityChannel);
            phoneStreamByPlayer.erase(participantId);
        }
        if (relay.isConnected()) queueStreamDelete(stream);
        phoneCalls.erase(callEntry);
        return true;
    }

    void attachListener(int sourceId, int listenerId)
    {
        auto source = sessions.find(sourceId);
        if (source == sessions.end() || !source->second.listeners.insert(listenerId).second)
        {
            return;
        }

        queueStreamListener(CommandStreamAttachListener, sourceId, listenerId);
        if (IPlayer* listener = core->getPlayers().get(listenerId))
        {
            auto create = sampvoice_omp::makeStreamCreate(static_cast<std::uint16_t>(sourceId), VoiceDistance);
            sendControlPacket(*listener, create);
            auto target = sampvoice_omp::makeStreamSetTarget(static_cast<std::uint16_t>(sourceId),
                static_cast<std::uint16_t>((2 << 14) | (sourceId & 0x3FFF)));
            sendControlPacket(*listener, target);
        }
    }

    void detachListener(int sourceId, int listenerId, bool notifyClient)
    {
        queueStreamListener(CommandStreamDetachListener, sourceId, listenerId);
        if (notifyClient)
        {
            if (IPlayer* listener = core->getPlayers().get(listenerId))
            {
                auto deleted = sampvoice_omp::makeStreamDelete(static_cast<std::uint16_t>(sourceId));
                sendControlPacket(*listener, deleted);
            }
        }
    }

    void updateProximity()
    {
        if (!relay.isConnected())
        {
            return;
        }

        std::unordered_map<int, VoicePlayer> players;
        std::unordered_map<SpatialCell, std::vector<int>, SpatialCellHash> grid;
        for (auto& entry : sessions)
        {
            if (entry.second.initializedGeneration != relay.currentGeneration())
            {
                continue;
            }
            if (IPlayer* player = core->getPlayers().get(entry.first))
            {
                VoicePlayer voicePlayer { player, player->getPosition(), makeCell(*player) };
                grid[voicePlayer.cell].push_back(entry.first);
                players.emplace(entry.first, voicePlayer);
            }
        }

        for (auto& sourceEntry : sessions)
        {
            const int sourceId = sourceEntry.first;
            auto sourcePlayer = players.find(sourceId);
            if (sourcePlayer == players.end())
            {
                continue;
            }

            std::unordered_set<int> desired;
            const SpatialCell& sourceCell = sourcePlayer->second.cell;
            for (int x = sourceCell.x - 1; x <= sourceCell.x + 1; ++x)
            {
                for (int y = sourceCell.y - 1; y <= sourceCell.y + 1; ++y)
                {
                    for (int z = sourceCell.z - 1; z <= sourceCell.z + 1; ++z)
                    {
                        const SpatialCell cell { x, y, z, sourceCell.world, sourceCell.interior };
                        const auto bucket = grid.find(cell);
                        if (bucket == grid.end())
                        {
                            continue;
                        }
                        for (const int listenerId : bucket->second)
                        {
                            if (listenerId == sourceId)
                            {
                                continue;
                            }
                            const Vector3 delta = players.at(listenerId).position - sourcePlayer->second.position;
                            if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <= VoiceDistanceSquared)
                            {
                                desired.insert(listenerId);
                            }
                        }
                    }
                }
            }

            for (auto listener = sourceEntry.second.listeners.begin(); listener != sourceEntry.second.listeners.end();)
            {
                if (desired.find(*listener) == desired.end())
                {
                    detachListener(sourceId, *listener, true);
                    listener = sourceEntry.second.listeners.erase(listener);
                }
                else
                {
                    ++listener;
                }
            }
            for (const int listenerId : desired)
            {
                attachListener(sourceId, listenerId);
            }
        }
    }

    ICore* core = nullptr;
    CommandRelay relay;
    std::uint32_t keyState = 0x9E3779B9;
    std::unordered_map<int, Session> sessions;
    std::unordered_map<std::uint16_t, PhoneCall> phoneCalls;
    std::unordered_map<int, std::uint16_t> phoneStreamByPlayer;
    TimePoint nextProximityUpdate {};
    bool networkHandlersRegistered = false;
    IPawnComponent* pawnComponent = nullptr;
    bool pawnHandlerRegistered = false;
    static SampVoiceOMPControl* instance;
};

SampVoiceOMPControl* SampVoiceOMPControl::instance = nullptr;
} // namespace

COMPONENT_ENTRY_POINT()
{
    return new SampVoiceOMPControl();
}
