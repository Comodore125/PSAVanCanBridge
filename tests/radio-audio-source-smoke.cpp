// Run: g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/radio-audio-source-smoke.cpp -o /tmp/radio-source-test && /tmp/radio-source-test
// Pure host test: no ESP32, CAN interface or Spotify account required.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "Helpers/RadioAudioSource.hpp"

int main() {
    constexpr uint64_t start = 25000;
    // CAN 0x165 byte 0 = radio enabled, byte 2 high middle bits = source.
    const uint8_t tuner[4] = {0x80,0x00,0x10,0x00};
    const uint8_t cd[4] = {0x80,0x00,0x20,0x00};
    const uint8_t changer[4] = {0x80,0x00,0x30,0x00};
    const uint8_t aux1[4] = {0x80,0x00,0x40,0x00};
    const uint8_t aux2[4] = {0x80,0x00,0x50,0x00};
    const uint8_t usb[4] = {0x80,0x00,0x60,0x00};
    const uint8_t bt[4] = {0x80,0x00,0x70,0x00};
    const uint8_t off[4] = {0x00,0x00,0x40,0x00};
    const uint8_t* const frames[] = {tuner, cd, changer, aux1, aux2, usb, bt, off};
    for (unsigned i=0; i<8; ++i) {
        const uint32_t state = RadioAudioSource::EncodeAee2004(frames[i], start);
        assert(RadioAudioSource::FreshSource(state,start) == (i<7?i+1:0));
        assert(RadioAudioSource::IsAux(state,start) == (i==3||i==4));
    }
    const uint32_t aux = RadioAudioSource::EncodeAee2004(aux1,start);
    assert(RadioAudioSource::IsAux(aux,start+1500));
    assert(!RadioAudioSource::IsAux(aux,start+1600)); // Fail closed if 0x165 disappears.
    assert(!RadioAudioSource::IsAux(0,start));
    // 29-bit 100 ms timestamp wraps but freshness must still work.
    const uint64_t beforeWrap = uint64_t(RadioAudioSource::StampMask-1U)*100ULL;
    const uint32_t wrapping = RadioAudioSource::EncodeAee2004(aux2,beforeWrap);
    assert(RadioAudioSource::IsAux(wrapping,beforeWrap+300ULL));
    assert(!RadioAudioSource::IsAux(wrapping,beforeWrap+2000ULL));
    std::puts("PASS: RD45 AUX-only source decode, radio-off, stale frame, timestamp wrap.");
}
