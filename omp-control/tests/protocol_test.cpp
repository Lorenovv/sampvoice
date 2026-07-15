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
}
