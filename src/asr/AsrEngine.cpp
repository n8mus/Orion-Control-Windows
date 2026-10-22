// SPDX-License-Identifier: GPL-3.0-or-later
#include "asr/AsrEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <ggml-backend.h>
#include <whisper.h>
#include <mutex>

namespace ttc {

namespace {
constexpr int kRate = 16000;
constexpr int kFrame = 160;                        // 10 ms
constexpr int kTaps = 97;                          // 48 kHz lowpass for 3:1
constexpr float kOpenAboveFloorDb = 8.f;           // speech = floor + 8 dB
constexpr float kMinSpeechDb = -50.f;              // ... and never below this
constexpr int kFloorFrames = 200;                  // floor = quietest frame of 2 s
constexpr int kOpenFrames = 3;                     // 30 ms loud to open
constexpr int kHangFrames = 60;                    // 600 ms quiet closes a short one
constexpr int kPhraseFrames = 300;                 // past 3 s ...
constexpr int kPhrasePauseFrames = 25;             // ... a 250 ms phrase pause closes
constexpr int kCutFrames = 700;                    // 7 s: cut at the quietest point
constexpr int kCutLookFrames = 150;                //   of the last 1.5 s, keep going
constexpr int kPrerollFrames = 20;                 // 200 ms kept before onset
constexpr int kMinVoicedFrames = 40;               // < 400 ms voiced = drop
constexpr double kMaxQueueSecs = 15.0;             // backlog cap: stay current
constexpr int kMaxQueued = 2;                      //   (drop the oldest)
constexpr qint64 kHeardCallMs = 180000;            // decoded calls hint for 3 min
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}  // namespace

AsrEngine::AsrEngine(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ttc::AsrEngine::Result>();
    // 7.2 kHz windowed-sinc at 48 kHz: under the 8 kHz Nyquist of 16 kHz.
    taps_.resize(kTaps);
    double sum = 0;
    const int m = kTaps / 2;
    for (int i = 0; i < kTaps; ++i) {
        const int k = i - m;
        const double x = 2.0 * 7200.0 / 48000.0;
        const double s = k == 0 ? x : std::sin(M_PI * x * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1))
                       + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));
        taps_[size_t(i)] = float(s * w);
        sum += s * w;
    }
    for (float& t : taps_) t = float(t / sum);
    hist_.assign(kTaps, 0.f);
}

AsrEngine::~AsrEngine() { stop(); }

QString AsrEngine::defaultModelPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + "/n8mus/tentec-console/models/ggml-base.en.bin";
}

bool AsrEngine::start(const QString& modelPath, int threads, QString* err) {
    if (running_.load()) return true;
    if (!QFileInfo::exists(modelPath)) {
        if (err) *err = "no model at " + modelPath;
        return false;
    }
    if (!ctx_) {
        // Arch's ggml is built with loadable backends (one CPU build per
        // SIMD level, picked at runtime); load them first, as whisper-cli
        // does — or init asserts on a missing device.
        static std::once_flag once;
        std::call_once(once, [] { ggml_backend_load_all(); });
        whisper_context_params cp = whisper_context_default_params();
        cp.use_gpu = false;                        // GCN 1.1: CPU is the sure path
        ctx_ = whisper_init_from_file_with_params(modelPath.toLocal8Bit().constData(), cp);
        if (!ctx_) {
            if (err) *err = "whisper could not load " + modelPath;
            return false;
        }
    }
    threads_ = std::clamp(threads, 1, 16);
    stop_.store(false);
    running_.store(true);
    th_ = std::thread([this] { worker(); });
    return true;
}

void AsrEngine::stop() {
    if (th_.joinable()) {
        stop_.store(true);
        cv_.notify_all();
        th_.join();
    }
    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(mux_);
        queue_.clear();
    }
    if (ctx_) { whisper_free(ctx_); ctx_ = nullptr; }
}

void AsrEngine::setHints(const QStringList& calls, qint64 dialHz) {
    std::lock_guard<std::mutex> lk(mux_);
    hints_ = calls;
    dialHz_ = dialHz;
}

