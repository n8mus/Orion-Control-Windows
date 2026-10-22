// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/WatchList.h"

#include <QRegularExpression>

namespace ttc {

void WatchList::setPatterns(const QString& text) {
    raw_ = text;
    exact_.clear();
    prefixes_.clear();
    any_ = newOnes_ = false;
    n_ = 0;
    static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
    for (QString tok : text.toUpper().split(sep, Qt::SkipEmptyParts)) {
        ++n_;
        if (tok == QLatin1String("*")) any_ = true;
        else if (tok == QLatin1String("NEW")) newOnes_ = true;
        else if (tok.endsWith(QLatin1Char('*'))) {
            tok.chop(1);
            if (!tok.isEmpty()) prefixes_ << tok;
        } else exact_.insert(tok);
    }
}

bool WatchList::matches(const QString& call, const QString& park,
                        QChar status) const {
    if (n_ == 0) return false;
    if (any_) return true;
    if (newOnes_ && status == QLatin1Char('N')) return true;
    const QString c = call.toUpper();
    if (exact_.contains(c)) return true;
    if (!park.isEmpty() && exact_.contains(park.toUpper())) return true;
    for (const QString& p : prefixes_)
        if (c.startsWith(p)) return true;
    return false;
}

} // namespace ttc
