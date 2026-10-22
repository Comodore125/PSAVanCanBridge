#pragma once
// Deliberate OEM 0x63 diagnostic proof-of-concept. No automatic startup.
// Passive CAN observation in RX path; capped repeat uses the already-running
// low-priority Spotify task, never the CAN/VAN real-time handler.
#include <atomic>
#include <cstdint>
#include <cstdio>
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

    // Constant-time receive-side observation. Never wait, allocate or send.
    void Observe(const BusMessage& m) {
        if (m.id == 0x165 && m.dataLength >= 3) {
            uint8_t source = (m.data[0] & 0x80) ? ((m.data[2] >> 4) & 7) : 0;
            if (source != lastSource_.exchange(source)) Trace(5, source);
        } else if (m.id == 0x2E3) {
            lastPeerMs_.store(NowMs());
            ++peerCount_;
            Trace(4, Pack(m));
            // Fail closed on another sender, even in the middle of a series.
            periodic_.store(false);
        } else if (m.id == 0x15F && m.dataLength >= 3) {
            ++fcCount_;
            const uint32_t fc = Pack(m);
            Trace(3, fc);
            if (awaitFc_.load()) flowControl_.store(0x1000000U | fc);
        }
    }

    const char* Send(const char* artist, const char* title, bool confirmed) {
        if (!confirmed) return "explicit CAN risk acknowledgement required";
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) return "probe busy";
        periodic_.store(false);
        OemText63::Payload payload{};
        const uint8_t result = OemText63::Encode(artist,title,payload)
            ? Transmit(payload) : 9;
        Finish(result);
        busy_.store(false);
        return Describe(result);
    }

    bool StartRepeat(const char* artist, const char* title, uint32_t interval,
                     bool confirmed) {
        if (!confirmed || (interval != 5000 && interval != 10000 &&
                           interval != 30000)) return false;
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) return false;
        OemText63::Payload payload{};
        const bool ok = OemText63::Encode(artist,title,payload) &&
                        bus_ && Aux1() && Quiet(NowMs());
        if (ok) {
            repeatPayload_ = payload;
            intervalMs_ = interval;
            remaining_ = 8; // 8 sends maximum; no permanent CAN traffic.
            nextMs_ = NowMs(); // first attempt on next low-priority tick
            periodic_.store(true);
            Trace(6, interval);
        }
        busy_.store(false);
        return ok;
    }

    void Stop() {
        periodic_.store(false);
        Trace(7, 0);
    }

    // Invoked once per 200ms from the existing Spotify task: no extra RTOS task,
    // timer, heap allocation, metadata polling or CAN receive path work.
    void Tick() {
        if (!periodic_.load() || static_cast<int32_t>(NowMs()-nextMs_) < 0) return;
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) return;
        if (periodic_.load()) {
            const uint8_t code = Transmit(repeatPayload_);
            Finish(code);
            if (code != 1 && code != 2) periodic_.store(false);
            if (remaining_) --remaining_;
            if (!remaining_) periodic_.store(false);
            nextMs_ = NowMs() + intervalMs_;
        }
        busy_.store(false);
    }

    void Status(char* out, size_t capacity) const {
        const uint32_t now = NowMs();
        const uint32_t peer = lastPeerMs_.load();
        int used = snprintf(out,capacity,
            "{\"repeat\":%s,\"busy\":%s,\"interval_ms\":%lu,"
            "\"remaining\":%u,\"last_code\":%u,\"last_fc\":%lu,"
            "\"tx_frames\":%lu,\"rx_fc\":%lu,\"rx_2e3\":%lu,"
            "\"source\":%u,\"since_peer_ms\":%lu,\"trace\":[",
            periodic_.load()?"true":"false",busy_.load()?"true":"false",
            static_cast<unsigned long>(intervalMs_),unsigned(remaining_),
            unsigned(lastCode_.load()),static_cast<unsigned long>(lastFc_.load()),
            static_cast<unsigned long>(txCount_.load()),
            static_cast<unsigned long>(fcCount_.load()),
            static_cast<unsigned long>(peerCount_.load()),
            unsigned(lastSource_.load()),static_cast<unsigned long>(now-peer));
        if (used < 0 || static_cast<size_t>(used) >= capacity) return;
        const uint32_t head = traceHead_.load();
        const uint32_t count = head < 12 ? head : 12;
        for(uint32_t j=0; j<count; ++j) {
            const uint32_t i = (head - count + j) % 12;
            const int n = snprintf(out+used,capacity-used,"%s[%lu,%lu]",
                j?",":"",static_cast<unsigned long>(traces_[i].at.load()),
                static_cast<unsigned long>(traces_[i].tag.load()));
            if (n<0 || static_cast<size_t>(n)>=capacity-used) return;
            used+=n;
        }
        snprintf(out+used,capacity-used,"]}");
    }