double AsrEngine::queuedSeconds() const {
    std::lock_guard<std::mutex> lk(mux_);
    size_t n = 0;
    for (const Utt& u : queue_) n += u.pcm.size();
    return double(n) / kRate;
}

void AsrEngine::feed48k(const float* x, int n, bool muted) {
    if (!running_.load()) return;
    if (muted) {
        if (inSpeech_) closeUtterance();
        frame_.clear();
        return;
    }
    for (int i = 0; i < n; ++i) {
        std::move(hist_.begin() + 1, hist_.end(), hist_.begin());
        hist_.back() = x[i];
        if (++phase_ < 3) continue;
        phase_ = 0;
        float acc = 0.f;
        for (int k = 0; k < kTaps; ++k) acc += taps_[size_t(k)] * hist_[size_t(kTaps - 1 - k)];
        frame_.push_back(acc);
        if (int(frame_.size()) < kFrame) continue;

        double e = 0;
        for (float v : frame_) e += double(v) * v;
        const float db = float(10 * std::log10(e / kFrame + 1e-12));
        // Noise floor = the quietest 10 ms of the last 2 s. Speech has
        // dips between syllables, so the floor stays under even a talker
        // who never pauses (live-found: a creeping average climbed into
        // the speech and the over never closed).
        floorHist_.push_back(db);
        if (int(floorHist_.size()) > kFloorFrames) floorHist_.pop_front();
        floorDb_ = *std::min_element(floorHist_.begin(), floorHist_.end());
        const bool loud = db > std::max(floorDb_ + kOpenAboveFloorDb, kMinSpeechDb);
        if (!inSpeech_) {
            preroll_.insert(preroll_.end(), frame_.begin(), frame_.end());
            if (preroll_.size() > size_t(kPrerollFrames * kFrame))
                preroll_.erase(preroll_.begin(), preroll_.end() - kPrerollFrames * kFrame);
            loudRun_ = loud ? loudRun_ + 1 : 0;
            if (loudRun_ >= kOpenFrames) {
                inSpeech_ = true;
                utt_ = preroll_;
                uttDb_.assign(utt_.size() / kFrame, -120.f);
                preroll_.clear();
                voicedFrames_ = loudRun_;
                quietFrames_ = 0;
                uttStartMs_ = nowMs() - qint64(utt_.size()) * 1000 / kRate;
            }
        } else {
            utt_.insert(utt_.end(), frame_.begin(), frame_.end());
            uttDb_.push_back(db);
            if (loud) { ++voicedFrames_; quietFrames_ = 0; }
            else ++quietFrames_;
            const int frames = int(uttDb_.size());
            // Short pieces close fast: a talker who keeps going is cut at
            // phrase pauses past 3 s and, failing those, at 7 s — whisper
            // reads 3-7 s pieces best and fastest here, and the copy keeps
            // up instead of arriving 30 s late in one lump.
            if (quietFrames_ >= kHangFrames
                || (frames >= kPhraseFrames && quietFrames_ >= kPhrasePauseFrames))
                closeUtterance();
            else if (frames >= kCutFrames)
                cutUtterance();
        }
        frame_.clear();
    }
}

void AsrEngine::closeUtterance() {
    static const bool dbg = qEnvironmentVariableIsSet("TTC_ASRDBG");
    if (dbg)
        std::fprintf(stderr, "[asr] close: %.2f s, %d voiced frames, floor %.1f dB%s\n",
                     utt_.size() / double(kRate), voicedFrames_, floorDb_,
                     voicedFrames_ >= kMinVoicedFrames ? "" : " (dropped: too short)");
    inSpeech_ = false;
    loudRun_ = 0;
    if (voicedFrames_ >= kMinVoicedFrames) {
        // Trim most of the closing silence (keep 200 ms).
        const size_t trim = size_t(std::max(0, quietFrames_ - 20)) * kFrame;
        if (trim < utt_.size()) utt_.resize(utt_.size() - trim);
        std::lock_guard<std::mutex> lk(mux_);
        queue_.push_back({std::move(utt_), uttStartMs_, dialHz_});
        double secs = 0;
        for (const Utt& u : queue_) secs += double(u.pcm.size()) / kRate;
        while ((secs > kMaxQueueSecs || int(queue_.size()) > kMaxQueued)
               && queue_.size() > 1) {             // CPU can't keep up: stay current
            secs -= double(queue_.front().pcm.size()) / kRate;
            queue_.pop_front();
        }
        cv_.notify_one();
    }
    utt_.clear();
    uttDb_.clear();
    voicedFrames_ = quietFrames_ = 0;
}

