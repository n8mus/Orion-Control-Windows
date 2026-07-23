// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/TenTecOmni7.h"

#include <QHostAddress>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ttc {

namespace {

int clampi(int v, int lo, int hi) { return std::clamp(v, lo, hi); }

// The 38 receive-filter presets, *W id 0..37 (doc table, 14 kHz .. 200 Hz).
constexpr int kRxFilt[] = {
    14000, 9000, 8000, 7500, 7000, 6500, 6000, 5500, 5000, 4500,
    4000, 3800, 3600, 3400, 3200, 3000, 2800, 2600, 2500, 2400,
    2200, 2000, 1800, 1600, 1400, 1200, 1000, 900, 800, 700,
    600, 500, 450, 400, 350, 300, 250, 200,
};
constexpr int kRxFiltCount = static_cast<int>(std::size(kRxFilt));

// The 17 sideband TX bandwidth presets, *C1O id 0..16.
constexpr int kTxFilt[] = {
    4000, 3800, 3600, 3400, 3200, 3000, 2800, 2600, 2500, 2400,
    2200, 2000, 1800, 1600, 1400, 1200, 1000,
};
constexpr int kTxFiltCount = static_cast<int>(std::size(kTxFilt));

int nearestIdx(const int* table, int n, int hz) {
    int best = 0;
    for (int i = 1; i < n; ++i)
        if (std::abs(table[i] - hz) < std::abs(table[best] - hz)) best = i;
    return best;
}

// Inverse of SMeterWidget's hamlib TT565 cal table: the Omni reports
// S-units + dB directly, the console's meter wants Orion raw units.
int rawFromDbS9(double db) {
    struct Pt { double raw, dbv; };
    static const Pt cal[] = {
        {10, -48}, {24, -42}, {38, -36}, {47, -30}, {61, -24}, {70, -18},
        {79, -12}, {84, -6},  {94, 0},   {103, 10}, {118, 20}, {134, 30},
        {147, 40}, {161, 50},
    };
    constexpr int n = static_cast<int>(std::size(cal));
    if (db <= cal[0].dbv)     return static_cast<int>(cal[0].raw);
    if (db >= cal[n - 1].dbv) return static_cast<int>(cal[n - 1].raw);
    for (int i = 1; i < n; ++i)
        if (db <= cal[i].dbv) {
            const double f = (db - cal[i - 1].dbv) / (cal[i].dbv - cal[i - 1].dbv);
            return static_cast<int>(cal[i - 1].raw + f * (cal[i].raw - cal[i - 1].raw));
        }
    return static_cast<int>(cal[n - 1].raw);
}

} // namespace

TenTecOmni7::TenTecOmni7(QObject* parent) : RadioController(parent) {
    caps_.name             = "Ten-Tec Omni VII (Omni 8)";
    caps_.bandwidthMinHz   = 200;
    caps_.bandwidthMaxHz   = 14000;
    caps_.bandwidthStepHz  = 0;      // preset ladder, not continuous
    caps_.pbtRangeHz       = 8192;
    caps_.continuousFilter = false;
    caps_.dualReceiver     = false;  // one receiver; B is a TX-split dial
    caps_.needsHwHandshake = true;
    trace_ = qEnvironmentVariableIsSet("TTC_TRACE");
    serial_.setRawMode(true);
    connect(&serial_, &SerialPort::bytesReceived, this, &TenTecOmni7::onBytes);

    pttKeepalive_ = new QTimer(this);
    pttKeepalive_->setInterval(2000);   // radio drops TX after 5 s of silence
    connect(pttKeepalive_, &QTimer::timeout, this,
            [this] { send(QByteArray("*T\x04", 3) + char(0x00)); });
}

