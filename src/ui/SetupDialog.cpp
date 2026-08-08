// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SetupDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHostAddress>
#include <QMap>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QUdpSocket>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

namespace ttc {

namespace {
QLabel* section(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(
        "color: #6aa5d8; font-weight: bold; padding-top: 10px;");
    return l;
}

// PipeWire/Pulse capture sources by name — the CW window's RADIO source
// (the radio's line-out into a USB codec). Listed so a tester picks
// their interface instead of inheriting this station's SignaLink node.
QStringList audioSources() {
    QProcess p;
    p.start("pactl", {"list", "short", "sources"});
    if (!p.waitForFinished(2000)) return {};
    QStringList out;
    for (const QString& line :
         QString::fromUtf8(p.readAllStandardOutput()).split('\n'))
        if (line.contains("alsa_input"))
            out << line.section('\t', 1, 1);
    return out;
}
} // namespace

SetupDialog::SetupDialog(const QString& liveRadioDev,
                         const QString& liveKeyerDev,
                         const QString& liveMeterDev, bool radioConnected,
                         QWidget* parent)
    : QDialog(parent), liveRadioDev_(liveRadioDev),
      liveKeyerDev_(liveKeyerDev), liveMeterDev_(liveMeterDev),
      radioConnected_(radioConnected) {
    setWindowTitle("Station setup");
    setStyleSheet(
        "QDialog { background: #141b24; }"
        "QLabel { color: #c8d4e0; font-size: 14px; }"
        "QLineEdit, QComboBox, QSpinBox { background: #1c2430; color: #c8d4e0;"
        " border: 1px solid #2a3644; border-radius: 3px; padding: 4px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }"
        "QCheckBox { color: #c8d4e0; spacing: 8px; }"
        "QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid"
        " #2a3644; border-radius: 3px; padding: 5px 14px; }"
        "QPushButton:hover { background: #2a3644; }");

    QSettings s;
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    lay->addLayout(form);

    form->addRow(section("STATION", this));
    call_ = new QLineEdit(s.value("station/callsign", "N8EM").toString(), this);
    call_->setMaxLength(12);
    form->addRow("Callsign", call_);
    grid_ = new QLineEdit(s.value("station/grid", "EN83al").toString(), this);
    grid_->setMaxLength(6);
    grid_->setToolTip("Maidenhead grid square (4 or 6 chars) — centers the "
                      "compass rose and bearing math");
    form->addRow("Grid square", grid_);

    form->addRow(section("RADIO  (takes effect on next launch)", this));
    model_ = new QComboBox(this);
    model_->addItem("USS Orion  (Ten-Tec 565)", "orion");
    model_->addItem("USS Orion II  (Ten-Tec 566)", "orion2");
    model_->addItem("USS Omni VII  (Ten-Tec 588)", "omni8");
    const QString m = s.value("radio/model", "orion").toString();
    model_->setCurrentIndex(m == "omni8" || m == "omni7" ? 2
                            : m == "orion2"              ? 1
                                                         : 0);
    form->addRow("Model", model_);

    // Per-radio device profiles: the Orion's serial port (deviceOrion), the
    // Omni VII's serial port (deviceSerial) and the Omni VII's Ethernet
    // address (deviceRemote) are three separate remembered values — switching
    // radios or Omni serial<->remote never stomps another radio's device.
    // The Connection selector applies to the Omni only (the Orion is
    // serial-only).
    // Unset means "not configured yet" — the port combos list what this
    // machine actually has. Defaults naming one developer's hardware only
    // preselect a path that exists on a single station.
    const QString legacyDev = s.value("radio/device").toString();
    const bool legacyRemote = legacyDev.startsWith("udp:");
    const bool legacyOrion =
        s.value("radio/model", "orion").toString().startsWith("orion");
    devSerial_ = s.value("radio/deviceSerial",
                         legacyRemote || legacyOrion ? QString() : legacyDev)
                     .toString();
    devRemote_ = s.value("radio/deviceRemote",
                         legacyRemote ? legacyDev : QString())
                     .toString();
    devOrion_ = s.value("radio/deviceOrion",
                        legacyOrion && !legacyRemote ? legacyDev : QString())
                    .toString();
    connMode_ = s.value("radio/connection",
                        legacyRemote ? "remote" : "serial").toString();

    conn_ = new QComboBox(this);
    conn_->addItem("At the radio — serial (CAT cable)", "serial");
    conn_->addItem("Remote — Ethernet (One Plug / laptop)", "remote");
    conn_->setCurrentIndex(connMode_ == "remote" ? 1 : 0);
    conn_->setToolTip("Serial for operating at the desk; Remote streams CAT\n"
                      "and RX audio over Ethernet (radio in REMOTE mode).\n"
                      "Applies to the Omni VII — the Orion is serial-only.");
    form->addRow("Connection", conn_);

    radioDev_ = new QComboBox(this);
    radioDev_->setEditable(true);          // paths / udp: strings beyond enum
    form->addRow("CAT serial port", radioDev_);
    devLabel_ = qobject_cast<QLabel*>(form->labelForField(radioDev_));
    connect(conn_, &QComboBox::currentTextChanged, this,
            [this] { applyConnMode(activeProfile()); });
    connect(model_, &QComboBox::currentTextChanged, this,
            [this] { applyConnMode(activeProfile()); });
    auto* rtest = new QPushButton("Test", this);
    radioTest_ = new QLabel(this);
    auto* rrow = new QHBoxLayout;
    rrow->addWidget(rtest);
    rrow->addWidget(radioTest_, 1);
    form->addRow("", rrow);
    connect(rtest, &QPushButton::clicked, this, &SetupDialog::testRadio);

    form->addRow(section("CW KEYER", this));
    keyerDev_ = new QComboBox(this);
    keyerDev_->setEditable(true);
    form->addRow("WinKeyer port", keyerDev_);
    auto* ktest = new QPushButton("Test", this);
    keyerTest_ = new QLabel(this);
    auto* krow = new QHBoxLayout;
    krow->addWidget(ktest);
    krow->addWidget(keyerTest_, 1);
    form->addRow("", krow);
    connect(ktest, &QPushButton::clicked, this, &SetupDialog::testKeyer);
    audioDev_ = new QComboBox(this);
    audioDev_->setEditable(true);
    audioDev_->setToolTip(
        "Capture device carrying the radio's RX audio (SignaLink or any\n"
        "USB codec) — feeds the CW window's RADIO source and pitch meter");
    for (const QString& a : audioSources()) audioDev_->addItem(a);
    audioDev_->setEditText(
        s.value("cw/audioDev",
                "alsa_input.usb-BurrBrown_from_Texas_Instruments_USB_AUDIO_"
                "CODEC-00.analog-stereo").toString());
    form->addRow("Radio audio in", audioDev_);

    form->addRow(section("DX CLUSTER", this));
    spotHost_ = new QLineEdit(s.value("spots/host", "dxc.ve7cc.net").toString(), this);
    form->addRow("Node", spotHost_);
    spotPort_ = new QSpinBox(this);
    spotPort_->setRange(1, 65535);
    spotPort_->setValue(s.value("spots/port", 23).toInt());
    form->addRow("Port", spotPort_);
    spotLogin_ = new QLineEdit(
        s.value("spots/login", s.value("station/callsign", "N8EM").toString())
            .toString(), this);
    spotLogin_->setToolTip("Cluster login — normally your callsign");
    form->addRow("Login", spotLogin_);

    form->addRow(section("ROTOR", this));
    rotorOn_ = new QCheckBox("rotctld rotor control", this);
    rotorOn_->setChecked(s.value("rotor/enabled", false).toBool());
    form->addRow("", rotorOn_);
    rotorPort_ = new QSpinBox(this);
    rotorPort_->setRange(1, 65535);
    rotorPort_->setValue(s.value("rotor/port", 4533).toInt());
    form->addRow("rotctld port", rotorPort_);

    // RF wattmeter. Off by default: a station without an LP-100A must get
    // exactly the behaviour it had before this section existed, which means
    // the sweep keeps reading the radio's own SWR.
    form->addRow(section("RF WATTMETER", this));
    lpOn_ = new QCheckBox("External wattmeter on a serial port", this);
    lpOn_->setChecked(
        s.value("meter/enabled", s.value("lp100a/enabled", false)).toBool());
    lpOn_->setToolTip(
        "A calibrated external wattmeter on its own serial port —\n"
        "more, and more accurate, than the radio's built-in metering.");
    form->addRow("", lpOn_);

    lpModel_ = new QComboBox(this);
    lpModel_->addItem("TelePost LP-100A (vector: R+jX, Smith chart)",
                      "lp100a");
    lpModel_->addItem("Array Solutions PowerMaster (SWR + power)",
                      "powermaster");
    lpModel_->setCurrentIndex(
        s.value("meter/model", "lp100a").toString() == "powermaster" ? 1 : 0);
    lpModel_->setToolTip(
        "LP-100A is polled at 115200 and measures complex impedance —\n"
        "sweeps get Smith charts. The PowerMaster streams at 38400 —\n"
        "fast forward/reflected/SWR, sweeps draw SWR curves only.");
    form->addRow("Model", lpModel_);

    lpDev_ = new QComboBox(this);
    lpDev_->setEditable(true);
    lpDev_->setToolTip(
        "Straight-through DB9 cable, NOT a null modem — a null modem\n"
        "reads as a dead port on either meter.");
    form->addRow("Meter serial port", lpDev_);

    lpSwr_ = new QComboBox(this);
    lpSwr_->addItem("LP-100A, falling back to the radio", "meter");
    lpSwr_->addItem("Radio only", "radio");
    lpSwr_->setCurrentIndex(
        s.value("swr/source", "meter").toString() == "radio" ? 1 : 0);
    lpSwr_->setToolTip(
        "Which SWR the right-click TUNE sweep believes.\n"
        "The meter is used only while it is enabled AND answering;\n"
        "if it goes quiet mid-sweep the radio takes over silently.");
    form->addRow("Sweep reads", lpSwr_);

    auto* mtest = new QPushButton("Test", this);
    lpTest_ = new QLabel(this);
    auto* mrow = new QHBoxLayout;
    mrow->addWidget(mtest);
    mrow->addWidget(lpTest_, 1);
    form->addRow("", mrow);
    connect(mtest, &QPushButton::clicked, this, &SetupDialog::testMeter);

    auto syncLpRows = [this] {
        const bool on = lpOn_->isChecked();
        lpModel_->setEnabled(on);
        lpDev_->setEnabled(on);
        lpSwr_->setEnabled(on);
    };
    connect(lpOn_, &QCheckBox::toggled, this, syncLpRows);
    syncLpRows();

    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshPorts();
    applyConnMode(activeProfile());      // fills the radio device row per profile
}

// Probe the selected wattmeter once and decode what came back. The LP-100A
// answers its poll with the callsign programmed into it — the friendliest
// possible "yes, this is the right port". The PowerMaster acknowledges its
// activation command and starts streaming.
void SetupDialog::testMeter() {
    const QString dev = lpDev_->currentText().trimmed();
    const bool pm = lpModel_->currentData().toString() == "powermaster";
    if (!liveMeterDev_.isEmpty() && dev == liveMeterDev_) {
        lpTest_->setText("✓ in use by this console — connected");
        return;
    }
    QSerialPort p;
    p.setPortName(dev);
    p.setBaudRate(pm ? 38400 : 115200);
    p.setDataBits(QSerialPort::Data8);
    p.setParity(QSerialPort::NoParity);
    p.setStopBits(QSerialPort::OneStop);
    p.setFlowControl(QSerialPort::NoFlowControl);
    if (!p.open(QIODevice::ReadWrite)) {
        lpTest_->setText("✗ " + p.errorString());
        return;
    }
    lpTest_->setText("probing…");
    lpTest_->repaint();
    QByteArray got;
    const QByteArray probe = pm
        ? QByteArray("\x02" "D1" "\x03" "C0" "\r" "S\x00", 9)  // activation
        : QByteArrayLiteral(";P?");                            // vendor poll
    for (int attempt = 0; attempt < 3 && got.size() < 40; ++attempt) {
        p.clear();
        got.clear();
        p.write(probe);
        p.flush();
        for (int i = 0; i < 5 && got.size() < 40; ++i)
            if (p.waitForReadyRead(250)) got += p.readAll();
    }
    p.close();
    if (pm) {
        // Expect STX-framed frames; a 'D' payload carries the live power.
        const int d = got.indexOf("\x02" "D,");
        if (d >= 0) {
            const QList<QByteArray> f = got.mid(d + 1).split(',');
            const QString fwd = f.size() > 1
                ? QString::fromLatin1(f[1]).trimmed() : QString();
            lpTest_->setText(fwd.isEmpty()
                ? QStringLiteral("✓ PowerMaster streaming")
                : QStringLiteral("✓ PowerMaster streaming — fwd %1 W").arg(fwd));
        } else if (got.contains('\x02')) {
            lpTest_->setText("✓ PowerMaster acknowledged");
        } else {
            lpTest_->setText("✗ no stream — right port? meter set to 38400? "
                             "straight-through cable?");
        }
        return;
    }
    const int semi = got.indexOf(';');
    if (semi < 0 || got.size() - semi < 43) {
        lpTest_->setText("✗ no answer — is this the LP-100A port?");
        return;
    }
    const QList<QByteArray> f = got.mid(semi + 1, 42).split(',');
    if (f.size() != 9) {
        lpTest_->setText("✗ unrecognized reply");
        return;
    }
    const QString call = QString::fromLatin1(f[4]).trimmed();
    lpTest_->setText(call.isEmpty()
                         ? QStringLiteral("✓ LP-100A answered")
                         : QStringLiteral("✓ LP-100A answered — call %1").arg(call));
}

// Serial candidates: everything QSerialPortInfo can see (USB adapters AND
// native COM ports) — but offered under a STABLE name wherever one exists.
// QSerialPortInfo reports kernel paths, and those renumber whenever the
// hardware set changes: adding a second PCIe serial card renumbers ttyS4-7
// and silently repoints every setting that named one (which once left a
// rotor daemon writing into a transceiver's CAT port). A
// /dev/serial/by-id/ symlink is tied to the adapter's own serial number
// instead, so it survives replugging and re-enumeration. Native PCIe/COM
// ports get no such symlink from the kernel and are listed as-is; a
// station that wants stable names for those needs its own udev rule.
// The combos are editable, so a hand-written path is always still possible.
// The 8250 driver publishes ttyS0..ttyS31 whether or not there is silicon
// behind them, so a station with two real ports would otherwise have to find
// them among thirty phantoms. /sys/class/tty/<port>/type reads 0
// (PORT_UNKNOWN) for a node with no UART; a real one reports its chip (4 =
// 16550A). Ports with no type file at all — USB adapters, virtual pairs —
// are genuine devices and are kept.
static bool phantomSerialNode(const QString& portName) {
    QFile f(QStringLiteral("/sys/class/tty/%1/type").arg(portName));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    return f.readAll().trimmed() == "0";
}

QStringList SetupDialog::serialPortCandidates() {
    QMap<QString, QString> stable;                 // real path -> by-id path
    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    const auto links = byId.entryInfoList(QDir::AllEntries | QDir::System
                                          | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : links) {
        const QString real = fi.canonicalFilePath();
        if (!real.isEmpty()) stable.insert(real, fi.absoluteFilePath());
    }
    QStringList out;
    for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts()) {
        if (phantomSerialNode(p.portName())) continue;
        const QString raw = p.systemLocation();
        const QString real = QFileInfo(raw).canonicalFilePath();
        out << stable.value(real.isEmpty() ? raw : real, raw);
    }
    out.sort();
    out.removeDuplicates();
    return out;
}