// A talker who doesn't pause: send what we have up to the quietest frame
// of its last 1.5 s and carry on with the rest as the next piece.
void AsrEngine::cutUtterance() {
    const int n = int(uttDb_.size());
    int cut = n - 1;
    for (int k = n - kCutLookFrames; k < n; ++k)
        if (k > 0 && uttDb_[size_t(k)] < uttDb_[size_t(cut)]) cut = k;
    std::vector<float> rest(utt_.begin() + long(cut) * kFrame, utt_.end());
    std::vector<float> restDb(uttDb_.begin() + cut, uttDb_.end());
    utt_.resize(size_t(cut) * kFrame);
    uttDb_.resize(size_t(cut));
    quietFrames_ = 0;                              // nothing to trim at a cut
    const qint64 restStart = uttStartMs_ + qint64(cut) * 10;
    closeUtterance();
    inSpeech_ = true;
    utt_ = std::move(rest);
    uttDb_ = std::move(restDb);
    voicedFrames_ = int(uttDb_.size());
    uttStartMs_ = restStart;
}

bool AsrEngine::hallucination(const QString& t) {
    // Whisper's well-known inventions on noise and silence.
    const QString s = t.trimmed().toLower();
    if (s.isEmpty() || s.size() < 2) return true;
    static const char* kSame[] = {"thank you.", "thank you", "thanks for watching!",
                                  "thanks for watching.", "you", "you.", "bye.", "bye",
                                  "okay.", "oh.", "so.", "[blank_audio]", "(silence)",
                                  "[silence]", "(music)", "[music]", "...", "."};
    for (const char* k : kSame)
        if (s == QLatin1String(k)) return true;
    return s.contains("subtitles") || s.contains("amara.org") || s.contains("subscribe");
}

// "3, 3, 3, 3, 3" -> "3, 3, 3": a word repeated more than three times
// running is the repetition loop, not speech.
QString AsrEngine::collapseRepeats(const QString& t) {
    static const QRegularExpression re("\\b(\\w+)(?:[\\s,.]+\\1\\b){3,}",
                                       QRegularExpression::CaseInsensitiveOption);
    QString out = t;
    for (int guard = 0; guard < 8; ++guard) {
        const auto m = re.match(out);
        if (!m.hasMatch()) break;
        const QString w = m.captured(1);
        out.replace(m.capturedStart(), m.capturedLength(), w + ", " + w + ", " + w + " …");
    }
    return out;
}

// The prompt read back as the transcript: nothing but hint calls.
bool AsrEngine::promptEcho(const QString& t, const QStringList& hints) {
    static const QRegularExpression split("[^A-Za-z0-9/]+");
    const QStringList words = t.toUpper().split(split, Qt::SkipEmptyParts);
    if (words.isEmpty()) return true;
    for (const QString& w : words)
        if (!hints.contains(w)) return false;
    return true;
}

