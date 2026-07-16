// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "sdr/SdrSource.h"
#include <sdrplay_api.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace ttc {

// SDRplay RSP2 IQ source via the SDRplay API (libsdrplay_api). Compiled only when
// -DBUILD_SDRPLAY=ON. Streams IQ from the Orion's spare-antenna jack; the IQ
// callback runs on the SDRplay streaming thread (consumers must be quick / hand off).
class SdrPlaySource : public SdrSource {
public:
    SdrPlaySource();
    ~SdrPlaySource() override;

    bool apiOk() const { return apiOpen_; }

    bool start(double centerHz, double sampleRate) override;
    void stop() override;
    void setCenterFrequency(double hz) override;
    void setIqCallback(std::function<void(const IqBlock&)> cb) override { cb_ = std::move(cb); }

    // Which RSP2 SMA the spare-jack coax is on (A = Orion, B = Omni VII in
    // Jon's setup). Applies live if streaming, so radio switches retune the
    // panadapter input without a restart. Default Antenna A.
    void setAntennaB(bool b);

    // RSP2 built-in MW/broadcast-band RF notch filter (helps on a shared HF
    // feed near strong AM broadcasters). Applies live if streaming.
    void setBroadcastNotch(bool on);
    bool antennaB() const { return antennaB_; }
    bool broadcastNotch() const { return notch_; }

    // Front-end gain. The shared-antenna feed is hot, so more attenuation than
    // usual: raise LNAstate (0..8 on RSP2) and gRdB (20..59) to clear ADC overload.
    void setGain(int gRdB, int lnaState) { gRdB_ = gRdB; lnaState_ = lnaState; }
    // Same, but applies immediately while streaming (user gain control).
    void setGainLive(int gRdB, int lnaState);
    int  gainReduction() const { return gRdB_; }
    int  lnaState() const { return lnaState_; }

    // Decimation reduces the effective sample rate / panadapter span
    // (fs / factor). factor 1 = off. Set before start(). Effective span in Hz is
    // sampleRate/factor; analog BW is narrowed to match to avoid aliasing.
    void setDecimation(int factor) { decim_ = factor < 1 ? 1 : factor; }
    int  decimation() const { return decim_; }

    std::string lastError() const { return err_; }

    // ADC overload events since start (the API repeats them while the
    // condition persists, so a 100 W carrier at the tap produces a rapid
    // burst — the TX monitor's fastest trigger). Any thread.
    unsigned overloadCount() const {
        return overloads_.load(std::memory_order_relaxed);
    }
    // Epoch ms (same clock as QDateTime::currentMSecsSinceEpoch, but this
    // file stays Qt-free — sdr_probe links it without Qt).
    int64_t lastOverloadMs() const {
        return lastOverloadMs_.load(std::memory_order_relaxed);
    }
    // TX panic: when armed, an overload event slams the tuner to maximum
    // attenuation FROM THE EVENT CALLBACK — milliseconds instead of the
    // GUI tick. Deliberately does NOT touch gRdB_/lnaState_, so the
    // logical (receive) gain state survives; the owner restores by simply
    // re-applying it with setGainLive. Calling sdrplay_api_Update from
    // the event callback is proven ground: the overload ack has always
    // been sent from there.
    void setTxPanic(bool armed) {
        panicArmed_.store(armed, std::memory_order_relaxed);
    }

private:
    static void streamCb(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                         unsigned int numSamples, unsigned int reset, void* ctx);
    static void eventCb(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                        sdrplay_api_EventParamsT* params, void* ctx);
    bool fail(const char* where, sdrplay_api_ErrT e);

    std::function<void(const IqBlock&)> cb_;
    std::string err_;
    bool apiOpen_   = false;
    bool streaming_ = false;
    bool antennaB_  = false;
    bool notch_     = false;
    int  gRdB_      = 40;   // tuned for the hot shared-antenna feed (see docs/phase0-sdr.md)
    int  lnaState_  = 6;
    int  decim_     = 1;
    std::atomic<unsigned> overloads_{0};
    std::atomic<int64_t> lastOverloadMs_{0};
    std::atomic<bool> panicArmed_{false};
    int64_t lastPanicMs_ = 0;              // event-callback thread only

    sdrplay_api_DeviceT       device_{};
    sdrplay_api_DeviceParamsT* params_ = nullptr;
    IqBlock scratch_;                       // reused per-callback buffer
};

} // namespace ttc
