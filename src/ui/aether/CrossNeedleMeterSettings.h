// SPDX-License-Identifier: GPL-3.0-or-later
// From AetherSDR (GPL-3.0), unchanged — the Panel layout's meters
// (src/ui/aether/README).
#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR::CrossNeedleMeterSettingsCodec {

inline constexpr int kVersion = 2;
inline const QString kSettingsKey = QStringLiteral("CrossNeedleMeter");
inline const QString kClassicTheme = QStringLiteral("classic-warm");
inline const QString kUplightTheme = QStringLiteral("dark-room-uplight");
inline const QString kDarkTheme = QStringLiteral("graphite-dark");

struct Snapshot {
    QString faceTheme{kUplightTheme};
    bool showRange{true};
};

QString normalizeTheme(const QString& theme);
QString encode(const Snapshot& settings);
Snapshot decode(const QByteArray& encoded, QString* error = nullptr);
Snapshot migrateLegacyTheme(const QString& theme);

} // namespace AetherSDR::CrossNeedleMeterSettingsCodec
