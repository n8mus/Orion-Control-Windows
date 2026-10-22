// SPDX-License-Identifier: GPL-3.0-or-later
// Voice keyer EQ verification (pure DSP + file I/O, no audio devices):
//   cmake --build build --target vkeqtest && ./build/vkeqtest
// - the ported AetherSDR ClientEq matches its own analytic response
// - snapToTargets() puts the very first block on the final curve
// - renderVoiceEq high-passes, normalizes to the target peak, writes a
//   readable WAV, and passes audio through untouched (bar level) when off
// - settings survive JSON
#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/ClipDeck.h"
#include "audio/VoiceEq.h"
#include "dsp/ClientDeEss.h"
#include "dsp/ClientEq.h"
#include "dsp/ClientFinalLimiter.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); ++fails; }
}

static std::vector<float> tone(float hz, int n, float amp = 0.25f, int rate = 48000) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[size_t(i)] = amp * std::sin(2.0 * M_PI * hz * i / rate);
    return x;
}

static double rmsDb(const std::vector<float>& x, size_t from, size_t to) {
    double acc = 0;
    for (size_t i = from; i < to; ++i) acc += double(x[i]) * x[i];
    return 10.0 * std::log10(acc / double(to - from) + 1e-30);
}

// Single-bin DFT power of hz in x (Goertzel would do; this is short).
static double binDb(const std::vector<float>& x, float hz, int rate = 48000) {
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        re += x[i] * std::cos(2.0 * M_PI * hz * double(i) / rate);
        im += x[i] * std::sin(2.0 * M_PI * hz * double(i) / rate);
    }
    return 20.0 * std::log10(std::hypot(re, im) / double(x.size()) + 1e-30);
}

static double gainThrough(ClientEq& eq, float hz) {
    std::vector<float> x = tone(hz, 48000);
    const double in = rmsDb(x, 24000, 48000);
    eq.reset();
    for (size_t o = 0; o < x.size(); o += 512)
        eq.process(x.data() + o, int(std::min<size_t>(512, x.size() - o)), 1);
    return rmsDb(x, 24000, 48000) - in;            // steady-state half
}

