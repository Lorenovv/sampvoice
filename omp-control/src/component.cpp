/*
 * The legacy SampVoice control plug-in hooks RakNet and dereferences CNetGame.
 * This component uses only the public open.mp SDK instead.
 */

#include <bitstream.hpp>
#include <sdk.hpp>

#include <cstdint>
#include <unordered_map>

#include "sampvoice_omp/protocol.hpp"

namespace
{
class SampVoiceOMPControl final : public IComponent, public SingleNetworkInEventHandler, public NetworkEventHandler
{
public:
    StringView componentName() const override
    {
        return "SampVoiceOMPControl";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(0, 1, 0, 0);
    }

    UID getUID() override
    {
        return UID(0x823e6cc27f7b9a31);
    }

    void onLoad(ICore* loadedCore) override
    {
        core = loadedCore;
        core->addPerRPCInEventHandler<sampvoice_omp::ConnectRPC>(this);
        core->addNetworkEventHandler(this);
    }

    void onPeerDisconnect(IPlayer& peer, PeerDisconnectReason) override
    {
        sessions.erase(peer.getID());
    }

    bool onReceive(IPlayer& peer, NetworkBitStream& stream) override
    {
        sampvoice_omp::Handshake handshake {};
        const auto byteCount = static_cast<std::size_t>((stream.GetNumberOfBitsUsed() + 7) / 8);
        if (!sampvoice_omp::parseHandshake(stream.GetData(), byteCount, handshake))
        {
            return true;
        }

        sessions[peer.getID()] = handshake;

        // The next commit adds the relay command channel and sends the v4
        // ClientInitialize control packet.  At this point we only observe the
        // handshake through supported SDK APIs, without consuming RPC 25.
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
        }
    }

private:
    ICore* core = nullptr;
    std::unordered_map<int, sampvoice_omp::Handshake> sessions;
};
} // namespace

COMPONENT_ENTRY_POINT()
{
    return new SampVoiceOMPControl();
}
