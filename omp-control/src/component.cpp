/*
 * The legacy SampVoice control plug-in hooks RakNet and dereferences CNetGame.
 * This component uses only the public open.mp SDK instead.
 */

#include <bitstream.hpp>
#include <sdk.hpp>

#include <cerrno>
#include <cstdint>
#include <random>
#include <unordered_map>
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
constexpr std::uint8_t CommandPlayerCreate = 1;
constexpr std::uint8_t CommandPlayerDelete = 6;

class CommandRelay final
{
public:
    ~CommandRelay()
    {
        closeConnection();
        if (listener != -1)
        {
            close(listener);
        }
    }

    bool start()
    {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == -1)
        {
            return false;
        }

        const int enabled = 1;
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
            fcntl(listener, F_SETFL, O_NONBLOCK) == -1)
        {
            close(listener);
            listener = -1;
            return false;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(ControlPort);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 1) != 0)
        {
            close(listener);
            listener = -1;
            return false;
        }

        return true;
    }

    bool tick()
    {
        bool accepted = false;
        if (connection == -1 && listener != -1)
        {
            const int candidate = accept(listener, nullptr, nullptr);
            if (candidate != -1)
            {
                if (fcntl(candidate, F_SETFL, O_NONBLOCK) != -1)
                {
                    connection = candidate;
                    ++generation;
                    accepted = true;
                }
                else
                {
                    close(candidate);
                }
            }
        }

        flush();
        return accepted;
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

    int listener = -1;
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
        core->addPerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
        core->addNetworkEventHandler(this);
        core->getEventDispatcher().addEventHandler(this);
    }

    void onTick(Microseconds, TimePoint) override
    {
        if (relay.tick())
        {
            synchronizeAll();
        }
    }

    void onPeerDisconnect(IPlayer& peer, PeerDisconnectReason) override
    {
        if (sessions.erase(peer.getID()) != 0 && relay.isConnected())
        {
            std::vector<std::uint8_t> command(3);
            command[0] = CommandPlayerDelete;
            sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(peer.getID()));
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
            core->removePerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
            core->removeNetworkEventHandler(this);
            core->getEventDispatcher().removeEventHandler(this);
        }
    }

private:
    std::uint32_t nextVoiceKey()
    {
        std::uint32_t key = 0;
        while (key == 0)
        {
            key = random();
        }
        return key;
    }

    void initializePlayer(IPlayer& peer, Session& session)
    {
        if (!relay.isConnected() || session.initializedGeneration == relay.currentGeneration())
        {
            return;
        }

        session.voiceKey = nextVoiceKey();
        session.initializedGeneration = relay.currentGeneration();

        std::vector<std::uint8_t> command(7);
        command[0] = CommandPlayerCreate;
        sampvoice_omp::writeUInt16BE(command.data() + 1, static_cast<std::uint16_t>(peer.getID()));
        sampvoice_omp::writeUInt32BE(command.data() + 3, session.voiceKey);
        relay.enqueue(std::move(command));

        auto packet = sampvoice_omp::makeClientInitialize(session.voiceKey, 0, VoicePort,
            static_cast<std::uint16_t>(peer.getID()));
        peer.sendPacket(Span<std::uint8_t>(packet.data(), packet.size() * 8), OrderingChannel_SyncRPC);
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

    ICore* core = nullptr;
    CommandRelay relay;
    std::mt19937 random { std::random_device {}() };
    std::unordered_map<int, Session> sessions;
};
} // namespace

COMPONENT_ENTRY_POINT()
{
    return new SampVoiceOMPControl();
}
