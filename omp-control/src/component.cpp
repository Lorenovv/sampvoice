/*
 * The legacy SampVoice control plug-in hooks RakNet and dereferences CNetGame.
 * This component uses only the public open.mp SDK instead.
 */

#include <bitstream.hpp>
#include <sdk.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
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
constexpr std::uint32_t VoiceChannel = 1;
constexpr std::uint8_t CommandPlayerCreate = 1;
constexpr std::uint8_t CommandPlayerSpeaker = 3;
constexpr std::uint8_t CommandPlayerAttachStream = 4;
constexpr std::uint8_t CommandPlayerDelete = 6;
constexpr std::uint8_t CommandStreamCreate = 7;
constexpr std::uint8_t CommandStreamTransiter = 8;
constexpr std::uint8_t CommandStreamAttachListener = 9;
constexpr std::uint8_t CommandStreamDetachListener = 10;

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
    public NetworkEventHandler, public CoreEventHandler
{
public:
    StringView componentName() const override
    {
        return "SampVoiceOMPControl";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(0, 2, 0, 0);
    }

    UID getUID() override
    {
        return UID(0x823e6cc27f7b9a31);
    }

    void onLoad(ICore* loadedCore) override
    {
        core = loadedCore;
        relay.start();
        core->getEventDispatcher().addEventHandler(this);
    }

    void onReady() override
    {
        // LegacyNetwork is loaded after normal components. Registering here
        // ensures RPC 25 is attached to the real game network, not an empty
        // network list during component discovery.
        core->addPerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
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
        if (!sampvoice_omp::parseHandshake(stream.GetData(), byteCount, handshake))
        {
            return true;
        }

        Session& session = sessions[peer.getID()];
        session.handshake = handshake;
        initializePlayer(peer, session);
        return true;
    }

    void free() override
    {
        delete this;
    }

    void reset() override
    {
        sessions.clear();
    }

    ~SampVoiceOMPControl() override
    {
        if (core)
        {
            if (networkHandlersRegistered)
            {
                core->removePerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
                core->removeNetworkEventHandler(this);
            }
            core->getEventDispatcher().removeEventHandler(this);
        }
    }

private:
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
        queuePlayerSpeaker(peer.getID());
        queuePlayerAttachStream(peer.getID());

        auto packet = sampvoice_omp::makeClientInitialize(session.voiceKey, 0, VoicePort,
            static_cast<std::uint16_t>(peer.getID()));
        peer.sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
        auto channels = sampvoice_omp::makeSpeakerActiveChannels(VoiceChannel);
        peer.sendPacket(Span<std::uint8_t>(channels.data(), channels.size() * 8), OrderingChannel_SyncRPC);
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

    void queuePlayerSpeaker(int playerId)
    {
        std::vector<std::uint8_t> command(7);
        command[0] = CommandPlayerSpeaker;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(playerId));
        sampvoice_omp::writeUInt32BE(command.data() + 3, VoiceChannel);
        relay.enqueue(std::move(command));
    }

    void queuePlayerAttachStream(int playerId)
    {
        std::vector<std::uint8_t> command(9);
        command[0] = CommandPlayerAttachStream;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(playerId));
        sampvoice_omp::writeUInt32BE(command.data() + 3, VoiceChannel);
        sampvoice_omp::writeUInt16BE(command.data() + 7, static_cast<std::uint16_t>(playerId));
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

    template <std::size_t Size>
    static void sendControlPacket(IPlayer& player, std::array<std::uint8_t, Size>& packet)
    {
        player.sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
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
    TimePoint nextProximityUpdate {};
    bool networkHandlersRegistered = false;
};
} // namespace

COMPONENT_ENTRY_POINT()
{
    return new SampVoiceOMPControl();
}