bool TenTecOmni7::open(const std::string& device) {
    const QString dev = QString::fromStdString(device);
    if (dev.startsWith(QStringLiteral("udp:"))) {
        // One Plug Ethernet (radio booted into REMOTE mode, hold-2 power-on):
        // udp:HOST[:PORT], default CMD port 49152.
        const QString spec = dev.mid(4);
        const int colon = spec.lastIndexOf(':');
        const QString host = colon > 0 ? spec.left(colon) : spec;
        netPort_ = colon > 0 ? quint16(spec.mid(colon + 1).toUInt()) : 49152;
        netAddr_ = QHostAddress(host).toIPv4Address();
        if (!netAddr_ || !netPort_) return false;
        netPass_ = quint16(QSettings().value("radio/netPasscode", 0).toUInt());
        udp_ = new QUdpSocket(this);
        // Symmetric ports: the radio replies TO the CMD port, so we must own
        // it locally — an ephemeral source port gets silence (live-found
        // during bring-up; the One Plug audio streams work the same way).
        if (!udp_->bind(QHostAddress::AnyIPv4, netPort_)) {
            delete udp_;
            udp_ = nullptr;
            return false;
        }
        connect(udp_, &QUdpSocket::readyRead, this, [this] {
            while (udp_ && udp_->hasPendingDatagrams()) {
                QByteArray d(int(udp_->pendingDatagramSize()), 0);
                udp_->readDatagram(d.data(), d.size());
                onBytes(d);                    // datagrams carry whole frames
            }
        });
        return true;
    }
    return serial_.open(device, 57600, /*hwHandshake=*/true);
}

void TenTecOmni7::send(const QByteArray& payload) {
    if (trace_) {
        fprintf(stderr, "[omni->]");
        for (char c : payload) fprintf(stderr, " %02x", static_cast<unsigned char>(c));
        fprintf(stderr, "\n");
    }
    if (udp_) {
        QByteArray d;
        d.append(char(netPass_ >> 8));         // 2-byte BE NET PASSCODE
        d.append(char(netPass_ & 0xff));
        d.append(payload);
        d.append('\r');
        udp_->writeDatagram(d, QHostAddress(netAddr_), netPort_);
        return;
    }
    serial_.write(payload + '\r');
}

// ---- tuning / mode / filter -------------------------------------------------

void TenTecOmni7::setFrequencyHz(Rx rx, uint64_t hz) {
    QByteArray p;
    p += (rx == Rx::Main) ? "*A" : "*B";
    const uint32_t f = static_cast<uint32_t>(hz);
    p += char((f >> 24) & 0xff);
    p += char((f >> 16) & 0xff);
    p += char((f >> 8) & 0xff);
    p += char(f & 0xff);
    send(p);
}

void TenTecOmni7::queryFrequency(Rx rx) { send(rx == Rx::Main ? "?A" : "?B"); }

char TenTecOmni7::modeChar(Mode m) {
    switch (m) {                       // Omni numbering (Jupiter-compatible)
        case Mode::AM:  return '0';
        case Mode::USB: return '1';
        case Mode::LSB: return '2';
        case Mode::CWU: return '3';
        case Mode::FM:  return '4';
        case Mode::CWL: return '5';
    }
    return '1';
}

Mode TenTecOmni7::charMode(char c) {
    switch (c) {
        case '0': return Mode::AM;
        case '1': return Mode::USB;
        case '2': return Mode::LSB;
        case '3': return Mode::CWU;
        case '4': return Mode::FM;
        case '5': return Mode::CWL;
        case '6': return Mode::USB;   // FSK: closest console mode
    }
    return Mode::USB;
}

void TenTecOmni7::setMode(Rx rx, Mode m) {
    // *M carries BOTH VFOs at once; first char is VFO A (live-verified —
    // set *M25 and ?RMM still answered LSB for the main).
    (rx == Rx::Main ? modeA_ : modeB_) = m;
    send(QByteArray("*M") + modeChar(modeA_) + modeChar(modeB_));
}

void TenTecOmni7::queryMode(Rx) { send("?M"); }

void TenTecOmni7::setBandwidthHz(Rx rx, int bwHz) {
    if (rx != Rx::Main) return;        // the sub dial has no separate filter
    send(QByteArray("*W") + char(nearestIdx(kRxFilt, kRxFiltCount, bwHz)));
}

void TenTecOmni7::setPbtHz(Rx rx, int pbtHz) {
    if (rx != Rx::Main) return;
    const int v = clampi(pbtHz, -8192, 8192);
    send(QByteArray("*P") + char((v >> 8) & 0xff) + char(v & 0xff));
}