// Current settings are kept even if not enumerated (the device may be
// unplugged right now, or be a station's own udev alias). The radio device
// row is owned by applyConnMode (it depends on the connection profile);
// here we handle the keyer and the wattmeter.
void SetupDialog::refreshPorts() {
    QSettings s;
    const QStringList ports = serialPortCandidates();

    // No default device: an unset port means "not configured", and the
    // operator picks from the list. Seeding a developer's own hardware
    // here just preselects a path that exists on exactly one station.
    const auto fill = [&ports](QComboBox* box, const QString& cur) {
        box->clear();
        box->addItems(ports);
        if (!cur.isEmpty() && !ports.contains(cur)) box->addItem(cur);
        box->setCurrentText(cur);
    };
    fill(keyerDev_, s.value("cw/port").toString());
    fill(lpDev_, s.value("meter/device", s.value("lp100a/device")).toString());
}

// Which stored device the field is editing: the Orion's serial port, the
// Omni's serial port, or the Omni's Ethernet address.
QString SetupDialog::activeProfile() const {
    if (model_->currentData().toString().startsWith("orion"))
        return QStringLiteral("orion");
    return conn_->currentData().toString();       // "serial" | "remote"
}

// Swap the device field between the per-radio profiles, remembering the
// field we're leaving so a flip-back is lossless. Serial profiles list the
// enumerated ports; remote offers the saved udp: string (plus this
// station's One Plug default as a hint). Orion models force + grey the
// Connection row (serial-only radio).
void SetupDialog::applyConnMode(const QString& profile) {
    const QString shown = radioDev_->currentText().trimmed();
    if (!shown.isEmpty()) {                       // stash (skip the empty init)
        if (lastProfile_ == "remote")      devRemote_ = shown;
        else if (lastProfile_ == "orion")  devOrion_  = shown;
        else if (lastProfile_ == "serial") devSerial_ = shown;
    }
    lastProfile_ = profile;
    const bool orion = profile == "orion";
    if (!orion) connMode_ = profile;              // the Omni's serial/remote pick
    {                                             // Orion: pin selector to serial
        QSignalBlocker block(conn_);
        if (orion) conn_->setCurrentIndex(0);
        else conn_->setCurrentIndex(connMode_ == "remote" ? 1 : 0);
    }
    conn_->setEnabled(!orion);

    radioDev_->clear();
    if (profile == "remote") {
        if (devLabel_) devLabel_->setText("Radio address");
        radioDev_->setToolTip("udp:HOST[:PORT] — the radio's IP in REMOTE\n"
                              "mode (default CMD port 49152). Same string on\n"
                              "the laptop, pointed at the radio's LAN address.");
        if (!devRemote_.isEmpty()) radioDev_->addItem(devRemote_);
        radioDev_->setCurrentText(devRemote_);   // format is in the tooltip
    } else {
        const QString& cur = orion ? devOrion_ : devSerial_;
        if (devLabel_) devLabel_->setText("CAT serial port");
        radioDev_->setToolTip(orion
            ? "Serial device of the Orion (its only CAT path)."
            : "Serial device this console opens when operating the Omni VII\n"
              "at the desk (front panel / RADIO mode).");
        const QStringList ports = serialPortCandidates();
        radioDev_->addItems(ports);
        if (!cur.isEmpty() && !ports.contains(cur)) radioDev_->addItem(cur);
        radioDev_->setCurrentText(cur);
    }
}

