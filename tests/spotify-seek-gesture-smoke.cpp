// g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/spotify-seek-gesture-smoke.cpp -o /tmp/spotify-gesture && /tmp/spotify-gesture
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "Helpers/SpotifySeekGesture.hpp"

using namespace SpotifySeekGesture;

int main() {
    Queue queue{};
    Tracker stalk{};
    Event event{};

    stalk.Observe(false, false, 1000, queue);
    stalk.Observe(true, false, 1100, queue);
    stalk.Observe(true, false, 1200, queue); // repeated VAN frame != second press
    assert(!queue.Pop(event)); // defer until release
    stalk.Observe(false, false, 1275, queue);
    assert(queue.Pop(event));
    assert(event.forward && event.heldMs == 175 && !IsLong(event));
    assert(IsFresh(event, 1275));
    assert(!queue.Pop(event));

    stalk.Observe(false, true, 2000, queue);
    stalk.Observe(false, false, 2700, queue); // exact threshold
    assert(queue.Pop(event));
    assert(!event.forward && IsLong(event) && SeekDeltaMs(event) == 7000);

    stalk.Observe(true, false, 5000, queue);
    stalk.Observe(false, false, 7000, queue);
    assert(queue.Pop(event));
    assert(event.forward && SeekDeltaMs(event) == 20000);

    // Multiple quick presses survive a single 200ms consumer interval.
    stalk.Observe(true, false, 8000, queue);
    stalk.Observe(false, false, 8060, queue);
    stalk.Observe(true, false, 8100, queue);
    stalk.Observe(false, false, 8180, queue);
    assert(queue.Pop(event) && event.heldMs == 60);
    assert(queue.Pop(event) && event.heldMs == 80);
    assert(!queue.Pop(event));

    // Queue is bounded; producer never blocks or overwrites unread events.
    for (uint32_t i=0; i<Queue::Capacity; i++)
        assert(queue.Push({9000+i, 50, true}));
    assert(!queue.Push({9050, 50, false}));
    for (uint32_t i=0; i<Queue::Capacity; i++) {
        assert(queue.Pop(event));
        assert(event.releasedAtMs == 9000+i);
    }
    assert(!queue.Pop(event));
    assert(queue.Push({9200, 50, false}));
    assert(queue.Pop(event) && !event.forward);

    // Delay/backlog expires; no replay after hotspot reconnect.
    assert(!IsFresh({1000, 75, true}, 2501));
    assert(IsFresh({1000, 75, true}, 2500));

    // 32-bit millisecond clock wrap is safe.
    Tracker wrapped{};
    const uint32_t nearEnd = 0xFFFFFF00U;
    wrapped.Observe(true, false, nearEnd, queue);
    wrapped.Observe(false, false, 0x000002E8U, queue); // +1000 ms
    assert(queue.Pop(event));
    assert(event.heldMs == 1000 && IsLong(event));

    // Abandoned/stuck >15s input does not become a delayed seek.
    Tracker stuck{};
    stuck.Observe(true, false, 12000, queue);
    stuck.Observe(false, false, 28100, queue);
    assert(!queue.Pop(event));

    assert(IsLong({0, 15000, true}) && SeekDeltaMs({0, 15000, true}) == 120000);
    std::puts("PASS: COM2000 short/hold, no repeats, multiple edges, queue overflow, staleness and wrap.");
}
