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
    // liveRadioDev/keyerDev: devices this running instance already holds
    // open (exclusively) — the Test buttons report "connected" for those
    // instead of probing into our own lock.
    SetupDialog(const QString& liveRadioDev, const QString& liveKeyerDev,
                bool radioConnected, QWidget* parent = nullptr);

    void accept() override;              // persist everything, stamp done

private:
    void refreshPorts();
    void testRadio();
    void testKeyer();
    // Swap the device field between the two saved profiles (serial at the
    // desk, udp: for remote/Ethernet). Stashes the outgoing field text so
    // flipping back and forth never loses the other connection's device.
    void applyConnMode(const QString& mode);

    QLineEdit* call_ = nullptr;
    QLineEdit* grid_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* conn_ = nullptr;          // Connection: serial | remote
    QComboBox* radioDev_ = nullptr;
    QLabel*    devLabel_ = nullptr;      // the device row's label, relabeled per mode
    QLabel*    radioTest_ = nullptr;
    QComboBox* keyerDev_ = nullptr;
    QLabel*    keyerTest_ = nullptr;
    QComboBox* audioDev_ = nullptr;
    QLineEdit* spotHost_ = nullptr;
    QSpinBox*  spotPort_ = nullptr;
    QLineEdit* spotLogin_ = nullptr;
    QCheckBox* rotorOn_ = nullptr;
    QSpinBox*  rotorPort_ = nullptr;
    QString    liveRadioDev_, liveKeyerDev_;
    QString    devSerial_, devRemote_;   // the two remembered profiles
    QString    connMode_;                // which one is currently shown
    bool       radioConnected_ = false;
};

} // namespace ttc