// Live CAT probe: ?V at 57600 8N1 (Orion ASCII dialect — both Orions).
// The Omni VII speaks binary with RTS/CTS; a raw probe would mislead, so
// it gets a hint instead. Probing the port THIS console holds open would
// only collide with our own exclusive lock — report the live connection.
void SetupDialog::testRadio() {
    const QString dev = radioDev_->currentText().trimmed();
    if (dev == liveRadioDev_) {
        radioTest_->setText(radioConnected_
                                ? "✓ in use by this console — connected"
                                : "held by this console, not answering — "
                                  "check cable/power");
        return;
    }
    // Remote (Ethernet): a real UDP ?V probe — the same passcode-framed
    // query the driver uses. Confirms the radio is reachable and in REMOTE
    // mode before committing, which is exactly what you want from the
    // laptop. (Only possible when the console isn't itself holding 49152 —
    // i.e. it launched on a serial device; the dev==liveRadioDev_ branch
    // above already covers the in-use case.)
    if (dev.startsWith("udp:")) {
        const QString spec = dev.mid(4);
        const int colon = spec.lastIndexOf(':');
        const QString host = colon > 0 ? spec.left(colon) : spec;
        const quint16 port = colon > 0 ? quint16(spec.mid(colon + 1).toUInt())
                                       : 49152;
        const quint16 pass =
            quint16(QSettings().value("radio/netPasscode", 0).toUInt());
        QUdpSocket u;
        if (!u.bind(QHostAddress::AnyIPv4, port)) {   // symmetric ports
            radioTest_->setText("✗ can't bind local :" + QString::number(port) +
                                " (another app using it?)");
            return;
        }
        QByteArray q;
        q.append(char(pass >> 8));
        q.append(char(pass & 0xff));
        q.append("?V\r", 3);
        u.writeDatagram(q, QHostAddress(host), port);
        if (u.waitForReadyRead(1200)) {
            QByteArray d(int(u.pendingDatagramSize()), 0);
            u.readDatagram(d.data(), d.size());
            radioTest_->setText("✓ " + QString::fromLatin1(d.simplified()));
        } else {
            radioTest_->setText("✗ no reply — radio in REMOTE mode? "
                                "IP/passcode right? cable to the LAN?");
        }
        return;
    }
    if (model_->currentData() == "omni8") {
        radioTest_->setText("Omni VII serial probe n/a (binary CAT) — save "
                            "and restart to test");
        return;
    }
    QSerialPort p;
    p.setPortName(dev);
    p.setBaudRate(57600);
    if (!p.open(QIODevice::ReadWrite)) {
        radioTest_->setText("✗ " + p.errorString());
        return;
    }
    p.write("?V\r");
    p.flush();
    QByteArray got;
    for (int i = 0; i < 6 && !got.contains('\r'); ++i)
        if (p.waitForReadyRead(250)) got += p.readAll();
    p.close();
    radioTest_->setText(got.isEmpty()
                            ? "✗ no reply — right port? radio on?"
                            : "✓ " + QString::fromLatin1(got.simplified()));
}

