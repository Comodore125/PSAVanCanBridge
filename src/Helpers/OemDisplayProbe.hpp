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
        return (result==1 || result==2) ? "sent; check display" : "failed; inspect OEM diagnostics";
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

    // Fixed-size little-endian telemetry (144 B). Avoid JSON allocations and
    // snprintf formatting on ESP32: /media decodes this via DataView.
    static constexpr size_t TelemetryWords = 36;
    void Snapshot(uint32_t* out) const {
        const uint32_t now=NowMs();
        out[0]=0x4F454D31U; // OEM1 protocol version
        out[1]=(periodic_.load()?1U:0U)|(busy_.load()?2U:0U);
        out[2]=intervalMs_.load(); out[3]=remaining_.load();
        out[4]=lastCode_.load(); out[5]=lastFc_.load();
        out[6]=txCount_.load(); out[7]=fcCount_.load();
        out[8]=peerCount_.load(); out[9]=lastSource_.load();
        out[10]=now-lastPeerMs_.load();
        const uint32_t head=traceHead_.load();
        const uint32_t count=head<12?head:12;
        out[11]=count;
        for(uint32_t i=0;i<12;++i) {
            const uint32_t slot=(head-count+i)%12;
            out[12+i*2]=i<count?traces_[slot].at.load():0;
            out[13+i*2]=i<count?traces_[slot].tag.load():0;
        }
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
    std::atomic<uint32_t> intervalMs_{10000}, nextMs_{0};
    std::atomic<uint8_t> remaining_{0};
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

};
