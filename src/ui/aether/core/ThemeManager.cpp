// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ThemeManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QWidget>

namespace AetherSDR {

namespace {
void flatten(const QJsonObject& o, const QString& prefix, QHash<QString, QString>& out) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = prefix.isEmpty() ? it.key() : prefix + '.' + it.key();
        if (it.value().isString()) out.insert(key, it.value().toString());
        else if (it.value().isObject() && !it.value().toObject().contains("type"))
            flatten(it.value().toObject(), key, out);   // gradients ("type") skipped
    }
}
}  // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager tm;
    return tm;
}

ThemeManager::ThemeManager() {
    QFile f(QStringLiteral(":/aether/default-dark.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QHash<QString, QString> raw;
    flatten(root.value("primitives").toObject(), QString(), raw);
    flatten(root.value("scopes").toObject().value("root").toObject()
                .value("tokens").toObject(), QString(), raw);
    // {alias} references, a few levels deep.
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        QString v = it.value();
        for (int depth = 0; depth < 8 && v.startsWith('{') && v.endsWith('}'); ++depth)
            v = raw.value(v.mid(1, v.size() - 2));
        tokens_.insert(it.key(), v);
    }
}

QString ThemeManager::value(const QString& token) const { return tokens_.value(token); }

QColor ThemeManager::color(const QString& token) const {
    const QString v = tokens_.value(token);
    return v.isEmpty() ? QColor(0x8e, 0xa8, 0xc0) : QColor::fromString(v);
}

QString ThemeManager::resolve(const QString& tmpl) const {
    static const QRegularExpression re(QStringLiteral("\\{\\{([A-Za-z0-9_.]+)\\}\\}"));
    QString out = tmpl;
    for (auto m = re.globalMatch(tmpl); m.hasNext();) {
        const auto hit = m.next();
        const QColor c = color(hit.captured(1));
        out.replace(hit.captured(0), c.alpha() == 255 ? c.name(QColor::HexRgb)
                                                     : QString("rgba(%1,%2,%3,%4)")
                                                           .arg(c.red()).arg(c.green())
                                                           .arg(c.blue()).arg(c.alpha()));
    }
    return out;
}

void ThemeManager::applyStyleSheet(QWidget* w, const QString& tmpl) const {
    if (w) w->setStyleSheet(resolve(tmpl));
}

} // namespace AetherSDR