// WinKeyer echo probe: three nulls to wake, Admin:Echo of one byte — the
// same sanity check WinKeyer::open() runs, minus the full session setup.
void SetupDialog::testKeyer() {
    const QString dev = keyerDev_->currentText().trimmed();
    if (dev == liveKeyerDev_) {
        keyerTest_->setText("✓ in use by this console — connected");
        return;
    }
    QSerialPort p;
    p.setPortName(dev);
    p.setBaudRate(1200);
    p.setStopBits(QSerialPort::TwoStop);
    if (!p.open(QIODevice::ReadWrite)) {
        keyerTest_->setText("✗ " + p.errorString());
        return;
    }
    p.setDataTerminalReady(true);
    p.setRequestToSend(false);
    keyerTest_->setText("probing…");
    keyerTest_->repaint();
    QThread::msleep(800);                  // WKUSB wake-up after DTR
    p.clear();
    p.write(QByteArray("\x13\x13\x13", 3));
    p.flush();
    QThread::msleep(300);
    p.readAll();
    p.write(QByteArray("\x00\x04\x14", 3));
    p.flush();
    QByteArray got;
    for (int i = 0; i < 8 && !got.contains(char(0x14)); ++i)
        if (p.waitForReadyRead(250)) got += p.readAll();
    p.close();
    keyerTest_->setText(got.contains(char(0x14))
                            ? "✓ WinKeyer answered"
                            : "✗ no echo — is this the WinKeyer port?");
}