void TenTecOmni7::queryFilter(Rx rx) {
    if (rx != Rx::Main) return;
    send("?W");
    send("?P");
}

// ---- receiver front end -----------------------------------------------------

void TenTecOmni7::setAgc(Rx, char agc) {
    char c = '2';
    switch (agc) {                     // no programmable AGC: 'P' -> medium
        case 'F': c = '3'; break;
        case 'M': case 'P': c = '2'; break;
        case 'S': c = '1'; break;
        case 'O': c = '0'; break;
    }
    send(QByteArray("*G") + c);
}

void TenTecOmni7::queryAgc(Rx) { send("?G"); }

void TenTecOmni7::setRfGain(Rx, int pct) {
    // Doc claims 0 = full gain / 127 = minimum, but the real radio runs the
    // scale the ordinary way up (on-air verified: the doc's inversion made
    // the console slider work backwards). Third doc error on this rig.
    send(QByteArray("*I") + char(clampi(pct, 0, 100) * 127 / 100));
}

void TenTecOmni7::queryRfGain(Rx) { send("?I"); }
void TenTecOmni7::setAttenuator(Rx, int step) {
    send(QByteArray("*J") + char('0' + clampi(step, 0, 3)));
}
void TenTecOmni7::queryAttenuator(Rx) { send("?J"); }
void TenTecOmni7::setPreamp(Rx, bool on) { send(QByteArray("*C1Z") + char(on ? 1 : 0)); }
void TenTecOmni7::queryPreamp(Rx) { send("?C1Z"); }

void TenTecOmni7::sendKTriple() {
    send(QByteArray("*K") + char(nb_) + char(nr_) + char(an_));
}
void TenTecOmni7::setNoiseBlanker(Rx, int l)   { nb_ = clampi(l, 0, 7); sendKTriple(); }
void TenTecOmni7::setNoiseReduction(Rx, int l) { nr_ = clampi(l, 0, 9); sendKTriple(); }
void TenTecOmni7::setAutoNotch(Rx, int l)      { an_ = clampi(l, 0, 9); sendKTriple(); }
void TenTecOmni7::queryDspLevels(Rx) { send("?K"); }

// ---- transmitter ------------------------------------------------------------

void TenTecOmni7::setPtt(bool on) {
    if (on) {
        send(QByteArray("*T") + char(0x04) + char(0x00));
        pttKeepalive_->start();        // radio un-keys after 5 s without this
    } else {
        pttKeepalive_->stop();
        send(QByteArray("*T") + char(0x00) + char(0x00));
    }
}

void TenTecOmni7::setTxPower(int pct)     { send(QByteArray("*C1X") + char(clampi(pct, 0, 100) * 127 / 100)); }
void TenTecOmni7::queryTxPower()          { send("?C1X"); }
void TenTecOmni7::setMicGain(int pct)     { send(QByteArray("*C1D") + char(clampi(pct, 0, 100) * 127 / 100)); }
void TenTecOmni7::queryMicGain()          { send("?C1D"); }
void TenTecOmni7::setSpeechProc(int l)    { send(QByteArray("*C1F") + char(clampi(l, 0, 9) * 127 / 9)); }
void TenTecOmni7::querySpeechProc()       { send("?C1F"); }
void TenTecOmni7::setAuxInputGain(int p)  { send(QByteArray("*C1E") + char(clampi(p, 0, 100) * 127 / 100)); }
void TenTecOmni7::queryAuxInputGain()     { send("?C1E"); }
void TenTecOmni7::setMonitor(int p)       { send(QByteArray("*C1W") + char(clampi(p, 0, 100) * 127 / 100)); }
void TenTecOmni7::setTxFilter(int hz)     { send(QByteArray("*C1O") + char(nearestIdx(kTxFilt, kTxFiltCount, hz))); }
void TenTecOmni7::queryTxFilter()         { send("?C1O"); }
void TenTecOmni7::setTxEq(int db)         { send(QByteArray("*C1I") + char(clampi(db + 20, 0, 40) * 127 / 40)); }
void TenTecOmni7::setTxRolloff(int hz)    { send(QByteArray("*C1J") + char(clampi((hz - 70) / 10, 0, 23))); }
void TenTecOmni7::setTunerEnabled(bool o) { send(QByteArray("*C1P") + char(o ? 1 : 0)); }
void TenTecOmni7::startTune()             { send("*C2A"); }
void TenTecOmni7::queryTuner()            { send("?C2A"); }