private:
    struct TraceEntry { std::atomic<uint32_t> at{0}, tag{0}; };
    TraceEntry traces_[12]{};
    std::atomic<uint32_t> traceHead_{0};
    std::atomic<uint32_t> lastPeerMs_{0}, flowControl_{0}, lastFc_{0};
    std::atomic<uint32_t> txCount_{0}, fcCount_{0}, peerCount_{0};
    std::atomic<uint8_t> lastSource_{0}, lastCode_{0};
    std::atomic<bool> busy_{false}, periodic_{false}, awaitFc_{false};
    OemText63::Payload repeatPayload_{};
    uint32_t intervalMs_ = 10000, nextMs_ = 0;
    uint8_t remaining_ = 0;
    ITransportLayer* bus_ = nullptr;
    CarState* state_ = nullptr;

    static uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time()/1000ULL); }
    static uint32_t Pack(const BusMessage& m) {
        return ((m.dataLength ? uint32_t(m.data[0]) : 0)<<16) |
               ((m.dataLength>1 ? uint32_t(m.data[1]) : 0)<<8) |
               (m.dataLength>2 ? uint32_t(m.data[2]) : 0);
    }
    void Trace(uint8_t kind, uint32_t data) {
        const uint32_t slot = traceHead_.fetch_add(1) % 12;
        traces_[slot].tag.store((uint32_t(kind)<<24)|(data&0xFFFFFF));
        traces_[slot].at.store(NowMs());
    }
    bool Aux1() const {
        return state_ && RadioAudioSource::FreshSource(
            __atomic_load_n(&state_->RadioAudioSourceStamped,__ATOMIC_RELAXED),
            esp_timer_get_time()/1000ULL)==4;
    }
    bool Quiet(uint32_t now) const {
        return now>30000U && uint32_t(now-lastPeerMs_.load())>30000U;
    }
    bool Tx(const OemText63::Frame& frame) {
        if (!bus_ || !Aux1() || !Quiet(NowMs()) ||
            (!periodic_.load() && periodicTx_)) return false;
        BusMessage m{};
        m.id=0x2E3; m.protocol=ProtocolType::AEE2004; m.type=MessageType::Normal;
        m.dataLength=frame.length;
        for(uint8_t i=0;i<frame.length;++i) m.data[i]=frame.data[i];
        const bool ok = bus_->SendMessage(m)!=0;
        if(ok) { ++txCount_; Trace(2,Pack(m)); }
        return ok;
    }
    bool periodicTx_ = false;
    uint8_t Transmit(const OemText63::Payload& p) {
        if(!bus_) return 10;
        if(!Aux1()) return 3;
        if(!Quiet(NowMs())) return 4;
        periodicTx_=periodic_.load();
        flowControl_.store(0);
        if(p.size<=7) {
            const uint8_t rc=Tx(OemText63::SingleFrame(p))?2:5;
            periodicTx_=false;
            return rc;
        }
        awaitFc_.store(true);
        if(!Tx(OemText63::FirstFrame(p))) {
            awaitFc_.store(false); periodicTx_=false; return 5;
        }
        uint32_t fc=0;
        for(unsigned i=0;i<60;++i) {
            fc=flowControl_.load();
            if(fc) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        awaitFc_.store(false);
        if(!fc) {periodicTx_=false;return 6;}
        const uint8_t type=uint8_t((fc>>16)&0xFF);
        const uint8_t block=uint8_t((fc>>8)&0xFF);
        const uint8_t stmin=uint8_t(fc&0xFF);
        lastFc_.store(fc&0xFFFFFF);
        if(type!=0x30 || block!=0 || stmin>0x7F) {
            periodicTx_=false;return 7;
        }
        size_t offset=6; uint8_t seq=1;
        while(offset<p.size) {
            vTaskDelay(pdMS_TO_TICKS(stmin<10?10:stmin));
            const auto f=OemText63::ConsecutiveFrame(p,offset,seq++);
            if(!f.length || !Tx(f)) {periodicTx_=false;return 5;}
            offset+=f.length-1;
        }
        periodicTx_=false;
        return 1;
    }
    void Finish(uint8_t code) {lastCode_.store(code);Trace(8,code);}
    static const char* Describe(uint8_t code) {
        switch(code) {
            case 1: return "ISO-TP complete; inspect EMF-C";
            case 2: return "ISO-TP single frame sent; inspect EMF-C";
            case 3: return "no fresh AUX1 (0x165)";
            case 4: return "0x2E3 observed or 30s quiet time not met";
            case 5: return "CAN TX blocked or failed";
            case 6: return "no physical 0x15F flow control";
            case 7: return "unsupported 0x15F flow control";
            case 9: return "invalid artist/title";
            default: return "CAN transport unavailable";
        }
    }
};
