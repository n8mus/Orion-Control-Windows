// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QSpinBox;

namespace ttc {

// First-run / alpha settings: everything a NEW station must point at its
// own hardware before the console makes sense — today those were
// conf-file edits with this station's values as the defaults. Opens
// automatically until completed once (setup/done), afterwards from the
// SDR menu. Radio model/port take effect on the next launch (drivers and
// serial framing are built at startup); the rest applies from settings
// on their normal paths.
class SetupDialog : public QDialog {
    Q_OBJECT
public:
    // liveRadioDev/keyerDev/meterDev: devices this running instance already
    // holds open (exclusively) — the Test buttons report "connected" for
    // those instead of probing into our own lock.
    SetupDialog(const QString& liveRadioDev, const QString& liveKeyerDev,
                const QString& liveMeterDev, bool radioConnected,
                QWidget* parent = nullptr);

    void accept() override;              // persist everything, stamp done

protected:
    // Wheel over a combo/spin box must scroll the page, not edit the value.
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void refreshPorts();
    // Enumerated serial ports, each offered under a stable
    // /dev/serial/by-id name where the kernel provides one (raw path
    // otherwise) — kernel names renumber when the hardware set changes.
    static QStringList serialPortCandidates();
    void testRadio();
    void testKeyer();
    void testMeter();
    // Swap the device field between the per-radio profiles (Orion serial,
    // Omni serial, Omni udp:). Stashes the outgoing field text so flipping
    // back and forth never loses another radio's device.
    void applyConnMode(const QString& profile);
    QString activeProfile() const;       // "orion" | "serial" | "remote"

    QLineEdit* call_ = nullptr;
    QLineEdit* grid_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* conn_ = nullptr;          // Connection: serial | remote
    QComboBox* radioDev_ = nullptr;
    QLabel*    devLabel_ = nullptr;      // the device row's label, relabeled per mode
    QLabel*    radioTest_ = nullptr;
    QComboBox* keyerSel_ = nullptr;   // which keying engine
    QComboBox* keyerDev_ = nullptr;
    QLabel*    keyerTest_ = nullptr;
    QComboBox* audioDev_ = nullptr;
    QLineEdit* spotHost_ = nullptr;
    QSpinBox*  spotPort_ = nullptr;
    QLineEdit* spotLogin_ = nullptr;
    QCheckBox* rotorOn_ = nullptr;
    QComboBox* rotorMode_ = nullptr;     // direct | rotctld
    QComboBox* rotorDev_ = nullptr;      // DCU-3 serial port (direct mode)
    QSpinBox*  rotorPort_ = nullptr;
    QCheckBox* lpOn_    = nullptr;       // external wattmeter present at all
    QComboBox* lpModel_ = nullptr;       // lp100a | powermaster
    QComboBox* lpDev_   = nullptr;
    QComboBox* lpSwr_   = nullptr;       // which SWR the sweep believes
    QLabel*    lpTest_  = nullptr;
    QString    liveRadioDev_, liveKeyerDev_, liveMeterDev_;
    QString    devSerial_, devRemote_;   // Omni VII serial / Ethernet profiles
    QString    devOrion_;                // Orion serial profile
    QString    connMode_;                // the Omni's serial/remote choice
    QString    lastProfile_;             // profile the field currently shows
    bool       radioConnected_ = false;
};

} // namespace ttc