void TenTecOmni7::queryTxAudio() {
    send("?C1D");
    send("?C1F");
    send("?C1O");
    send("?C1W");
}

// ---- audio / VFO plumbing ---------------------------------------------------

void TenTecOmni7::setAfVolume(Rx rx, int pct) {
    if (rx != Rx::Main) return;        // single receiver
    send(QByteArray("*U") + char(clampi(pct, 0, 100) * 127 / 100));
}

void TenTecOmni7::queryAfVolume(Rx rx) {
    if (rx == Rx::Main) send("?U");
}

void TenTecOmni7::setVfoAssignment(char, char, char tx) {
    // The Omni has no VFO matrix — just a split flag (TX on B).
    send(QByteArray("*N") + char(tx == 'B' ? 1 : 0));
    emit vfoAssignmentReported('A', 'B', tx == 'B' ? 'B' : 'A');
}

void TenTecOmni7::queryVfoAssignment() { send("?N"); }
void TenTecOmni7::querySMeter() { send("?S"); }

// ---- response parsing -------------------------------------------------------

// Expected payload length (bytes between the prefix and the CR) for fixed-
// size binary responses; -1 = ASCII, read to CR; -2 = unknown prefix.
static int payloadLen(const QByteArray& b) {
    switch (b[0]) {
        case 'A': case 'B': return 4;
        case 'M': case 'P': return 2;
        case 'W': case 'G': case 'J': case 'N':
        case 'H': case 'I': case 'U': return 1;
        case 'K': case 'L': return 3;
        case 'V': case '@': case 'X': return -1;
        case 'Z': return -1;           // error echo: 0-1 ASCII chars
        case 'S':
            if (b.size() < 2) return -3;               // need the next byte
            return (static_cast<unsigned char>(b[1]) & 0x80) ? 2 : -1;
        case 'C': {
            if (b.size() < 3) return -3;
            const char grp = b[1], ltr = b[2];
            int n = 1;
            if (grp == '1' && ltr == 'T') n = 2;
            if (grp == '1' && ltr == 'X') n = 3;
            return 2 + n;              // group+letter chars count toward payload
        }
        default: return -2;
    }
}

void TenTecOmni7::onBytes(const QByteArray& chunk) {
    buf_ += chunk;
    for (;;) {
        if (buf_.isEmpty()) return;
        const int len = payloadLen(buf_);
        if (len == -3) return;                          // need more bytes to decide
        if (len == -2) { buf_.remove(0, 1); continue; } // resync byte-by-byte
        int end;                                        // index of the CR
        if (len == -1) {
            end = buf_.indexOf('\r');
            if (end < 0) return;
        } else {
            if (buf_.size() < 1 + len + 1) return;      // prefix + payload + CR
            end = 1 + len;
            if (buf_[end] != '\r') {                    // framing slip: resync
                buf_.remove(0, 1);
                continue;
            }
        }
        const QByteArray msg = buf_.left(end);
        buf_.remove(0, end + 1);
        if (!msg.isEmpty()) handleMsg(msg);
    }
}