int main(int argc, char** argv) {
    // Profiles live in QSettings: keep the test's away from the real config.
    QTemporaryDir cfg;
    qputenv("XDG_CONFIG_HOME", cfg.path().toLocal8Bit());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("vkeqtest");
    QCoreApplication::setApplicationName("vkeqtest");

    // 1. Measured gain == analytic magnitude, for each filter type.
    struct Case { ClientEq::FilterType t; float f0, gain, q; int slope; float probe; };
    const Case cases[] = {
        {ClientEq::FilterType::Peak,      1000, 6,  1.0f, 12, 1000},
        {ClientEq::FilterType::Peak,      300, -3,  1.0f, 12,  300},
        {ClientEq::FilterType::LowShelf,  150,  4, 0.707f, 12,  60},
        {ClientEq::FilterType::HighShelf, 3000, 5, 0.707f, 12, 4500},
        {ClientEq::FilterType::HighPass,  100,  0, 0.707f, 24,  50},
        {ClientEq::FilterType::LowPass,   3000, 0, 0.707f, 24, 6000},
    };
    for (const Case& c : cases) {
        ClientEq eq;
        eq.prepare(48000);
        eq.setEnabled(true);
        eq.setActiveBandCount(1);
        ClientEq::BandParams b;
        b.type = c.t; b.freqHz = c.f0; b.gainDb = c.gain; b.q = c.q;
        b.slopeDbPerOct = c.slope; b.enabled = true;
        eq.setBand(0, b);
        eq.snapToTargets();
        const double meas = gainThrough(eq, c.probe);
        const double want = ClientEq::bandMagnitudeDb(b, c.probe, 48000);
        // The analytic curve is the analog prototype (as AetherSDR draws
        // it); the digital filter departs from it slightly deep in a
        // stopband near the top of the band, so allow more there.
        const double tol = std::fabs(want) > 12.0 ? 2.0 : 0.6;
        if (std::fabs(meas - want) > tol) {
            printf("  type %d f0 %.0f probe %.0f: measured %.2f dB, analytic %.2f dB\n",
                   int(c.t), c.f0, c.probe, meas, want);
            check(false, "measured gain matches analytic response");
        }
    }

    // 2. snapToTargets: the first 10 ms already carry the +6 dB peak
    //    (without it the smoother would start from 0 dB at 1 kHz).
    {
        ClientEq eq;
        eq.prepare(48000);
        eq.setEnabled(true);
        eq.setActiveBandCount(1);
        ClientEq::BandParams b;
        b.type = ClientEq::FilterType::Peak; b.freqHz = 2500; b.gainDb = 9;
        b.q = 1.0f; b.enabled = true;
        eq.setBand(0, b);
        eq.snapToTargets();
        std::vector<float> x = tone(2500, 4800);
        const double in = rmsDb(x, 240, 480);
        eq.process(x.data(), 480, 1);
        check(std::fabs(rmsDb(x, 240, 480) - in - 9.0) < 1.0,
              "first block is already on the final curve");
    }

    // 3. Render: 50 Hz rumble + 1 kHz voice-band tone through SSB voice.
    QTemporaryDir dir;
    const QString in = dir.path() + "/raw.wav", out = dir.path() + "/vk.wav";
    {
        std::vector<float> x = tone(50, 48000, 0.3f);
        const std::vector<float> v = tone(1000, 48000, 0.25f);
        for (size_t i = 0; i < x.size(); ++i) x[i] += v[i];
        check(ClipDeck::writeWavMono(in, x, 48000), "write test WAV");
        const VoiceEqSettings s = VoiceEqSettings::preset(VoiceEqSettings::Preset::SsbVoice);
        QString err;
        check(renderVoiceEq(in, out, s, 0.7, &err), "render with SSB voice");
        std::vector<float> y; int rate = 0;
        check(ClipDeck::readWavMono(out, y, rate) && rate == 48000 && y.size() == x.size(),
              "rendered WAV reads back, same length/rate");
        double sec = 0, pk = 0;
        ClipDeck::wavStats(out, sec, pk);
        check(std::fabs(pk - 0.7) < 0.01, "rendered message normalized to 0.7 peak");
        const double before = binDb(x, 50) - binDb(x, 1000);
        const double after  = binDb(y, 50) - binDb(y, 1000);
        printf("  50 Hz vs 1 kHz: %.1f dB before, %.1f dB after\n", before, after);
        check(before - after > 20.0, "high-pass took the 50 Hz rumble down >20 dB");

        // Make-up gain after the EQ is capped at +12 dB: a take that was
        // mostly rumble must not have its remainder pumped up 20 dB.
        {
            VoiceEqSettings eqOnly = s;                // the cap is an EQ matter
            eqOnly.gate.on = eqOnly.deess.on = eqOnly.comp.on = eqOnly.limit.on = false;
            std::vector<float> z = tone(50, 48000, 0.6f);
            const std::vector<float> w = tone(1000, 48000, 0.02f);
            for (size_t i = 0; i < z.size(); ++i) z[i] += w[i];
            ClipDeck::writeWavMono(in, z, 48000);
            renderVoiceEq(in, out, eqOnly, 0.7, &err);
            double zs = 0, zp = 0;
            ClipDeck::wavStats(out, zs, zp);
            check(zp > 0.05 && zp < 0.5, "make-up gain capped at +12 dB (no full-level pump-up)");
            ClipDeck::writeWavMono(in, x, 48000);  // back to the main signal
        }

        VoiceEqSettings off = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
        off.limit.on = false;                      // the whole chain bypassed
        check(renderVoiceEq(in, out, off, 0.7, &err), "render with EQ off");
        ClipDeck::readWavMono(out, y, rate);
        const double offRatio = binDb(y, 50) - binDb(y, 1000);
        check(std::fabs(offRatio - before) < 0.2, "EQ off leaves the spectrum alone");
        check(!QFile::exists(out + ".tmp"), "no temp file left behind");
        check(!renderVoiceEq(dir.path() + "/missing.wav", out, s, 0.7, &err),
              "missing input reports failure");
    }

    // 4. JSON round trip keeps every band; types stay pinned to slots.
    {
        VoiceEqSettings s = VoiceEqSettings::preset(VoiceEqSettings::Preset::WideSsb);
        s.bands[3].enabled = true;
        s.bands[3].gainDb = -4.5f;
        s.bands[7].slopeDbPerOct = 36;
        const VoiceEqSettings r = VoiceEqSettings::fromJson(s.toJson());
        bool same = r.enabled == s.enabled;
        for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
            const auto& a = s.bands[size_t(i)];
            const auto& b = r.bands[size_t(i)];
            same &= a.enabled == b.enabled && a.freqHz == b.freqHz
                 && a.gainDb == b.gainDb && a.q == b.q
                 && a.slopeDbPerOct == b.slopeDbPerOct
                 && b.type == VoiceEqSettings::slotType(i);
        }
        check(same, "settings JSON round trip");
        const VoiceEqSettings w = VoiceEqSettings::preset(VoiceEqSettings::Preset::WideSsb);
        check(std::fabs(w.responseDb(1000)) < 1.0, "Wide SSB is ~flat at 1 kHz");
        check(w.responseDb(30) < -6.0, "Wide SSB high-pass bites at 30 Hz");
        check(VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat).responseDb(100) == 0.0f,
              "Flat is flat");
    }

    // 5. The fitted presets stay on the targets they were fitted to (a
    //    later tweak must not silently drift them off): relative to 1 kHz,
    //    within 2.5 dB at every probe inside each preset's passband.
    {
        const auto& targets = voiceEqTargets();
        const auto byName = [&](const char* n) -> const VoiceEqTarget& {
            for (const auto& t : targets) if (t.name == n) return t;
            return targets.front();
        };
        struct Fit { VoiceEqSettings::Preset p; const char* target; float lo, hi; };
        const Fit fits[] = {
            {VoiceEqSettings::Preset::DxPunch,  "Heil DX",   300, 2700},
            {VoiceEqSettings::Preset::Shure444, "Shure 444", 100, 4000},
            {VoiceEqSettings::Preset::HeilPr40, "Heil PR40", 60, 4000},
        };
        for (const Fit& f : fits) {
            const VoiceEqSettings s = VoiceEqSettings::preset(f.p);
            const VoiceEqTarget& t = byName(f.target);
            const float ref = s.responseDb(1000) - t.dbAt(1000);
            float worst = 0;
            for (float hz = f.lo; hz <= f.hi; hz *= 1.06f)
                worst = std::max(worst, std::fabs(s.responseDb(hz) - t.dbAt(hz) - ref));
            printf("  %s vs %s: worst %.2f dB (%.0f-%.0f Hz)\n",
                   qPrintable(VoiceEqSettings::presetNames()[int(f.p)]), f.target,
                   worst, f.lo, f.hi);
            check(worst < 2.5f, "fitted preset tracks its target");
        }
    }

    // 6. The operator's named profiles: save, list, load, replace, delete.
    {
        check(VoiceEqSettings::userProfileNames().isEmpty(), "no profiles at start");
        VoiceEqSettings mine = VoiceEqSettings::preset(VoiceEqSettings::Preset::DxPunch);
        mine.bands[5].gainDb = 7.5f;
        VoiceEqSettings::saveUserProfile("EV DX / contest", mine);   // '/' in a name
        VoiceEqSettings::saveUserProfile("ragchew", VoiceEqSettings::preset(
                                             VoiceEqSettings::Preset::EssbSmooth));
        check(VoiceEqSettings::userProfileNames()
                  == QStringList({"EV DX / contest", "ragchew"}), "profiles listed, sorted");
        VoiceEqSettings got;
        check(VoiceEqSettings::loadUserProfile("EV DX / contest", got)
                  && got.bands[5].gainDb == 7.5f && got.enabled, "profile loads back");
        mine.bands[5].gainDb = 4.0f;
        VoiceEqSettings::saveUserProfile("EV DX / contest", mine);    // replace
        VoiceEqSettings::loadUserProfile("EV DX / contest", got);
        check(got.bands[5].gainDb == 4.0f && VoiceEqSettings::userProfileNames().size() == 2,
              "same name replaces, no duplicate");
        VoiceEqSettings::deleteUserProfile("ragchew");
        check(VoiceEqSettings::userProfileNames() == QStringList({"EV DX / contest"}),
              "delete removes only that profile");
        check(!VoiceEqSettings::loadUserProfile("ragchew", got), "deleted one is gone");
    }

    // 7. Dynamics through the real render path.
    {
        // Speech-like test signal: 1 kHz bursts at varying level, with a
        // noise bed between them.
        std::vector<float> x(48000 * 4);
        unsigned seed = 1;
        auto noise = [&] { seed = seed * 1103515245u + 12345u;
                           return (int((seed >> 16) & 0x7fff) - 16384) / 16384.0f; };
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = double(i) / 48000;
            const int seg = int(t * 4);            // 250 ms segments
            const float amp = seg % 2 ? 0.0f : (seg % 4 == 0 ? 0.6f : 0.12f);
            x[i] = amp * float(std::sin(2 * M_PI * 1000 * t)) + 0.003f * noise();
        }
        ClipDeck::writeWavMono(in, x, 48000);
        QString err;
        VoiceRenderStats none, gated, comped;
        VoiceEqSettings base = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
        base.limit.on = false;
        renderVoiceEq(in, out, base, 0.7, &err, &none);
        check(std::fabs(none.talkPowerDb) < 0.05, "bypassed chain: no talk-power change");

        VoiceEqSettings g = base;
        g.gate.on = true;
        g.gate.thresholdDb = -30.f;
        g.gate.ratio = 10.f;
        g.gate.floorDb = -30.f;
        g.gate.holdMs = 20.f;                      // closed well inside the
        g.gate.releaseMs = 40.f;                   // 100-200 ms window measured
        renderVoiceEq(in, out, g, 0.7, &err, &gated);
        std::vector<float> y; int rate = 0;
        ClipDeck::readWavMono(out, y, rate);
        const double gapIn  = rmsDb(x, 48000 / 4 + 4800, 48000 / 2 - 2400);
        const double gapOut = rmsDb(y, 48000 / 4 + 4800, 48000 / 2 - 2400);
        const double burstIn  = rmsDb(x, 2400, 48000 / 4 - 2400);
        const double burstOut = rmsDb(y, 2400, 48000 / 4 - 2400);
        printf("  gate: gap %.1f dB down vs burst (deepest GR %.1f dB)\n",
               (gapIn - burstIn) - (gapOut - burstOut), gated.gateMaxDb);
        check((gapIn - burstIn) - (gapOut - burstOut) > 15.0, "gate turns the gaps down");

        // Compressor: loud and quiet phrases back to back (no dead air —
        // a hard step out of silence is every compressor's worst case and
        // not how speech arrives), syllable-rate amplitude wobble.
        std::vector<float> sp(48000 * 4);
        for (size_t i = 0; i < sp.size(); ++i) {
            const double t = double(i) / 48000;
            const float phrase = int(t * 2) % 2 ? 0.12f : 0.6f;
            const float syl = 0.6f + 0.4f * float(std::fabs(std::sin(2 * M_PI * 4 * t)));
            sp[i] = phrase * syl * float(std::sin(2 * M_PI * 700 * t));
        }
        ClipDeck::writeWavMono(in, sp, 48000);
        VoiceEqSettings c = base;
        c.comp.on = true;
        c.comp.thresholdDb = -24.f;
        c.comp.ratio = 4.f;
        c.limit.on = true;                         // as every voiced preset runs it
        renderVoiceEq(in, out, c, 0.7, &err, &comped);
        // What a compressor is for: quiet phrases come up toward loud ones.
        // (Power-weighted talk power on a mostly-loud test signal barely
        // moves; on real speech the chain measures +6..8 dB.)
        std::vector<float> cy; int cr = 0;
        ClipDeck::readWavMono(out, cy, cr);
        const double loudIn = rmsDb(sp, 4800, 19200), quietIn = rmsDb(sp, 28800, 43200);
        const double loudOut = rmsDb(cy, 4800, 19200), quietOut = rmsDb(cy, 28800, 43200);
        printf("  comp: quiet-to-loud gap %.1f dB -> %.1f dB; talk power %+.1f dB, GR avg %.1f / max %.1f dB\n",
               loudIn - quietIn, loudOut - quietOut, comped.talkPowerDb,
               comped.compAvgDb, comped.compMaxDb);
        check((loudIn - quietIn) - (loudOut - quietOut) > 6.0,
              "compressor brings quiet phrases up toward loud ones");
        check(comped.talkPowerDb > -0.5, "processing never costs talk power");
        check(comped.compMaxDb < -3.0, "compressor reports its gain reduction");
        double sec = 0, pk = 0;
        ClipDeck::wavStats(out, sec, pk);
        check(std::fabs(pk - 0.7) < 0.01, "processed message still peaks at 0.7");
    }
    {
        // Final limiter: a 0 dBFS burst comes out under the ceiling (after
        // the first few ms of attack), and with it off the burst passes.
        ClientFinalLimiter lim;
        lim.prepare(48000);
        lim.setEnabled(true);
        lim.setCeilingDb(-6.f);
        lim.setDcBlockEnabled(false);
        std::vector<float> x = tone(1000, 48000, 0.99f);
        for (size_t o = 0; o < x.size(); o += 480) lim.process(x.data() + o, 480, 1);
        float pk = 0;
        for (size_t i = 4800; i < x.size(); ++i) pk = std::max(pk, std::fabs(x[i]));
        printf("  final limiter: 0 dBFS in, %.2f dBFS out (ceiling -6)\n", 20 * std::log10(pk));
        check(20 * std::log10(pk) < -5.5, "final limiter holds its ceiling");
    }
    {
        // De-esser: a 4 kHz burst is cut, a 500 Hz tone is not.
        auto through = [](float hz) {
            ClientDeEss d;
            d.prepare(48000);
            d.setEnabled(true);
            d.setFrequencyHz(4000);
            d.setQ(1.5f);
            d.setThresholdDb(-30);
            d.setAmountDb(-8);
            std::vector<float> x = tone(hz, 48000, 0.3f);
            const double in = rmsDb(x, 24000, 48000);
            for (size_t o = 0; o < x.size(); o += 480) d.process(x.data() + o, 480, 1);
            return rmsDb(x, 24000, 48000) - in;
        };
        const double hi = through(4000), lo = through(500);
        printf("  de-esser: 4 kHz %+.1f dB, 500 Hz %+.1f dB\n", hi, lo);
        check(hi < -4.0 && lo > -1.0, "de-esser cuts sibilance band only");
    }
    {
        // A curve saved before the dynamics existed loads with them off
        // and the final limiter on; the chain round-trips through JSON.
        QJsonObject old = VoiceEqSettings::preset(VoiceEqSettings::Preset::SsbVoice).toJson();
        old.remove("gate"); old.remove("deess"); old.remove("comp"); old.remove("limit");
        const VoiceEqSettings o = VoiceEqSettings::fromJson(old);
        check(!o.gate.on && !o.deess.on && !o.comp.on && !o.limit.on,
              "pre-dynamics curve: every dynamics stage off (renders as before)");
        check(!VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat).limit.on
              && VoiceEqSettings::preset(VoiceEqSettings::Preset::SsbVoice).limit.on,
              "Flat is a true bypass; the voiced presets keep the limiter");
        const VoiceEqSettings dx = VoiceEqSettings::preset(VoiceEqSettings::Preset::DxPunch);
        const VoiceEqSettings r = VoiceEqSettings::fromJson(dx.toJson());
        check(r.comp.on == dx.comp.on && r.comp.ratio == dx.comp.ratio
              && r.comp.driveDb == dx.comp.driveDb && r.gate.thresholdDb == dx.gate.thresholdDb
              && r.deess.freqHz == dx.deess.freqHz && r.limit.ceilingDb == dx.limit.ceilingDb,
              "dynamics round-trip through JSON");
    }

    if (fails == 0) printf("vkeqtest: all checks passed\n");
    return fails ? 1 : 0;
}
