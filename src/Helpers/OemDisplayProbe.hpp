#pragma once
// Opt-in, one-shot CAN 0x2E3 ISO-TP *bench* experiment.
// The stock RD45 may own 0x2E3: CAN silence is not proof of bus isolation.
// No periodic TX, no source spoofing, no changes to the bridge scheduler.
#include <atomic>
#include <cstdint>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Protocol/BusMessage.hpp"
#include "Protocol/ITransportLayer.hpp"
#include "Helpers/CarState.hpp"
#include "Helpers/RadioAudioSource.hpp"
#include "Helpers/OemText63.hpp"

class OemDisplayProbe {
public:
    static OemDisplayProbe& Instance() { static OemDisplayProbe s; return s; }
    void Bind(ITransportLayer* bus, CarState* state) { bus_ = bus; state_ = state; }

    // Called from the existing destination RX hook. Always O(1), no allocation.
    void Observe(const BusMessage& message) {
        if (message.id == 0x2E3) {
            lastPeerMs_.store(NowMs(), std::memory_order_relaxed);
        } else if (message.id == 0x15F && message.dataLength >= 3 &&
                   armed_.load(std::memory_order_relaxed)) {
            const uint32_t response = 0x1000000U |
                (uint32_t(message.data[0]) << 16) |
                (uint32_t(message.data[1]) << 8) | message.data[2];
            flowControl_.store(response, std::memory_order_release);
        }
    }

    // Runs in the HTTP server task, NEVER a VAN/CAN receive or scheduler task.
    // An isolated test harness must be confirmed by the operator each session.
    const char* Send(const char* artist, const char* title, bool isolated) {
        if (!isolated) return "isolated bench confirmation required";
        if (!bus_ || !state_) return "destination CAN unavailable";
        bool expected = false;
        if (!armed_.compare_exchange_strong(expected, true)) return "probe busy";
        const char* result = SendOnce(artist, title);
        armed_.store(false, std::memory_order_release);
        return result;
    }

private:
    std::atomic<bool> armed_{false};
    std::atomic<uint32_t> lastPeerMs_{0};
    std::atomic<uint32_t> flowControl_{0};
    ITransportLayer* bus_ = nullptr;
    CarState* state_ = nullptr;

    static uint32_t NowMs() {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }
    bool Aux1() const {
        return state_ &&
            RadioAudioSource::FreshSource(
                __atomic_load_n(&state_->RadioAudioSourceStamped, __ATOMIC_RELAXED),
                esp_timer_get_time() / 1000ULL) == 4U;
    }
    bool Quiet(uint32_t now) const {
        // 30 seconds of *observed* quiet and 30 seconds since boot.
        return now > 30000U &&
            uint32_t(now - lastPeerMs_.load(std::memory_order_relaxed)) > 30000U;
    }
    bool Tx(const OemText63::Frame& frame) {
        if (!Aux1() || !Quiet(NowMs())) return false;
        BusMessage msg{};
        msg.id = 0x2E3;
        msg.protocol = ProtocolType::AEE2004;
        msg.type = MessageType::Normal;
        msg.dataLength = frame.length;
        for (uint8_t i=0; i<frame.length; ++i) msg.data[i] = frame.data[i];
        return bus_->SendMessage(msg) != 0;
    }

    const char* SendOnce(const char* artist, const char* title) {
        OemText63::Payload payload{};
        if (!OemText63::Encode(artist, title, payload))
            return "invalid artist/title";
        if (!Aux1()) return "RD45 must report fresh AUX1 (0x165)";
        if (!Quiet(NowMs())) return "CAN 0x2E3 is busy or quiet-time not met";
        flowControl_.store(0, std::memory_order_release);
        if (payload.size <= 7) {
            if (!Tx(OemText63::SingleFrame(payload))) return "CAN single frame failed";
            return "ISO-TP single frame sent; check physical display";
        }
        if (!Tx(OemText63::FirstFrame(payload))) return "CAN first frame failed";

        // Wait for physical display FC 0x15F, not a fabricated software ACK.
        uint32_t fc = 0;
        for (unsigned i=0; i<60; ++i) {
            fc = flowControl_.load(std::memory_order_acquire);
            if (fc) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!fc) return "timeout waiting for 0x15F flow control";
        const uint8_t status = static_cast<uint8_t>((fc >> 16) & 0xFF);
        const uint8_t blockSize = static_cast<uint8_t>((fc >> 8) & 0xFF);
        const uint8_t stmin = static_cast<uint8_t>(fc & 0xFF);
        if (status != 0x30 || blockSize != 0 || stmin > 0x7F)
            return "unsupported ISO-TP flow control";
        const uint32_t separation = stmin < 10 ? 10U : stmin;
        size_t offset = 6;
        uint8_t sequence = 1;
        while (offset < payload.size) {
            vTaskDelay(pdMS_TO_TICKS(separation));
            const OemText63::Frame frame =
                OemText63::ConsecutiveFrame(payload, offset, sequence++);
            if (!frame.length || !Tx(frame)) return "CAN consecutive frame aborted";
            offset += frame.length - 1;
        }
        return "ISO-TP complete; physical display must be checked";
    }
};
