// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/RigctldServer.h"

#include <QTcpSocket>
#include <cstdio>
#include <cstdlib>

namespace ttc {

static bool rcTrace() {
    static const bool on = std::getenv("TTC_TRACE") != nullptr;
    return on;
}

RigctldServer::RigctldServer(RadioController* radio, QObject* parent)
    : QObject(parent), radio_(radio) {
    connect(&server_, &QTcpServer::newConnection, this, &RigctldServer::onNewConnection);
}

bool RigctldServer::listen(quint16 port) {
    return server_.listen(QHostAddress::Any, port);
}

void RigctldServer::onNewConnection() {
    while (QTcpSocket* s = server_.nextPendingConnection()) {
        connect(s, &QTcpSocket::readyRead, this, &RigctldServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        if (rcTrace())
            std::fprintf(stderr, "[rigctld] client connected: %s\n",
                         s->peerAddress().toString().toLatin1().constData());
        emit clientConnected(s->peerAddress().toString());
    }
}

static const char* modeToHamlib(Mode m) {
    switch (m) {
        case Mode::USB: return "USB";
        case Mode::LSB: return "LSB";
        case Mode::CWU: return "CW";
        case Mode::CWL: return "CWR";
        case Mode::AM:  return "AM";
        case Mode::FM:  return "FM";
    }
    return "USB";
}

// The modes we actually accept, in hamlib spelling. "M <vfo> ?" asks for
// exactly this list.
static const char* const kModeList = "USB LSB CW CWR AM FM";

// Returns false for anything we do not recognise, so the caller can reject the
// command instead of acting on a guess. This used to fall through to
// Mode::USB, which turned every unrecognised argument into "set upper
// sideband" -- including the "?" of the mode-LIST query, so a client merely
// ASKING which modes exist yanked the radio to USB and its last USB filter.
// (Found via Not1MM, whose Radio init sends "M VFOA ?" once at startup.)
// Never guess a mode.
static bool hamlibToMode(const QByteArray& s, Mode& out) {
    if (s == "USB") { out = Mode::USB; return true; }
    if (s == "LSB") { out = Mode::LSB; return true; }
    if (s == "CW")  { out = Mode::CWU; return true; }
    if (s == "CWR") { out = Mode::CWL; return true; }
    if (s == "AM")  { out = Mode::AM;  return true; }
    if (s == "FM")  { out = Mode::FM;  return true; }
    return false;
}

void RigctldServer::onReadyRead() {
    auto* s = qobject_cast<QTcpSocket*>(sender());
    if (!s) return;
    while (s->canReadLine()) {
        QByteArray line = s->readLine().trimmed();
        if (line.isEmpty()) continue;
        emit commandReceived(QString::fromLatin1(line));
        QByteArray reply = handleLine(line);
        if (rcTrace()) {
            std::fprintf(stderr, "[rigctld<-] %s\n", line.constData());
            QByteArray shown = reply;
            shown.replace('\n', "\\n");
            std::fprintf(stderr, "[rigctld->] %s\n", shown.constData());
        }
        if (!reply.isNull()) {
            s->write(reply);
            s->flush();
        }
    }
}

// ---- Hamlib "extended response" protocol -----------------------------------
// cqrlog prefixes every command with '+' and expects replies of the form
//   <command echo>:\n<Name: value>...\nRPRT 0\n
// and sends compound polls as one line ("+f +m"). It also runs an init
// handshake: +\chk_vfo, then +\dump_caps, scanning the caps text for
// "Can get X: Y/N" flags that gate everything it does afterwards.

QByteArray RigctldServer::handleExtended(const QByteArray& cmd,
                                         const QList<QByteArray>& args) {
    QByteArray name = cmd;
    if (name.startsWith('\\')) name.remove(0, 1);

    // Canonical long names for the single-letter forms cqrlog polls with.
    if (name == "f") name = "get_freq";
    else if (name == "m") name = "get_mode";
    else if (name == "v") name = "get_vfo";
    else if (name == "t") name = "get_ptt";
    else if (name == "i") name = "get_split_freq";
    else if (name == "s") name = "get_split_vfo";
    else if (name == "F") name = "set_freq";
    else if (name == "M") name = "set_mode";
    else if (name == "T") name = "set_ptt";
    else if (name == "V") name = "set_vfo";

    QByteArray argEcho;
    for (const QByteArray& a : args) argEcho += " " + a;
    QByteArray r = name + ":" + argEcho + "\n";

    // cqrlog ignores "currVFO"/VFO arguments; so do we (one radio, one VFO).
    QList<QByteArray> a = args;
    while (!a.isEmpty() && (a[0] == "currVFO" || a[0].startsWith("VFO")))
        a.removeFirst();

    if (name == "chk_vfo") {
        r += "ChkVFO: 0\n";                         // no --vfo mode
    } else if (name == "dump_caps") {
        r += "Model name:\tOrion III SDR Console\n"
             "Mfg name:\tN8EM\n"
             "Backend version:\t1.0\n"
             "Rig type:\tTransceiver\n"
             "Can set Frequency:\tY\n"
             "Can get Frequency:\tY\n"
             "Can set Mode:\tY\n"
             "Can get Mode:\tY\n"
             "Can set VFO:\tY\n"
             "Can get VFO:\tN\n"
             "Can set PTT:\tY\n"
             "Can get PTT:\tY\n"
             "Can get Split VFO:\tY\n"
             "Can get Split Freq:\tY\n"
             "Can set Func:\tN\n"
             "Can get Func:\tN\n"
             "Can set Level:\tN\n"
             "Can get Level:\tN\n"
             "Can set Param:\tN\n"
             "Can get Param:\tN\n"
             "Can ctl Mem/VFO:\tN\n"
             "Can send Morse:\tN\n"
             "Can send Voice:\tN\n"
             "Can get power2mW:\tN\n"
             "Can get mW2power:\tN\n";
    } else if (name == "get_freq") {
        r += "Frequency: " + QByteArray::number(qulonglong(freqHz_)) + "\n";
    } else if (name == "get_mode") {
        r += QByteArray("Mode: ") + modeToHamlib(mode_) + "\n"
             "Passband: " + QByteArray::number(bwHz_) + "\n";
    } else if (name == "get_vfo") {
        r += "VFO: VFOA\n";
    } else if (name == "get_ptt") {
        r += QByteArray("PTT: ") + (ptt_ ? "1" : "0") + "\n";
    } else if (name == "get_split_freq") {
        r += "TX Frequency: " + QByteArray::number(qulonglong(freqHz_)) + "\n";
    } else if (name == "get_split_vfo") {
        r += "Split: 0\nTX VFO: VFOA\n";
    } else if (name == "set_freq") {
        if (!a.isEmpty()) {
            bool ok = false;
            const uint64_t hz = static_cast<uint64_t>(a[0].toDouble(&ok));
            if (ok && hz >= 100000 && hz <= 60000000) {
                freqHz_ = hz;
                if (radio_) radio_->setFrequencyHz(Rx::Main, hz);
            } else {
                return r + "RPRT -1\n";             // reject; never zero the rig
            }
        }
    } else if (name == "set_mode") {
        if (!a.isEmpty()) {
            if (a[0] == "?") {                      // mode-LIST query, not a set
                r += QByteArray(kModeList) + "\n";  // unlabelled line = the list
            } else {
                Mode m;
                if (!hamlibToMode(a[0], m))
                    return r + "RPRT -1\n";         // unknown mode: touch nothing
                mode_ = m;
                if (radio_) radio_->setMode(Rx::Main, mode_);
                if (a.size() >= 2 && a[1].toInt() > 0) {
                    bwHz_ = a[1].toInt();
                    if (radio_) radio_->setBandwidthHz(Rx::Main, bwHz_);
                }
            }
        }
    } else if (name == "set_ptt") {
        if (!a.isEmpty()) {
            ptt_ = a[0].toInt() != 0;
            emit pttRequested(ptt_);
        }
    } else if (name == "set_vfo" || name == "set_rit" || name == "set_xit"
               || name == "set_func" || name == "set_level" || name == "set_parm"
               || name == "vfo_op" || name == "set_powerstat") {
        // Accepted no-ops: enough for cqrlog's RIT-clear/tuner/power buttons
        // not to error out. (set_powerstat echo is scanned by cqrlog.)
    } else {
        return r + "RPRT -11\n";                    // unknown extended command
    }
    return r + "RPRT 0\n";
}

// Returns the bytes to send back. "RPRT 0\n" = success; "RPRT -11\n" = unimplemented.
QByteArray RigctldServer::handleLine(const QByteArray& line) {
    // Extended-protocol lines start with '+'; compound polls pack several
    // commands on one line ("+f +m"). Split and answer each in order.
    if (line.startsWith('+')) {
        QByteArray reply;
        QByteArray cmd;
        QList<QByteArray> args;
        const QList<QByteArray> tok = line.split(' ');
        for (const QByteArray& t : tok) {
            if (t.isEmpty()) continue;
            if (t.startsWith('+')) {
                if (!cmd.isEmpty()) reply += handleExtended(cmd, args);
                cmd = t.mid(1);
                args.clear();
            } else {
                args.append(t);
            }
        }
        if (!cmd.isEmpty()) reply += handleExtended(cmd, args);
        return reply;
    }

    const char c = line[0];
    // Strip the command letter, then drop any VFO token hamlib may prepend
    // ("F VFOA 7074000"). The remaining args are the real payload — parsing a
    // VFO name as a number would otherwise send the radio to 0 Hz.
    QList<QByteArray> tok = line.mid(1).split(' ');
    while (!tok.isEmpty() && (tok[0].isEmpty() || tok[0] == "currVFO"
                              || tok[0].startsWith("VFO") || tok[0] == "Main"
                              || tok[0] == "Sub"))
        tok.removeFirst();

    switch (c) {
        case 'f':                                   // get frequency
            return QByteArray::number(static_cast<qulonglong>(freqHz_)) + "\n";
        case 'F':                                   // set frequency
            if (!tok.isEmpty()) {
                bool ok = false;
                const uint64_t hz = static_cast<uint64_t>(tok[0].toDouble(&ok));
                if (ok && hz >= 100000 && hz <= 60000000) {
                    freqHz_ = hz;
                    if (radio_) radio_->setFrequencyHz(Rx::Main, hz);
                } else {
                    return "RPRT -1\n";             // reject; never zero the rig
                }
            }
            return "RPRT 0\n";
        case 'm':                                   // get mode + passband
            return QByteArray(modeToHamlib(mode_)) + "\n" + QByteArray::number(bwHz_) + "\n";
        case 'M':                                   // set mode [passband], or "?" = list
            if (!tok.isEmpty()) {
                if (tok[0] == "?")                  // query: report, never set
                    return QByteArray(kModeList) + "\n";
                Mode m;
                if (!hamlibToMode(tok[0], m))
                    return "RPRT -1\n";             // unknown mode: touch nothing
                mode_ = m;
                if (radio_) radio_->setMode(Rx::Main, mode_);
                if (tok.size() >= 2 && tok[1].toInt() > 0) {
                    bwHz_ = tok[1].toInt();
                    if (radio_) radio_->setBandwidthHz(Rx::Main, bwHz_);
                }
            }
            return "RPRT 0\n";
        case 'v':                                   // get current VFO
            return "VFOA\n";
        case 'V':                                   // set VFO (single-VFO rig)
            return "RPRT 0\n";
        case 't':                                   // get PTT (cache tracks T/R
            return ptt_ ? "1\n" : "0\n";            // via the metering replies)
        case 'T':                                   // set PTT
            if (!tok.isEmpty()) {
                const bool on = tok[0].toInt() != 0;
                ptt_ = on;                          // optimistic; polls confirm
                emit pttRequested(on);
            }
            return "RPRT 0\n";
        default: break;
    }

    // Modern (4.x) rigctld answers a bare protocol-version digit here; hamlib
    // 4.7 netrigctl_open chokes on the old "CHKVFO 0" form.
    if (line.startsWith("\\chk_vfo")) return "0\n";
    if (line.startsWith("\\dump_state")) {
        // Mirrors the structure a real rigctld 4.7.1 emits (protocol 1, ends
        // with key=value lines and "done") — hamlib reads this field-by-field
        // at connect and hangs if anything is missing. Mode mask 0xaf =
        // AM|CW|USB|LSB|FM|CWR; VFO mask 0x3 = VFOA|VFOB.
        return "1\n"                                 // protocol version
               "2\n"                                 // rig model (NET rigctl)
               "2\n"                                 // ITU region
               "100000.000000 60000000.000000 0xaf -1 -1 0x3 0x1\n"   // RX range
               "0 0 0 0 0 0 0\n"
               "1800000.000000 54000000.000000 0xaf 5000 100000 0x3 0x1\n"  // TX
               "0 0 0 0 0 0 0\n"
               "0xaf 1\n"                            // tuning step: 1 Hz, all modes
               "0 0\n"
               "0x2c 2400\n0x82 500\n0xaf 100\n0xaf 6000\n"           // filters
               "0 0\n"
               "9990\n"                              // max RIT
               "9990\n"                              // max XIT
               "8000\n"                              // max IF shift
               "0\n"                                 // announces
               "10\n"                                // preamp list (*RME)
               "6 12 18\n"                           // attenuator list
               "0x0\n0x0\n0x0\n0x0\n0x0\n0x0\n"      // get/set func, level, parm
               "vfo_ops=0x0\n"
               "ptt_type=0x1\n"
               "targetable_vfo=0\n"
               "has_set_vfo=1\n"
               "has_get_vfo=1\n"
               "has_set_freq=1\n"
               "has_get_freq=1\n"
               "has_set_conf=0\n"
               "has_get_conf=0\n"
               "has_power2mW=0\n"
               "has_mW2power=0\n"
               "has_get_ant=0\n"
               "has_set_ant=0\n"
               "timeout=0\n"
               "rig_model=2\n"
               "rigctld_version=Hamlib 4.7.1\n"
               "done\n";
    }
    if (line.startsWith("\\get_powerstat")) return "1\n";
    if (line.startsWith("\\set_powerstat")) return "RPRT 0\n";
    if (c == 's')                                   // get split vfo
        return "0\nVFOA\n";
    if (c == 'i')                                   // get split (TX) frequency
        return QByteArray::number(static_cast<qulonglong>(freqHz_)) + "\n";
    if (c == 'j' || c == 'z')                       // get RIT / XIT
        return "0\n";
    if (c == 'S' || c == 'I' || c == 'J' || c == 'Z')
        return "RPRT 0\n";                          // split/RIT/XIT sets: accept
    if (c == 'q' || c == 'Q') return "RPRT 0\n";    // acknowledged; client closes

    return "RPRT -11\n";                            // unimplemented
}

} // namespace ttc
