// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

namespace ttc {

// One keying engine behind one interface.
//
// The console has always keyed through a K1EL WinKeyer. The Orion's own
// keyer can do the job too (CAT "/c" character send) for an operator who
// doesn't own one — but the two are mutually exclusive AT THE HARDWARE:
// enabling the internal keyer makes the radio read the front KEY jack as
// a paddle, so a WinKeyer wired there reads as a held dit and runs away
// keying dits on the air. Whoever switches backends owns that ordering
// (close our serial port BEFORE *CK1; *CK0 BEFORE reopening it).
//
// Capabilities are declared, not assumed. The CW window gates its own
// controls on them — a backend that cannot unsend a character must not
// offer Live keys — so teaching the console a third keyer means filling
// in a struct, not hunting down every "if WinKeyer" in the UI.
class CwKeyer : public QObject {
    Q_OBJECT
public:
    struct Caps {
        const char* name = "none";   // names the window and the status line
        bool backspace   = false;    // can unsend a not-yet-keyed character
        bool speedPot    = false;    // a physical knob reports back to us
        bool breakIn     = false;    // tells us when the paddle interrupts
        bool tune        = false;    // steady key-down for tuning
        int  wpmMin      = 5;
        int  wpmMax      = 99;
    };

    explicit CwKeyer(QObject* parent = nullptr) : QObject(parent) {}

    virtual const Caps& caps() const = 0;
    // Opens from its own settings, so the window never has to know what a
    // given backend needs (a serial port, a radio, nothing at all).
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString lastError() const = 0;

    virtual void setSpeed(int wpm) = 0;
    virtual void send(const QString& text) = 0;
    virtual void stop() = 0;             // dump everything, key up NOW
    virtual void tune(bool on) = 0;
    virtual void backspace() = 0;        // no-op unless caps().backspace

signals:
    void potChanged(int wpm);            // physical speed pot moved
    void busyChanged(bool sending);
    void breakIn();                      // paddle touched — buffer dumped
    void errorOccurred(const QString& msg);
};

// "No keyer": what a station with neither a WinKeyer nor a CAT-keying
// radio gets. Exists so the CW window always has something to talk to —
// twenty call sites guarded by null checks would be twenty chances to
// key by accident.
class NullKeyer : public CwKeyer {
    Q_OBJECT
public:
    explicit NullKeyer(QObject* parent = nullptr) : CwKeyer(parent) {}
    const Caps& caps() const override { return kCaps; }
    bool open() override { return false; }
    void close() override {}
    bool isOpen() const override { return false; }
    QString lastError() const override {
        return "no keyer selected (SDR ▸ Station setup…)";
    }
    void setSpeed(int) override {}
    void send(const QString&) override {}
    void stop() override {}
    void tune(bool) override {}
    void backspace() override {}

    static constexpr Caps kCaps{"no keyer", false, false, false, false, 5, 99};
};

} // namespace ttc