AsrEngine::Result AsrEngine::transcribe(const std::vector<float>& pcm, const QStringList& hints) {
    Result r;
    if (!ctx_ || pcm.empty()) return r;
    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = threads_;
    p.no_context = true;
    p.single_segment = true;
    p.no_timestamps = true;
    p.suppress_blank = true;
    p.suppress_nst = true;                         // no [music] / (noise) tokens
    p.temperature_inc = 0.f;                       // no slow fallback re-decodes
    // Whisper's repetition loop ("1, 2, 3, 3, 3, 3, ..." for 200 tokens,
    // measured) costs seconds of decode: no piece can say more than ~5
    // tokens a second.
    p.language = "en";
    p.print_progress = p.print_realtime = p.print_timestamps = p.print_special = false;
    // Encoder window = this utterance (+ 1 s), in whisper's 20 ms frames,
    // rounded up to 64; 1500 = the full 30 s. Never under 384 (~7.7 s):
    // measured, a 2.4 s clip in a 192-frame window came out as garbage
    // ("The Id", p 0.08) where the full window read it right; 384 (7.7 s)
    // reads a 7 s piece as well as 512 in 3.3 s instead of 4.0.
    const double secs = double(pcm.size()) / kRate;
    p.max_tokens = int(std::ceil(secs * 5.0)) + 8;
    p.audio_ctx = std::clamp(int(std::ceil((secs + 1.0) * 50.0 / 64.0)) * 64, 384, 1500);
    // A sentence, not a list: measured, "Calls heard: G8SEZ, N8EM." came
    // back verbatim as the transcript of a 4 s clip, while "...contact
    // between N8EM and G8SEZ." read the audio and spelled the call. (The
    // previous piece's TEXT as prompt, AetherSDR's context carry, made it
    // repeat itself — so only calls carry over, see worker().)
    QByteArray prompt;
    if (hints.size() == 1)
        prompt = ("This is an amateur radio contact from " + hints[0] + ".").toUtf8();
    else if (hints.size() > 1)
        prompt = ("This is an amateur radio contact between " + hints.mid(0, hints.size() - 1).join(", ")
                  + " and " + hints.back() + ".").toUtf8();
    p.initial_prompt = prompt.isEmpty() ? nullptr : prompt.constData();
    const auto t0 = std::chrono::steady_clock::now();
    if (whisper_full(ctx_, p, pcm.data(), int(pcm.size())) != 0) return r;
    r.decodeMs = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count());
    const whisper_token eot = whisper_token_eot(ctx_);
    double psum = 0;
    int pn = 0;
    for (int sgi = 0; sgi < whisper_full_n_segments(ctx_); ++sgi) {
        r.text += QString::fromUtf8(whisper_full_get_segment_text(ctx_, sgi));
        for (int t = 0; t < whisper_full_n_tokens(ctx_, sgi); ++t) {
            if (whisper_full_get_token_id(ctx_, sgi, t) >= eot) continue;   // specials
            psum += whisper_full_get_token_p(ctx_, sgi, t);
            ++pn;
        }
    }
    r.text = collapseRepeats(r.text.trimmed());
    r.confidence = pn ? float(psum / pn) : 0.f;
    r.seconds = secs;
    r.calls = extractCalls(r.text);
    return r;
}

void AsrEngine::worker() {
    while (!stop_.load()) {
        Utt u;
        QStringList hints;
        {
            std::unique_lock<std::mutex> lk(mux_);
            cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) return;
            u = std::move(queue_.front());
            queue_.pop_front();
            hints = hints_;
        }
        // Hints: our own call first, then calls decoded in the last 3 min
        // (a call copied once helps every later piece), then the band map's.
        QStringList h;
        if (!hints.isEmpty()) h << hints.back();   // MainWindow appends ours last
        const qint64 tNow = nowMs();
        for (auto it = heardCalls_.begin(); it != heardCalls_.end();) {
            if (tNow - it.value() > kHeardCallMs) { it = heardCalls_.erase(it); continue; }
            if (!h.contains(it.key()) && h.size() < 5) h << it.key();
            ++it;
        }
        for (const QString& c : hints)
            if (!h.contains(c) && h.size() < 9) h << c;
        Result r = transcribe(u.pcm, h);
        r.atMs = u.atMs;
        r.dialHz = u.dialHz;
        if (qEnvironmentVariableIsSet("TTC_ASRDBG"))
            std::fprintf(stderr, "[asr] %.1f s -> \"%s\" conf %.2f in %d ms\n", r.seconds,
                         r.text.toUtf8().constData(), r.confidence, r.decodeMs);
        // AetherSDR's lowest colour band is < 0.45: below ~0.35 it is
        // almost always noise read as words.
        if (hallucination(r.text) || r.confidence < 0.35f || promptEcho(r.text, h)) continue;
        for (const CallHit& c : r.calls) heardCalls_[c.call] = tNow;
        emit transcribed(r);
    }
}

} // namespace ttc
