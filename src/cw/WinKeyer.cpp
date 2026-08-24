// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/WinKeyer.h"

#include <QSerialPort>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <algorithm>

namespace ttc {

namespace {
// WK2 status byte (0xC0 | bits), K1EL datasheet values.
constexpr quint8 kStBreakIn = 0x02;
constexpr quint8 kStBusy    = 0x04;
} // namespace

WinKeyer::WinKeyer(QObject* parent) : CwKeyer(parent) {
    ser_ = new QSerialPort(this);
    connect(ser_, &QSerialPort::readyRead, this, &WinKeyer::onReadyRead);
    potTimer_ = new QTimer(this);
    potTimer_->setSingleShot(true);
    potTimer_->setInterval(250);
    connect(potTimer_, &QTimer::timeout, this, [this] {
        if (potPending_ >= 0 && potPending_ != potEmitted_) {
            potEmitted_ = potPending_;
            emit potChanged(potEmitted_);
        }
    });
}

WinKeyer::~WinKeyer() { close(); }

// Settings-driven open, so the CW window never has to know that THIS
// backend is the one needing a serial port and a pot calibration.
bool WinKeyer::open() {
    QSettings s;
    const QString dev = s.value("cw/port").toString();
    if (dev.isEmpty()) {                   // unset = no keyer configured
        err_ = "no keyer port set (SDR ▸ Station setup…)";
        return false;
    }
    // Adopt whatever the operator has set in the control panel BEFORE the
    // port opens, so open()'s applyOwned() pushes them in one go. A key
    // that was never written stays unset and the keyer keeps its own.
    loadOwned();
    if (!open(dev)) return false;
    setPotRange(s.value("cw/potMin", 7).toInt(),
                s.value("cw/potMax", 45).toInt());
    return true;
}

// cw/wk/* mirrors the adopted set. Absent key = never adopted, which is
// the whole point: the console must not start writing element timing to
// a keyer whose owner never asked it to.
void WinKeyer::loadOwned() {
    QSettings s;
    const auto opt = [&s](const char* key) -> std::optional<int> {
        const QString k = QLatin1String("cw/wk/") + QLatin1String(key);
        if (!s.contains(k)) return std::nullopt;
        return s.value(k).toInt();
    };
    weight_      = opt("weight");
    keyComp_     = opt("keyComp");
    firstExt_    = opt("firstExt");
    ratio_       = opt("ratio");
    farns_       = opt("farnsworth");
    pttLead_     = opt("pttLead");
    pttTail_     = opt("pttTail");
    switchpoint_ = opt("switchpoint");
    sidetone_    = opt("sidetone");
    letterspace_ = opt("letterspace");
    // The mode register is one-way (it cannot be read back off the
    // keyer), so it is adopted only while the panel's explicit opt-in
    // is on — see setModeRegister.
    modeReg_ = s.value("cw/wk/manageMode", false).toBool() ? opt("modeReg")
                                                           : std::nullopt;
}

bool WinKeyer::open(const QString& device) {
    if (open_) close();
    ser_->setPortName(device);
    ser_->setBaudRate(1200);
    ser_->setDataBits(QSerialPort::Data8);
    ser_->setParity(QSerialPort::NoParity);
    ser_->setStopBits(QSerialPort::TwoStop);
    ser_->setFlowControl(QSerialPort::NoFlowControl);
    if (!ser_->open(QIODevice::ReadWrite)) {
        err_ = ser_->errorString();
        emit errorOccurred("WinKeyer: " + err_);
        return false;
    }
    ser_->setDataTerminalReady(true);
    ser_->setRequestToSend(false);
    // Handshake reads are blocking; keep the readyRead slot from eating
    // the reply bytes mid-wait.
    const QSignalBlocker noAsync(ser_);
    // The WKUSB needs a beat after DTR asserts before it listens — without
    // this the echo test times out (found the hard way; 800 ms is safe).
    QThread::msleep(800);
    ser_->clear();
    // Wake + sanity: three nulls, then Admin:Echo of one byte, then
    // Admin:Host Open (returns the firmware revision).
    const char nulls[] = {0x13, 0x13, 0x13};
    ser_->write(nulls, 3);
    ser_->flush();
    QThread::msleep(300);
    ser_->readAll();
    const char echo[] = {0x00, 0x04, 0x14};
    ser_->write(echo, 3);
    ser_->flush();
    QByteArray got;
    for (int i = 0; i < 10 && !got.contains(char(0x14)); ++i)
        if (ser_->waitForReadyRead(300)) got += ser_->readAll();
    if (!got.contains(char(0x14))) {
        err_ = "no echo response — is this the WinKeyer port?";
        emit errorOccurred("WinKeyer: " + err_);
        ser_->close();
        return false;
    }
    const char hostOpen[] = {0x00, 0x02};
    ser_->write(hostOpen, 2);
    ser_->flush();
    ser_->waitForReadyRead(600);
    // Host open answers with the firmware revision (31 = rev 31.03). We
    // used to throw it away; the control panel shows it, and it is the
    // only cheap way to tell a WK2 apart from a WK3 — which matters
    // because the two disagree about sidetone encoding and the X1MODE
    // bit layout. Host open also leaves the keyer in WK1 mode.
    const QByteArray rev = ser_->readAll();
    fwRev_ = rev.isEmpty() ? -1 : int(uchar(rev.back()));
    wk2_ = false;
    open_ = true;
    setSpeed(wpm_);
    applyOwned();                          // re-adopt across a reconnect
    return true;
}

void WinKeyer::close() {
    if (!ser_->isOpen()) return;
    if (open_) {
        stop();
        const char hostClose[] = {0x00, 0x03};
        ser_->write(hostClose, 2);
        ser_->flush();
    }
    ser_->close();
    open_ = false;
    if (busy_) { busy_ = false; emit busyChanged(false); }
}

void WinKeyer::setSpeed(int wpm) {
    wpm_ = std::clamp(wpm, 5, 99);
    if (!open_) return;
    const char cmd[] = {0x02, char(wpm_)};
    ser_->write(cmd, 2);
}

void WinKeyer::setPotRange(int minWpm, int maxWpm) {
    potMin_ = std::clamp(minWpm, 5, 60);
    if (!open_) return;
    const int range = std::clamp(maxWpm - potMin_, 1, 63);
    const char cmd[] = {0x05, char(potMin_), char(range), 0x00};
    ser_->write(cmd, 4);
    const char getPot[] = {0x07};          // prime with the knob's position
    ser_->write(getPot, 1);
}

// ── Transmitted-CW shaping ────────────────────────────────────────────
// One shape for all of these: clamp to the datasheet's range, remember
// that we now own the parameter, and write it if the port is up. Ranges
// and the meaning of each default are WK3 datasheet Rev 1.3 pp8-14.

// "A value of 50 selects no weighting adjustment. Values less than 50
// reduce weighting and values greater than 50 increase weighting...
// Reduction in weighting results in a thinner sounding keying while
// increased weighting results in a heavier sound." Weighting tracks
// speed, so one setting sounds the same at every WPM. p8.
void WinKeyer::setWeighting(int pct) {
    weight_ = std::clamp(pct, 10, 90);
    if (!open_) return;
    const char cmd[] = {0x03, char(*weight_)};
    ser_->write(cmd, 2);
}

// "QSK keying on modern transceivers can cause shortening of dit and dah
// elements which is especially noticeable at high speeds... adding a
// uniform length to each dit and dah element." Added time is taken back
// out of the spacing, so the WPM does not change. Unlike weighting this
// is absolute ms, so it does NOT track speed. p13.
void WinKeyer::setKeyComp(int ms) {
    keyComp_ = std::clamp(ms, 0, 250);
    if (!open_) return;
    const char cmd[] = {0x11, char(*keyComp_)};
    ser_->write(cmd, 2);
}

// "Due to a slow receive to transmit changeover time, the first dit or
// dah of a letter sequence can be chopped and reduced in length." Only
// the first element of a transmission is stretched. The datasheet notes
// this is "usually only a noticeable problem at higher CW speeds
// >25 WPM" — which is where the operator runs. p12.
void WinKeyer::setFirstExtension(int ms) {
    firstExt_ = std::clamp(ms, 0, 250);
    if (!open_) return;
    const char cmd[] = {0x10, char(*firstExt_)};
    ser_->write(cmd, 2);
}

// DAH/DIT = 3*(nn/50): 50 = a textbook 1:3, 33 = 1:2, 66 = 1:4. The
// datasheet's own warning is worth repeating in the UI — "some ops use
// this option to make their CW sound less machine like but a little
// goes a long way". p14.
void WinKeyer::setRatio(int nn) {
    ratio_ = std::clamp(nn, 33, 66);
    if (!open_) return;
    const char cmd[] = {0x17, char(*ratio_)};
    ser_->write(cmd, 2);
}

// 0 = off. The command's own range is 10-99, so anything between is
// snapped rather than sent as an out-of-range byte. p10.
void WinKeyer::setFarnsworth(int wpm) {
    farns_ = wpm <= 0 ? 0 : std::clamp(wpm, 10, 99);
    if (!open_) return;
    const char cmd[] = {0x0D, char(*farns_)};
    ser_->write(cmd, 2);
}

// Both values are in 10 ms units, 0-250. Note the tail the operator
// actually gets is "three dit times + tail x 10 ms" (p8), which is why
// the panel shows the derived number next to the raw one.
void WinKeyer::setPttLeadTail(int leadMs, int tailMs) {
    pttLead_ = std::clamp(leadMs, 0, 250);
    pttTail_ = std::clamp(tailMs, 0, 250);
    if (!open_) return;
    const char cmd[] = {0x04, char(*pttLead_), char(*pttTail_)};
    ser_->write(cmd, 3);
}

// Paddle sensitivity as a percentage of a dit; 50 is one dit time. p13.
void WinKeyer::setSwitchpoint(int pct) {
    switchpoint_ = std::clamp(pct, 10, 90);
    if (!open_) return;
    const char cmd[] = {0x12, char(*switchpoint_)};
    ser_->write(cmd, 2);
}

// WK1/WK2 sidetone is a TABLE index, not a frequency: 1=4000 Hz, 2=2000,
// 3=1333, 4=1000, 5=800, 6=666, 7=571, 8=500, 9=444, 10=400 (p7). WK3
// mode instead takes 62500/frequency. We stay in WK1/WK2, so the table
// is what applies — the panel offers those ten and says so.
void WinKeyer::setSidetone(int n) {
    sidetone_ = std::clamp(n, 1, 10);
    if (!open_) return;
    const char cmd[] = {0x01, char(*sidetone_)};
    ser_->write(cmd, 2);
}

// The paddle's own feel: iambic A/B, Ultimatic, bug, paddle swap,
// autospace, CT spacing (p11). Deliberately NOT written unless the
// operator opts in, because WK3's "Get Values" admin command always
// returns 0 — there is no way to read the owner's settings back, so
// writing this is one-way and the console owns it from then on.
void WinKeyer::setModeRegister(int bits) {
    modeReg_ = bits & 0xFF;
    if (!open_) return;
    const char cmd[] = {0x0E, char(*modeReg_)};
    ser_->write(cmd, 2);
}

// Extra inter-character space, 0-15 in 2% steps. Worth having because
// the radio's own keyer measurably runs a 4.2-unit character gap rather
// than the textbook 3 (see OrionKeyer's kCharGapUnits, timed off-air) —
// which is a large part of why it sounds less crowded than a keyer
// sending exact 3-unit gaps.
//
// X1MODE does not exist in WK1 mode, the mode host-open leaves us in, so
// adopting letterspace moves the keyer into WK2 mode. That turns on
// pushbutton status reporting, which onReadyRead has to tell apart from
// keyer status. Never touching this control keeps us in WK1 exactly as
// the console has always run.
void WinKeyer::setLetterspace(int steps) {
    letterspace_ = std::clamp(steps, 0, 15);
    if (!open_) return;
    if (!wk2_) {
        const char wk2[] = {0x00, 0x0B};   // Admin 11: set WK2 mode
        ser_->write(wk2, 2);
        ser_->flush();
        wk2_ = true;
    }
    // WK2 X1MODE puts letterspace in the UPPER nibble (datasheet Table 1;
    // WK3 mode moves it to the lower five bits — another reason to pin
    // ourselves to a known mode rather than guess).
    const char cmd[] = {0x00, 0x0F, char((*letterspace_ & 0x0F) << 4)};
    ser_->write(cmd, 3);
}

void WinKeyer::applyOwned() {
    if (!open_) return;
    // Order follows the datasheet's own Load Defaults list so anything
    // interdependent lands the way the keyer expects.
    if (modeReg_)     setModeRegister(*modeReg_);
    if (sidetone_)    setSidetone(*sidetone_);
    if (weight_)      setWeighting(*weight_);
    if (pttLead_ || pttTail_)
        setPttLeadTail(pttLead_.value_or(0), pttTail_.value_or(0));
    if (keyComp_)     setKeyComp(*keyComp_);
    if (farns_)       setFarnsworth(*farns_);
    if (switchpoint_) setSwitchpoint(*switchpoint_);
    if (ratio_)       setRatio(*ratio_);
    if (firstExt_)    setFirstExtension(*firstExt_);
    if (letterspace_) setLetterspace(*letterspace_);
}

void WinKeyer::send(const QString& text) {
    if (!open_) return;
    QByteArray out;
    for (QChar qc : text.toUpper()) {
        const char c = qc.toLatin1();
        // Exactly the keyer's own ASCII table (WK3 datasheet p17 "ASCII
        // Code Assignments, Prosign Mapping").
        //
        // This list used to be hand-written and disagreed with the
        // datasheet both ways: it BLOCKED '<' (AR), '>' (SK), '$' (SX) and
        // '|' (the half-dit pad), and it PASSED '!' which the keyer maps
        // to null and silently drops. The operator went looking for '<'
        // and '>' as the AR/SK keys — which is exactly what they are —
        // and the console was eating them.
        //
        // Omitted always: ! # % & * map to null on the keyer and vanish.
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '
            || c == '.' || c == ',' || c == '?' || c == '/' || c == ':'
            || c == ';' || c == '<' || c == '=' || c == '>' || c == '('
            || c == ')' || c == '+' || c == '-' || c == '\'' || c == '"'
            || c == '@' || c == '$' || c == '|')
            out.append(c);
        // '[' AS, '\' DN, ']' KN exist only on WK3 silicon; older chips
        // ignore them, so they used to be blocked outright "because a
        // WinKeyer USB may be either and we never read back the firmware
        // revision to tell". We read it now (host open answers with the
        // major rev, 3x = WK3), so on hardware that HAS them they are no
        // longer swallowed — and on a WK2 they stay blocked rather than
        // being silently dropped by the keyer with no clue why.
        else if (isWk3() && (c == '[' || c == '\\' || c == ']'))
            out.append(c);
    }
    if (out.isEmpty()) return;
    ser_->write(out);
    if (!busy_) { busy_ = true; emit busyChanged(true); }
}

