/*
 * SampVoice open.mp control component.
 *
 * This file contains the wire-format pieces shared by the v4 client and the
 * control component.  They deliberately do not depend on SA:MP internals.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sampvoice_omp
{
constexpr std::array<std::uint8_t, 4> ConnectSignature { 0xDE, 0xAD, 0xC0, 0xDE };
constexpr std::uint8_t ConnectRPC = 25;
constexpr std::uint8_t ControlPacket = 222;

#pragma pack(push, 1)
struct ConnectData
{
    std::uint32_t version;
    std::uint8_t hasMicrophone;
};
#pragma pack(pop)

enum class ControlPacketType : std::uint8_t
{
    ClientInitialize = 0
};

#pragma pack(push, 1)
struct ClientInitialize
{
    std::uint32_t voiceKey;
    std::uint32_t voiceHost;
    std::uint16_t voicePort;
    std::uint16_t voiceId;
};
#pragma pack(pop)

static_assert(sizeof(ConnectData) == 5, "Unexpected SampVoice v4 handshake layout.");
static_assert(sizeof(ClientInitialize) == 12, "Unexpected SampVoice v4 ClientInitialize layout.");

struct Handshake
{
    std::uint32_t version;
    bool hasMicrophone;
};

inline bool parseHandshake(const std::uint8_t* data, std::size_t size, Handshake& result)
{
    constexpr std::size_t dataSize = sizeof(std::uint32_t) + sizeof(std::uint8_t);
    if (data == nullptr || size < ConnectSignature.size() + 1 + dataSize)
    {
        return false;
    }

    for (std::size_t signatureOffset = size - (ConnectSignature.size() + 1 + dataSize) + 1; signatureOffset-- > 0;)
    {
        bool signatureMatches = true;
        for (std::size_t i = 0; i != ConnectSignature.size(); ++i)
        {
            if (data[signatureOffset + i] != ConnectSignature[i])
            {
                signatureMatches = false;
                break;
            }
        }

        if (!signatureMatches)
        {
            continue;
        }

        const std::size_t lengthOffset = signatureOffset + ConnectSignature.size();
        const std::size_t payloadSize = data[lengthOffset];
        const std::size_t payloadOffset = lengthOffset + 1;
        if (payloadSize != dataSize || payloadOffset + payloadSize > size)
        {
            continue;
        }

        result.version = (std::uint32_t(data[payloadOffset]) << 24) |
            (std::uint32_t(data[payloadOffset + 1]) << 16) |
            (std::uint32_t(data[payloadOffset + 2]) << 8) |
            std::uint32_t(data[payloadOffset + 3]);
        result.hasMicrophone = data[payloadOffset + 4] != 0;
        return true;
    }

    return false;
}
} // namespace sampvoice_omp
