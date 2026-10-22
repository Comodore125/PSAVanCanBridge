#pragma once
// Passive COM2000 SEEK gestures. No allocations, delays, CAN writes or Spotify
// calls in the AEE2001 receive path. One producer (VAN) / one consumer (task).
#include <cstdint>

namespace SpotifySeekGesture {
    constexpr uint32_t LongPressMs = 700U;
    constexpr uint32_t MaxGestureMs = 15000U;
    constexpr uint32_t MaxEventAgeMs = 1500U;
    constexpr uint32_t MsPerHeldMs = 10U; // 2 s held -> 20 s seek
    constexpr uint32_t MaxSeekDeltaMs = 120000U;

    struct Event {
        uint32_t releasedAtMs = 0;
        uint16_t heldMs = 0;
        bool forward = false;
    };

    struct Queue {
        static constexpr uint32_t Capacity = 16U;
        Event items[Capacity]{};
        uint32_t writeIndex = 0;
        uint32_t readIndex = 0;

        bool Push(const Event& event) {
            const uint32_t write = __atomic_load_n(&writeIndex, __ATOMIC_RELAXED);
            const uint32_t read = __atomic_load_n(&readIndex, __ATOMIC_ACQUIRE);
            if (write - read >= Capacity) return false; // fail closed on overflow
            items[write % Capacity] = event;
            __atomic_store_n(&writeIndex, write + 1U, __ATOMIC_RELEASE);
            return true;
        }

        bool Pop(Event& event) {
            const uint32_t read = __atomic_load_n(&readIndex, __ATOMIC_RELAXED);
            const uint32_t write = __atomic_load_n(&writeIndex, __ATOMIC_ACQUIRE);
            if (read == write) return false;
            event = items[read % Capacity];
            __atomic_store_n(&readIndex, read + 1U, __ATOMIC_RELEASE);
            return true;
        }
    };

    struct Tracker {
        bool up = false;
        bool down = false;
        uint32_t upAtMs = 0;
        uint32_t downAtMs = 0;

        void Observe(bool newUp, bool newDown, uint32_t nowMs, Queue& queue) {
            if (newUp && !up) upAtMs = nowMs;
            if (newDown && !down) downAtMs = nowMs;
            if (!newUp && up) Release(true, upAtMs, nowMs, queue);
            if (!newDown && down) Release(false, downAtMs, nowMs, queue);
            up = newUp;
            down = newDown;
        }

    private:
        static void Release(bool forward, uint32_t startedMs,
                            uint32_t nowMs, Queue& queue) {
            const uint32_t held = nowMs - startedMs; // rollover-safe
            if (held > MaxGestureMs) return;
            queue.Push({nowMs, static_cast<uint16_t>(held), forward});
        }
    };

    inline bool IsLong(const Event& event) {
        return event.heldMs >= LongPressMs;
    }

    inline uint32_t SeekDeltaMs(const Event& event) {
        const uint32_t delta = uint32_t(event.heldMs) * MsPerHeldMs;
        return delta < MaxSeekDeltaMs ? delta : MaxSeekDeltaMs;
    }

    inline bool IsFresh(const Event& event, uint32_t nowMs) {
        return nowMs - event.releasedAtMs <= MaxEventAgeMs;
    }
}
