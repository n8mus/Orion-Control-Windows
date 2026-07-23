// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "radio/RadioController.h"
#include "radio/SerialPort.h"

class QTimer;
class QUdpSocket;

namespace ttc {

// Ten-Tec Omni VII (588) driver — the console's "Omni 8" personality.
// Binary CAT over 57600 8N1 with RTS/CTS. Commands are '*'<letters><binary
// data><CR>, queries '?'<letters><CR>; responses echo the command letters
// followed by binary data and CR. Binary payloads can contain 0x0D, so the
// parser frames by a per-prefix expected-length table, not by CR splitting.
//
// Live-verified against Jon's radio (firmware 1.036, RADIO mode):
//  - ?M response: FIRST char is VFO A's mode (the doc's <d1><d0> byte-order
//    table is wrong; its example was right).
//  - The whole C1/C2 config group (TX power, mic, EQ, rolloff, preamp,
//    antenna, tuner...) answers only in REMOTE mode — in RADIO mode sets
//    return the 'Z' error and queries are silent. The methods below send
//    them anyway: harmless in RADIO mode, functional if Jon ever flips the
//    radio to REMOTE (which locks its front panel).
//
// Transports: a serial device path, OR "udp:HOST[:PORT]" for the radio's
// One Plug Ethernet in REMOTE mode (default port 49152). Same command set;
// each UDP payload is a 2-byte big-endian NET PASSCODE (radio/netPasscode,
// default 0) + the serial frame. Ports are symmetric — the radio replies to
// the command port, so we bind it locally. One datagram carries whole
// frames, fed to the same length-table parser. Live-ruled 12.9 ms median
// round-trip, 0 loss (tools/omni7_ruler.py).
class TenTecOmni7 : public RadioController {
    Q_OBJECT
public:
    explicit TenTecOmni7(QObject* parent = nullptr);

    bool open(const std::string& device) override;

    const CapabilityProfile& caps() const override { return caps_; }
    bool connected() const override { return serial_.isOpen() || udp_; }

    void setFrequencyHz(Rx rx, uint64_t hz) override;    // *A/*B 4-byte BE
    void setMode(Rx rx, Mode mode) override;             // *M<chA><chB>
    void setBandwidthHz(Rx rx, int bwHz) override;       // *W nearest of 38 presets
    void setPbtHz(Rx rx, int pbtHz) override;            // *P 2-byte signed BE
    void queryFrequency(Rx rx) override;                 // ?A / ?B
    void queryFilter(Rx rx) override;                    // ?W + ?P (main only)
    void queryMode(Rx rx) override;                      // ?M (reports both VFOs)

    void setAgc(Rx, char agc) override;                  // *G '0'..'3'
    void setRfGain(Rx, int pct) override;                // *I (0 = MAX: inverted)
    void setAttenuator(Rx, int step) override;           // *J '0'..'3'
    void setNoiseReduction(Rx, int level) override;      // *K triple (cached)
    void setNoiseBlanker(Rx, int level) override;
    void setAutoNotch(Rx, int level) override;
    void setPreamp(Rx, bool on) override;                // *C1Z (REMOTE only)
    void queryAgc(Rx) override;
    void queryRfGain(Rx) override;
    void queryAttenuator(Rx) override;
    void queryPreamp(Rx) override;
    void queryDspLevels(Rx) override;                    // ?K

    void setPtt(bool on) override;                       // *T + 2 s keepalive
    void setTxPower(int pct) override;                   // *C1X (REMOTE only)
    void setMicGain(int pct) override;                   // *C1D (REMOTE only)
    void setSpeechProc(int level) override;              // *C1F (REMOTE only)
    void setTxFilter(int hz) override;                   // *C1O preset (REMOTE only)
    void setAuxInputGain(int pct) override;              // *C1E line (REMOTE only)
    void setMonitor(int pct) override;                   // *C1W (REMOTE only)
    void setTxEq(int db) override;                       // *C1I ±20 dB (REMOTE only)
    void setTxRolloff(int hz) override;                  // *C1J 70-300 (REMOTE only)
    void setTunerEnabled(bool on) override;              // *C1P (REMOTE only)
    void startTune() override;                           // *C2A (REMOTE only)
    void queryTxPower() override;
    void queryTxAudio() override;
    void queryTxFilter() override;
    void querySpeechProc() override;
    void queryMicGain() override;
    void queryAuxInputGain() override;
    void queryTuner() override;

    void setAfVolume(Rx, int pct) override;              // *U (main only)
    void queryAfVolume(Rx) override;

    void setVfoAssignment(char m, char s, char t) override;  // split via *N
    void queryVfoAssignment() override;                  // ?N
    void querySMeter() override;                         // ?S

private slots:
    void onBytes(const QByteArray& chunk);

private:
    void send(const QByteArray& payload);                // appends CR, writes
    void sendKTriple();
    void handleMsg(const QByteArray& msg);               // one framed response
    static char modeChar(Mode m);
    static Mode charMode(char c);

    SerialPort serial_;
    QUdpSocket* udp_ = nullptr;          // One Plug Ethernet ("udp:" device)
    quint32 netAddr_ = 0;                // radio IPv4 (host byte order)
    quint16 netPort_ = 49152;            // UDP CMD port, symmetric both ends
    quint16 netPass_ = 0;                // NET PASSCODE (radio/netPasscode)
    CapabilityProfile caps_;
    QByteArray buf_;                                     // parser accumulator
    QTimer* pttKeepalive_ = nullptr;                     // radio drops TX after 5 s
    Mode modeA_ = Mode::LSB, modeB_ = Mode::LSB;         // *M sets both at once
    int nb_ = 0, nr_ = 0, an_ = 0;                       // *K sets all three
    bool trace_ = false;
};

} // namespace ttc
