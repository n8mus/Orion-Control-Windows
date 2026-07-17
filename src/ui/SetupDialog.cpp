// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SetupDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
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
                         const QString& liveKeyerDev, bool radioConnected,
                         QWidget* parent)
    : QDialog(parent), liveRadioDev_(liveRadioDev),
      liveKeyerDev_(liveKeyerDev), radioConnected_(radioConnected) {
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
    radioDev_ = new QComboBox(this);
    radioDev_->setEditable(true);          // paths beyond the enumeration
    form->addRow("CAT serial port", radioDev_);
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

    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshPorts();
}

// Serial candidates: everything QSerialPortInfo can see (USB adapters AND
// native COM ports), current settings kept even if not enumerated (device
// may be unplugged right now).
void SetupDialog::refreshPorts() {
    QSettings s;
    QStringList ports;
    for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts())
        ports << p.systemLocation();
    ports.sort();
    const QString curRadio =
        s.value("radio/device",
                model_->currentData() == "omni8" ? "/dev/omni7" : "/dev/orion")
            .toString();
    const QString curKeyer =
        s.value("cw/port",
                "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A904QF5Z-if00-port0")
            .toString();
    for (auto* combo : {radioDev_, keyerDev_}) {
        combo->clear();
        combo->addItems(ports);
    }
    if (!ports.contains(curRadio)) radioDev_->addItem(curRadio);
    if (!ports.contains(curKeyer)) keyerDev_->addItem(curKeyer);
    radioDev_->setCurrentText(curRadio);
    keyerDev_->setCurrentText(curKeyer);
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
    if (model_->currentData() == "omni8") {
        radioTest_->setText("Omni VII probe n/a (binary CAT) — save and "
                            "restart to test");
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
    s.setValue("radio/device", radioDev_->currentText().trimmed());
    s.setValue("cw/port", keyerDev_->currentText().trimmed());
    s.setValue("cw/audioDev", audioDev_->currentText().trimmed());
    s.setValue("spots/host", spotHost_->text().trimmed());
    s.setValue("spots/port", spotPort_->value());
    s.setValue("spots/login", spotLogin_->text().trimmed().toUpper());
    s.setValue("rotor/enabled", rotorOn_->isChecked());
    s.setValue("rotor/port", rotorPort_->value());
    s.setValue("setup/done", true);
    QDialog::accept();
}

} // namespace ttc
