#pragma once
// Read-only RD4/RD45 source state from AEE2004 comfort CAN 0x165.
// No CAN/VAN injection and no modifications to steering-stalk forwarding.
#include <cstdint>

namespace RadioAudioSource {
    // One atomic 32-bit word: 29-bit monotonic 100 ms stamp + 3-bit source.
    // A fresh radio status is required; unknown/off/stale always fails closed.
    constexpr uint32_t StampMask = 0x1FFFFFFFU;
    constexpr uint32_t MaxAgeTicks = 15U; // 1.5 seconds; typical 0x165 period is 100 ms.

    inline uint32_t EncodeAee2004(const uint8_t* data, uint64_t nowMs) {
        // byte 0 bit 7 = radio powered, byte 2 bits 4..6 = selected source.
        const uint8_t source = (data[0] & 0x80U) ? ((data[2] >> 4) & 7U) : 0U;
        return ((static_cast<uint32_t>(nowMs / 100U) & StampMask) << 3) | source;
    }

    inline uint8_t FreshSource(uint32_t encoded, uint64_t nowMs) {
        const uint8_t source = static_cast<uint8_t>(encoded & 7U);
        if (source == 0U) return 0U;
        const uint32_t nowTicks = static_cast<uint32_t>(nowMs / 100U) & StampMask;
        const uint32_t thenTicks = (encoded >> 3) & StampMask;
        const uint32_t age = (nowTicks - thenTicks) & StampMask;
        return age <= MaxAgeTicks ? source : 0U;
    }

    inline bool IsAux(uint32_t encoded, uint64_t nowMs) {
        const uint8_t source = FreshSource(encoded, nowMs);
        return source == 4U || source == 5U; // AUX1 or AUX2 only
    }
}