void TenTecOmni7::handleMsg(const QByteArray& m) {
    if (trace_) {
        fprintf(stderr, "[omni<-]");
        for (char c : m) fprintf(stderr, " %02x", static_cast<unsigned char>(c));
        fprintf(stderr, "\n");
    }
    const auto u8 = [&](int i) { return static_cast<int>(static_cast<unsigned char>(m[i])); };
    switch (m[0]) {
        case 'A': case 'B': {
            if (m.size() < 5) return;
            const uint64_t hz = (uint64_t(u8(1)) << 24) | (u8(2) << 16)
                              | (u8(3) << 8) | u8(4);
            emit frequencyReported(m[0] == 'A' ? Rx::Main : Rx::Sub, hz);
            return;
        }
        case 'M':
            if (m.size() < 3) return;
            modeA_ = charMode(m[1]);                     // first char = VFO A
            modeB_ = charMode(m[2]);
            emit modeReported(Rx::Main, modeA_);
            emit modeReported(Rx::Sub, modeB_);
            return;
        case 'W':
            if (m.size() < 2) return;
            if (u8(1) < kRxFiltCount)
                emit bandwidthReported(Rx::Main, kRxFilt[u8(1)]);
            return;
        case 'P': {
            if (m.size() < 3) return;
            const int v = static_cast<int16_t>((u8(1) << 8) | u8(2));
            emit pbtReported(Rx::Main, v);
            return;
        }
        case 'G': {
            if (m.size() < 2) return;
            char a = 'M';
            switch (m[1]) { case '0': a = 'O'; break; case '1': a = 'S'; break;
                            case '2': a = 'M'; break; case '3': a = 'F'; break; }
            emit agcReported(Rx::Main, a);
            return;
        }
        case 'J':
            if (m.size() < 2) return;
            emit attenReported(Rx::Main, clampi(m[1] - '0', 0, 3));
            return;
        case 'I':
            if (m.size() < 2) return;
            emit rfGainReported(Rx::Main, u8(1) * 100 / 127);
            return;
        case 'U':
            if (m.size() < 2) return;
            emit afVolumeReported(Rx::Main, u8(1) * 100 / 127);
            return;
        case 'K':
            if (m.size() < 4) return;
            nb_ = u8(1); nr_ = u8(2); an_ = u8(3);
            emit nbReported(Rx::Main, nb_);
            emit nrReported(Rx::Main, nr_);
            emit autoNotchReported(Rx::Main, an_);
            return;
        case 'N':
            if (m.size() < 2) return;
            emit vfoAssignmentReported('A', 'B', m[1] ? 'B' : 'A');
            return;
        case 'S': {
            if (m.size() >= 2 && (u8(1) & 0x80)) {       // transmit: fwd/refl watts
                const double fwd = u8(1) & 0x7f, refl = m.size() >= 3 ? u8(2) : 0;
                double swr = 1.0;
                if (fwd > 0 && refl > 0 && refl < fwd) {
                    const double rho = std::sqrt(refl / fwd);
                    swr = (1 + rho) / (1 - rho);
                }
                emit txMeterReported(fwd, refl, swr);
            } else if (m.size() >= 5) {                  // receive: "SABCD" ASCII
                const int sUnits = (m[1] - '0') * 10 + (m[2] - '0');
                const int dbOver = (m[3] - '0') * 10 + (m[4] - '0');
                const double dbS9 = (sUnits - 9) * 6.0 + dbOver;
                emit sMeterReported(rawFromDbS9(dbS9), 0);
            }
            return;
        }
        case 'C': {                                      // REMOTE-mode extras
            if (m.size() < 4) return;
            const QByteArray key = m.left(3);
            if (key == "C1X") emit txPowerReported(u8(3) * 100 / 127);
            else if (key == "C1D") emit micGainReported(u8(3) * 100 / 127);
            else if (key == "C1F") emit speechProcReported(u8(3) * 9 / 127);
            else if (key == "C1E") emit auxInputGainReported(u8(3) * 100 / 127);
            else if (key == "C1W") emit monitorReported(u8(3) * 100 / 127);
            else if (key == "C1O") { if (u8(3) < kTxFiltCount) emit txFilterReported(kTxFilt[u8(3)]); }
            else if (key == "C1I") emit txEqReported(u8(3) * 40 / 127 - 20);
            else if (key == "C1J") emit txRolloffReported(70 + clampi(u8(3), 0, 23) * 10);
            else if (key == "C1Z") emit preampReported(Rx::Main, m[3] != 0);
            else if (key == "C2A") emit tunerReported(u8(3) & 0x01);
            return;
        }
        default:
            return;                                      // V/X/@/Z etc: informational
    }
}

} // namespace ttc
