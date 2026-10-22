// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for AetherSDR's core/AppSettings: their keys go to our QSettings
// under aether/ (new keys only — nothing of ours is touched).
#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QVariant>

namespace AetherSDR {

class AppSettings {
public:
    static AppSettings& instance() { static AppSettings s; return s; }
    QVariant value(const QString& key, const QVariant& def = {}) const {
        return QSettings().value("aether/" + key, def);
    }
    void setValue(const QString& key, const QVariant& v) { QSettings().setValue("aether/" + key, v); }
    void remove(const QString& key) { QSettings().remove("aether/" + key); }
    bool contains(const QString& key) const { return QSettings().contains("aether/" + key); }
    void save() {}
};

} // namespace AetherSDR