// 0x1B bonds the next two characters into one prosign (WK3 datasheet
// p15, "Merge Letters", unchanged since WK1). Always merge rather than
// leaning on the keyer's native ASCII table: that table differs between
// WK2 and WK3, and the cwdaemon convention our clients speak disagrees
// with it anyway ('<' is SK to cwdaemon, AR to the keyer).
void WinKeyer::sendProsign(char a, char b) {
    if (!open_) return;
    const char cmd[] = {0x1B, char(QChar(a).toUpper().toLatin1()),
                              char(QChar(b).toUpper().toLatin1())};
    ser_->write(cmd, 3);
    if (!busy_) { busy_ = true; emit busyChanged(true); }
}

void WinKeyer::stop() {
    if (!open_) return;
    const char cmd[] = {0x0A};
    ser_->write(cmd, 1);
    ser_->flush();
}

void WinKeyer::tune(bool on) {
    if (!open_) return;
    const char cmd[] = {0x0B, char(on ? 1 : 0)};
    ser_->write(cmd, 2);
}

void WinKeyer::backspace() {
    if (!open_) return;
    const char cmd[] = {0x08};
    ser_->write(cmd, 1);
}

void WinKeyer::onReadyRead() {
    const QByteArray data = ser_->readAll();
    for (unsigned char b : data) {
        if ((b & 0xC0) == 0xC0) {          // status byte
            // In WK2 mode (which we only enter for letterspace) the keyer
            // also sends PUSHBUTTON status bytes, tagged by bit 3, on
            // every press and release. They share the b110 prefix, so
            // without this they would read as keyer status and flap
            // busy/break-in from a button nobody pressed for CW. In WK1
            // mode bit 3 is KEYDOWN and no pushbutton bytes ever arrive,
            // so the old behaviour is untouched. WK3 datasheet Tables
            // 14-16, p14.
            if (wk2_ && (b & 0x08)) continue;
            const bool busy = b & kStBusy;
            if (b & kStBreakIn) emit breakIn();
            if (busy != busy_) { busy_ = busy; emit busyChanged(busy); }
        } else if ((b & 0xC0) == 0x80) {   // speed pot: offset from pot min
            // Coalesce with a short settle timer. The keyer reports each
            // NEW value exactly once, so the old "same value twice in a
            // row" debounce could never fire on a normal turn — the pot
            // was completely dead (live-found). The timer keeps the
            // original goal (a pot flapping ±1 between detents doesn't
            // chatter the speed) while a real turn lands ~250 ms after
            // the knob stops.
            potPending_ = potMin_ + (b & 0x3F);
            potTimer_->start();
        }
        // anything else would be serial echoback — we never enable it
    }
}

} // namespace ttc
