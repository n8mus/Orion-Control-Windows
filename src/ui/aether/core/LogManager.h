// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for AetherSDR's core/LogManager: the two logging categories the
// ported meters use (warnings only, like theirs).
#pragma once
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcMeters)
Q_DECLARE_LOGGING_CATEGORY(lcGui)
