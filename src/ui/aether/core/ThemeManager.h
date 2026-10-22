// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for AetherSDR's core/ThemeManager — just the calls the ported
// meters (src/ui/aether) make: color(token), applyStyleSheet(widget, a
// template with {{token}} holes), themeChanged, theme::setContainer.
// Tokens resolve from AetherSDR's own Default Dark theme
// (resources/aether/default-dark.json), so the meters wear their real
// colours. One theme, never switched: themeChanged never fires.
#pragma once
#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

class QWidget;

namespace AetherSDR {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();
    QColor color(const QString& token) const;
    QColor color(const QWidget*, const QString& token) const { return color(token); }
    QString value(const QString& token) const;
    // Fills {{token}} holes with their colours and sets the sheet.
    void applyStyleSheet(QWidget* w, const QString& tmpl) const;
    QString resolve(const QString& tmpl) const;

signals:
    void themeChanged();

private:
    ThemeManager();
    QHash<QString, QString> tokens_;    // dotted token -> "#rrggbb" / "#aarrggbb"
};

namespace theme {
inline void setContainer(QWidget*, const QString&) {}   // their scoped themes: not used
}

} // namespace AetherSDR
