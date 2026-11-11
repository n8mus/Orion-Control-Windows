// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Which desktop logger the console keeps in step with: cqrlog on Linux,
// HRD Logbook on Windows (operator, 2026-11-11: HRD is the Windows
// station's logger). Both sync classes share one public shape, so the
// rest of the console names neither.
#include <QtGlobal>

#ifdef Q_OS_WIN
#include "log/HrdSync.h"
namespace ttc { using StationSync = HrdSync; }
#define TTC_STATION_LOG "HRD Logbook"
#define TTC_STATION_LOG_SHORT "HRD"
#else
#include "log/CqrlogSync.h"
namespace ttc { using StationSync = CqrlogSync; }
#define TTC_STATION_LOG "cqrlog"
#define TTC_STATION_LOG_SHORT "cqrlog"
#endif
