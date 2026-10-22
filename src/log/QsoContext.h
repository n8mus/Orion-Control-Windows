// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

namespace ttc {

// The station being worked RIGHT NOW, shared by every window that has an
// opinion about it (2026-10-19). Before this the LOG window and the CW
// window each kept their own idea of the DX: a call typed in one never
// reached the other, and the RST you just took down lived nowhere the CW
// macros could see it.
//
// One record, two rules: a setter that changes nothing emits nothing (so
// LOG and CW can mirror each other without a feedback loop), and the
// window the operator is typing in is always the writer. Cleared when the
// QSO is logged or the form is wiped.
class QsoContext : public QObject {
    Q_OBJECT
public:
    explicit QsoContext(QObject* parent = nullptr) : QObject(parent) {}

    const QString& call() const { return call_; }
    const QString& handle() const { return handle_; }   // the on-air name
    const QString& name() const { return name_; }       // the callbook's
    const QString& qth() const { return qth_; }
    const QString& grid() const { return grid_; }
    const QString& rstSent() const { return rstSent_; }
    const QString& rstRcvd() const { return rstRcvd_; }

    void setCall(const QString& v)    { set(call_, v.trimmed().toUpper()); }
    void setHandle(const QString& v)  { set(handle_, v.trimmed()); }
    void setName(const QString& v)    { set(name_, v.trimmed()); }
    void setQth(const QString& v)     { set(qth_, v.trimmed()); }
    void setGrid(const QString& v)    { set(grid_, v.trimmed().toUpper()); }
    void setRstSent(const QString& v) { set(rstSent_, v.trimmed()); }
    void setRstRcvd(const QString& v) { set(rstRcvd_, v.trimmed()); }

    void clear() {
        if (call_.isEmpty() && handle_.isEmpty() && name_.isEmpty()
            && qth_.isEmpty() && grid_.isEmpty() && rstSent_.isEmpty()
            && rstRcvd_.isEmpty())
            return;
        call_.clear(); handle_.clear(); name_.clear(); qth_.clear();
        grid_.clear(); rstSent_.clear(); rstRcvd_.clear();
        emit changed();
    }

signals:
    void changed();

private:
    void set(QString& slot, const QString& v) {
        if (slot == v) return;             // no echo, no loop
        slot = v;
        emit changed();
    }

    QString call_, handle_, name_, qth_, grid_, rstSent_, rstRcvd_;
};

} // namespace ttc