void SetupDialog::accept() {
    QSettings s;
    s.setValue("station/callsign", call_->text().trimmed().toUpper());
    s.setValue("station/grid", grid_->text().trimmed());
    s.setValue("radio/model", model_->currentData().toString());
    // Fold the currently-shown field back into its per-radio profile, save
    // all three, and mirror the active one into radio/device (what the app
    // opens at launch; a udp: string also auto-enables RIP audio).
    const QString shown = radioDev_->currentText().trimmed();
    const QString prof = activeProfile();
    if (prof == "remote")      devRemote_ = shown;
    else if (prof == "orion")  devOrion_  = shown;
    else                       devSerial_ = shown;
    s.setValue("radio/connection", connMode_);
    s.setValue("radio/deviceSerial", devSerial_);
    s.setValue("radio/deviceRemote", devRemote_);
    s.setValue("radio/deviceOrion", devOrion_);
    s.setValue("radio/device",
               prof == "orion" ? devOrion_
               : connMode_ == "remote" ? devRemote_ : devSerial_);
    s.setValue("cw/port", keyerDev_->currentText().trimmed());
    s.setValue("cw/audioDev", audioDev_->currentText().trimmed());
    s.setValue("spots/host", spotHost_->text().trimmed());
    s.setValue("spots/port", spotPort_->value());
    s.setValue("spots/login", spotLogin_->text().trimmed().toUpper());
    s.setValue("rotor/enabled", rotorOn_->isChecked());
    s.setValue("rotor/port", rotorPort_->value());
    s.setValue("meter/enabled", lpOn_->isChecked());
    s.setValue("meter/model", lpModel_->currentData().toString());
    s.setValue("meter/device", lpDev_->currentText().trimmed());
    // Mirror into the original keys so a rollback to an older build keeps
    // the meter working (those builds only know lp100a/*).
    s.setValue("lp100a/enabled", lpOn_->isChecked());
    s.setValue("lp100a/device", lpDev_->currentText().trimmed());
    s.setValue("swr/source", lpSwr_->currentData().toString());
    s.setValue("setup/done", true);
    QDialog::accept();
}

} // namespace ttc
