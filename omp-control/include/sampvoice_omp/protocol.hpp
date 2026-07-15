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
#include <cstring>
#include <string>
#include <vector>

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
    ClientInitialize = 0,
    SpeakerActiveChannels = 1,
    SpeakerSetKey = 3,
    StreamCreate = 4,
    StreamSetTarget = 9,
    StreamDelete = 12,

    // MANIKULAR VOICE extensions. The legacy SampVoice control protocol uses
    // the low range; these values leave it untouched for compatibility.
    ManikularSettingsRequest = 128,
    ManikularSettingsCommand = 129
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

inline void writeUInt16BE(std::uint8_t* output, std::uint16_t value)
{
    output[0] = static_cast<std::uint8_t>(value >> 8);
    output[1] = static_cast<std::uint8_t>(value);
}

inline void writeUInt32BE(std::uint8_t* output, std::uint32_t value)
{
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

inline void writeFloatBE(std::uint8_t* output, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeUInt32BE(output, bits);
}

inline std::array<std::uint8_t, 2 + sizeof(ClientInitialize)> makeClientInitialize(
    std::uint32_t voiceKey, std::uint32_t voiceHost, std::uint16_t voicePort, std::uint16_t voiceId)
{
    std::array<std::uint8_t, 2 + sizeof(ClientInitialize)> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::ClientInitialize);
    writeUInt32BE(result.data() + 2, voiceKey);
    writeUInt32BE(result.data() + 6, voiceHost);
    writeUInt16BE(result.data() + 10, voicePort);
    writeUInt16BE(result.data() + 12, voiceId);
    return result;
}

inline std::array<std::uint8_t, 6> makeSpeakerActiveChannels(std::uint32_t channels)
{
    std::array<std::uint8_t, 6> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::SpeakerActiveChannels);
    writeUInt32BE(result.data() + 2, channels);
    return result;
}

inline std::array<std::uint8_t, 7> makeSpeakerSetKey(std::uint32_t channels, std::uint8_t key)
{
    std::array<std::uint8_t, 7> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::SpeakerSetKey);
    writeUInt32BE(result.data() + 2, channels);
    result[6] = key;
    return result;
}

inline std::array<std::uint8_t, 8> makeStreamCreate(std::uint16_t stream, float distance)
{
    std::array<std::uint8_t, 8> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::StreamCreate);
    writeUInt16BE(result.data() + 2, stream);
    writeFloatBE(result.data() + 4, distance);
    return result;
}

inline std::array<std::uint8_t, 6> makeStreamSetTarget(std::uint16_t stream, std::uint16_t target)
{
    std::array<std::uint8_t, 6> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::StreamSetTarget);
    writeUInt16BE(result.data() + 2, stream);
    writeUInt16BE(result.data() + 4, target);
    return result;
}

inline std::array<std::uint8_t, 4> makeStreamDelete(std::uint16_t stream)
{
    std::array<std::uint8_t, 4> result {};
    result[0] = ControlPacket;
    result[1] = static_cast<std::uint8_t>(ControlPacketType::StreamDelete);
    writeUInt16BE(result.data() + 2, stream);
    return result;
}

inline std::array<std::uint8_t, 2> makeManikularSettingsRequest()
{
    return { ControlPacket, static_cast<std::uint8_t>(ControlPacketType::ManikularSettingsRequest) };
}

inline std::vector<std::uint8_t> makeManikularSettingsCommand(std::uint8_t action,
    std::uint8_t value = 0, const std::string& name = {})
{
    std::vector<std::uint8_t> result;
    result.reserve(4 + name.size());
    result.push_back(ControlPacket);
    result.push_back(static_cast<std::uint8_t>(ControlPacketType::ManikularSettingsCommand));
    result.push_back(action);
    if (name.empty())
    {
        result.push_back(value);
    }
    else
    {
        result.push_back(static_cast<std::uint8_t>(name.size()));
        result.insert(result.end(), name.begin(), name.end());
    }
    return result;
}
} // namespace sampvoice_omp
