#pragma once
// Experimental RD45 USB-like text 0x63 over CAN 0x2E3 (ISO-TP).
// Pure, bounded, allocation-free framing; not coupled to CAN scheduling.
#include <cstdint>
#include <cstddef>

namespace OemText63 {
    constexpr size_t Capacity = 80;
    struct Payload { uint8_t data[Capacity]{}; uint8_t size = 0; };

    inline void AppendAscii(Payload& out, const char* text, size_t limit) {
        if (!text) return;
        size_t used = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
             *p && used < limit && out.size < Capacity-1; ++p) {
            // OEM display character set for UTF-8 is not yet verified.
            // Replace non-ASCII bytes instead of emitting invalid glyph sequences.
            if (*p >= 0x80) {
                if ((*p & 0xC0) != 0x80) { out.data[out.size++] = '?'; ++used; }
                continue;
            }
            out.data[out.size++] = *p < 0x20 ? ' ' : *p;
            ++used;
        }
    }
    inline bool Encode(const char* artist, const char* title, Payload& out) {
        out = Payload{};
        if (!artist || !title || !*artist || !*title) return false;
        out.data[out.size++] = 0x63;
        AppendAscii(out, artist, 31);
        out.data[out.size++] = 0;
        AppendAscii(out, title, 44);
        out.data[out.size++] = 0;
        return out.size >= 5;
    }

    struct Frame { uint8_t data[8]{}; uint8_t length = 0; };
    inline Frame SingleFrame(const Payload& payload) {
        Frame frame{};
        if (payload.size > 7) return frame;
        frame.data[0] = payload.size;
        for (uint8_t i=0; i<payload.size; ++i) frame.data[i+1] = payload.data[i];
        frame.length = payload.size + 1;
        return frame;
    }
    inline Frame FirstFrame(const Payload& payload) {
        Frame frame{};
        frame.data[0] = static_cast<uint8_t>(0x10 | (payload.size >> 8));
        frame.data[1] = payload.size;
        for (uint8_t i=0; i<6 && i<payload.size; ++i) frame.data[i+2] = payload.data[i];
        frame.length = payload.size > 6 ? 8 : static_cast<uint8_t>(payload.size+2);
        return frame;
    }
    inline Frame ConsecutiveFrame(const Payload& payload, size_t offset, uint8_t sequence) {
        Frame frame{};
        if (offset >= payload.size) return frame;
        frame.data[0] = static_cast<uint8_t>(0x20 | (sequence & 0x0F));
        frame.length = 1;
        for (size_t i=offset; i<payload.size && frame.length<8; ++i)
            frame.data[frame.length++] = payload.data[i];
        return frame;
    }
}
