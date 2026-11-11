// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rtty/RttyDemod.h"
#include "rtty/RttyMod.h"
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <vector>

class QProcess;
class QTimer;

namespace ttc {

class PcmSink;

// The console's RTTY modem, one object the main window owns (2026-11-07):
//
//   receive  — RttyDemod on the RADIO's audio (the SignaLink capture the
//              CW reader shares, AudioCwSource's raw tap): the Orion's
//              front end, filter and AGC do the selecting, as they do for
//              CW copy (operator's finding — the SDR IQ loses on air).
//              Decoded text out in lines; calls heard in CQ / DE patterns
//              out as `heard` for the band map.
//   transmit — RttyMod -> pacat into the SignaLink (the DVR's path): AFSK
//              on LSB, mark 2125 / space 2295, 170 Hz shift, 45.45 baud.
//              start() opens the stream and the modulator diddles until
//              text arrives; end() lets the queue play out, adds a mark
//              tail, waits for the pipe to drain and THEN emits txDrained
//              — the main window drops PTT on that, never before (the
//              RADE end-of-over pattern). abort() kills it now (Esc).
//
// The engine keys nothing itself: PTT, DIG and the radio's mode are the
// main window's, as everywhere else.
class RttyEngine : public QObject {
    Q_OBJECT
public:
    struct Settings {
        double baud = 45.45;          // 45.45 / 50 / 75
        int shiftHz = 170;            // 170 / 200 / 425 / 850
        int markHz = 2125;            // audio mark tone (space = mark + shift)
        bool reverse = false;         // swap mark/space (the other sideband)
        bool usos = true;             // unshift on space
        bool diddle = true;           // LTRS idle while keyed with nothing to say
        int txLevelDb = -6;           // AFSK level, dBFS peak (-30..0)
        double squelch = 0.5;         // demod keying-contrast squelch, 0 = off
        bool afc = true;              // the demod's pair search (+-200 Hz)
    };
    static Settings loadSettings();           // rtty/*
    static void saveSettings(const Settings& s);

    explicit RttyEngine(QObject* parent = nullptr);
    ~RttyEngine() override;

    void apply(const Settings& s, bool save = true);   // both directions
    const Settings& settings() const { return s_; }
    // The radio's sideband. On USB the standard signal (mark the higher
    // RF) lands with the mark as the HIGHER audio tone, i.e. reversed
    // against the LSB convention the tones are named for — so USB flips
    // the sense itself; REV is the operator's extra flip on top.
    void setSidebandUsb(bool usb);
    bool sidebandUsb() const { return usb_; }
    // Put the tone pair where the operator clicked in the passband (the
    // fldigi waterfall click): mark = centre - shift/2, not saved — a
    // fresh session starts on the convention again. The AFC trims it.
    void setToneCenterHz(int hz);
    int markHz() const { return s_.markHz; }                 // nominal
    double effectiveMarkHz() const;                          // + AFC
    void resetAfc() { demod_.resetAfc(); }

    // ---- receive ----
    void setRxEnabled(bool on);
    bool rxEnabled() const { return rxOn_; }
    void feedAudio(const float* s, int n);    // 48 kHz mono, any thread-safe caller's thread = ours
    RttyDemod::Status rxStatus() const { return demod_.status(); }
    void setMyCall(const QString& c) { myCall_ = c.trimmed().toUpper(); }

    // ---- transmit ----
    bool start(const QString& sinkNode);      // pacat up, diddling
    void send(const QString& text);           // queue text (keyed as it comes)
    void end();                               // play out, tail, then txDrained
    void abort();                             // now
    bool running() const;
    bool ending() const { return ending_; }
    size_t pending() const { return mod_.pending(); }

signals:
    void textDecoded(const QString& text);    // characters as they come
    // A call seen in "CQ ... DE CALL" / "CALL1 DE CALL2": the sender, and
    // whether CALL1 was us.
    void heard(const QString& call, bool toMe);
    void txDrained();                         // the last bit is on the air
    void txFailed(const QString& why);
    void txProgress(int pendingChars);        // for the deck's "sent" line

private:
    void onChar(char c);
    void scanLine();
    void pace();                              // timer: keep pacat fed
    void stopProcess();

    Settings s_;
    bool usb_ = false;
    RttyDemod demod_;
    RttyMod mod_;
    bool rxOn_ = false;
    QString line_;                            // the line being decoded
    QString myCall_;
    QString lastHeard_;
    qint64 lastHeardSecs_ = 0;

    QProcess* play_ = nullptr;
    PcmSink* sink_ = nullptr;                 // Windows: in place of pacat
    QTimer* pacer_ = nullptr;
    QElapsedTimer clock_;
    qint64 written_ = 0;                      // samples handed to pacat
    bool ending_ = false, tailDone_ = false;
    qint64 tailSamples_ = 0;
    std::vector<float> fbuf_;
    std::vector<int16_t> sbuf_;
};

}  // namespace ttc
