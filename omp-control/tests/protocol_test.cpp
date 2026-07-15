#include "sampvoice_omp/protocol.hpp"

#include <array>
#include <cassert>

int main()
{
    const std::array<std::uint8_t, 13> packet {
        0x00, 0xDE, 0xAD, 0xC0, 0xDE, 0x05, 0x04, 0x00, 0x00, 0x01, 0x01, 0xFF, 0xFF
    };

    sampvoice_omp::Handshake handshake {};
    assert(sampvoice_omp::parseHandshake(packet.data(), packet.size(), handshake));
    assert(handshake.version == 0x04000001);
    assert(handshake.hasMicrophone);

    const std::array<std::uint8_t, 9> malformed {
        0xDE, 0xAD, 0xC0, 0xDE, 0x04, 0x04, 0x00, 0x00, 0x01
    };
    assert(!sampvoice_omp::parseHandshake(malformed.data(), malformed.size(), handshake));

    const auto initialize = sampvoice_omp::makeClientInitialize(0x11223344, 0, 2020, 17);
    assert(initialize[0] == sampvoice_omp::ControlPacket);
    assert(initialize[1] == 0);
    assert(initialize[2] == 0x11 && initialize[3] == 0x22 && initialize[4] == 0x33 && initialize[5] == 0x44);
    assert(initialize[10] == 0x07 && initialize[11] == 0xE4);
    assert(initialize[12] == 0 && initialize[13] == 17);

    const auto channels = sampvoice_omp::makeSpeakerActiveChannels(1);
    assert(channels[0] == sampvoice_omp::ControlPacket && channels[1] == 1);
    assert(channels[2] == 0 && channels[3] == 0 && channels[4] == 0 && channels[5] == 1);

    const auto target = sampvoice_omp::makeStreamSetTarget(15, (2 << 14) | 17);
    assert(target[1] == 9 && target[2] == 0 && target[3] == 15);
    assert(target[4] == 0x80 && target[5] == 17);
}
