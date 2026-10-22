// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QtMath>

namespace ttc {
namespace bearing {

// Initial great-circle bearing, degrees true (0..360), east-positive lon.
inline double initialDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = qDegreesToRadians(lat1), p2 = qDegreesToRadians(lat2);
    const double dl = qDegreesToRadians(lon2 - lon1);
    const double y = qSin(dl) * qCos(p2);
    const double x = qCos(p1) * qSin(p2) - qSin(p1) * qCos(p2) * qCos(dl);
    double b = qRadiansToDegrees(qAtan2(y, x));
    if (b < 0) b += 360.0;
    return b;
}

// Great-circle distance, km.
inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = qDegreesToRadians(lat1), p2 = qDegreesToRadians(lat2);
    const double dp = p2 - p1;
    const double dl = qDegreesToRadians(lon2 - lon1);
    const double a = qSin(dp / 2) * qSin(dp / 2)
                   + qCos(p1) * qCos(p2) * qSin(dl / 2) * qSin(dl / 2);
    return 6371.0 * 2.0 * qAtan2(qSqrt(a), qSqrt(1.0 - a));
}

} // namespace bearing
} // namespace ttc
