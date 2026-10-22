// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for AetherSDR's core/KiwiSdrProtocol: only the meter-reading
// types their S-meter's KiwiSDR overload names (copied verbatim). We have
// no KiwiSDR receiver; nothing ever feeds that overload.
#pragma once
#include <QMetaType>
#include <QString>

namespace AetherSDR::KiwiSdrProtocol {

enum class MeterSource {
    Unknown,
    SndMetadata,
    AudioSamples,
    WaterfallBins,
    MsgMetadata,
    ManualTest,
};

enum class MeterCapability {
    Unavailable,
    RelativeAudio,
    RelativeWaterfall,
    RawSndMeter,
    CalibratedSndMeter,
    Experimental,
};

enum class MeterConfidence {
    None,
    Low,
    Medium,
    High,
    Verified,
};

struct MeterReading {
    qint64 timestampUtcMs{0};
    MeterSource source{MeterSource::Unknown};
    MeterCapability capability{MeterCapability::Unavailable};
    float rawValue{0.0f};
    bool hasRawValue{false};
    float dbm{0.0f};
    bool hasDbm{false};
    QString sUnits;
    float relativeLevel{0.0f};
    bool hasRelativeLevel{false};
    bool squelchStateKnown{false};
    bool squelched{false};
    bool valid{false};
    MeterConfidence confidence{MeterConfidence::None};
    QString label{QStringLiteral("Meter unavailable")};
    QString notes;
};

} // namespace AetherSDR::KiwiSdrProtocol

Q_DECLARE_METATYPE(AetherSDR::KiwiSdrProtocol::MeterReading)
