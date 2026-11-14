#include "audio/loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loudness {

namespace {
const double PI = 3.14159265358979323846;

Biquad biquad(bool highShelf, double gainDb, double q, double fc, double sr) {
    double a = std::pow(10.0, gainDb / 40);
    double w0 = 2 * PI * fc / sr;
    double alpha = std::sin(w0) / (2 * q), cs = std::cos(w0);
    double b[3], den[3];
    if (highShelf) {
        double sa = 2 * std::sqrt(a) * alpha;
        b[0] = a * ((a + 1) + (a - 1) * cs + sa);
        b[1] = -2 * a * ((a - 1) + (a + 1) * cs);
        b[2] = a * ((a + 1) + (a - 1) * cs - sa);
        den[0] = (a + 1) - (a - 1) * cs + sa;
        den[1] = 2 * ((a - 1) - (a + 1) * cs);
        den[2] = (a + 1) - (a - 1) * cs - sa;
    } else {  // high-pass
        b[0] = (1 + cs) / 2;
        b[1] = -(1 + cs);
        b[2] = (1 + cs) / 2;
        den[0] = 1 + alpha;
        den[1] = -2 * cs;
        den[2] = 1 - alpha;
    }
    return {b[0] / den[0], b[1] / den[0], b[2] / den[0], den[1] / den[0], den[2] / den[0]};
}
}  // namespace

std::array<Biquad, 2> kWeighting(double sr) {
    return {biquad(true, 4.0, 1 / std::sqrt(2.0), 1500.0, sr), biquad(false, 0.0, 0.5, 38.0, sr)};
}

// Below this mean power (-150 LUFS) the sound is silence: the K-weighting filters' state decays towards 0
// after the sound stops without ever reaching it, which read as "-1853.2" instead of the "–" of silence.
constexpr double SILENT_POWER = 1e-15;

double toLufs(double power) {
    return power > SILENT_POWER ? -0.691 + 10 * std::log10(power) : -std::numeric_limits<double>::infinity();
}

double integrated(const std::vector<double>& gates) {
    double absolute = std::pow(10.0, (ABSOLUTE_GATE_LUFS + 0.691) / 10);
    double sum = 0;
    size_t n = 0;
    for (double g : gates)
        if (g > absolute) sum += g, n++;
    if (!n) return -std::numeric_limits<double>::infinity();
    double relative = toLufs(sum / n) + RELATIVE_GATE_LU;
    double gate = std::pow(10.0, (relative + 0.691) / 10);
    double sum2 = 0;
    size_t n2 = 0;
    for (double g : gates)
        if (g > absolute && g > gate) sum2 += g, n2++;
    return n2 ? toLufs(sum2 / n2) : -std::numeric_limits<double>::infinity();
}

LoudnessMeter::LoudnessMeter(int sr, int ch)
    : channels(ch), sos_(kWeighting(sr)), zi_(2 * 2 * std::max(1, ch), 0.0), hop_((int)(sr * BLOCK_S)) {}

void LoudnessMeter::add(const float* x, int frames) {
    if (frames <= 0) return;
    if ((int)power_.size() < frames) power_.resize(frames);
    std::fill(power_.begin(), power_.begin() + frames, 0.0);
    for (int c = 0; c < channels; c++) {
        // scipy.signal.sosfilt: direct form II transposed, section after section
        double* z0 = &zi_[(0 * 2 + 0) * channels + c];
        double* z1 = &zi_[(0 * 2 + 1) * channels + c];
        double* w0 = &zi_[(1 * 2 + 0) * channels + c];
        double* w1 = &zi_[(1 * 2 + 1) * channels + c];
        const Biquad& s = sos_[0];
        const Biquad& h = sos_[1];
        double a0 = *z0, a1 = *z1, c0 = *w0, c1 = *w1;
        for (int i = 0; i < frames; i++) {
            double v = x[(size_t)i * channels + c];
            double y = s.b0 * v + a0;
            a0 = s.b1 * v - s.a1 * y + a1;
            a1 = s.b2 * v - s.a2 * y;
            double y2 = h.b0 * y + c0;
            c0 = h.b1 * y - h.a1 * y2 + c1;
            c1 = h.b2 * y - h.a2 * y2;
            power_[i] += y2 * y2;  // per-frame power summed over channels
        }
        *z0 = a0, *z1 = a1, *w0 = c0, *w1 = c1;
    }
    collect(power_.data(), frames);
}

void LoudnessMeter::addSilence(int frames) {
    std::fill(zi_.begin(), zi_.end(), 0.0);
    collect(nullptr, frames);
}

void LoudnessMeter::collect(const double* power, int n) {
    int pos = 0;
    while (pos < n) {
        int take = std::min(hop_ - count_, n - pos);
        if (power)
            for (int i = 0; i < take; i++) sum_ += power[pos + i];
        count_ += take;
        pos += take;
        if (count_ == hop_) {
            std::lock_guard<std::mutex> g(lock_);
            blocks_.push_back(sum_ / hop_);
            if ((int)blocks_.size() > SHORT_TERM_BLOCKS) blocks_.pop_front();
            if ((int)blocks_.size() >= MOMENTARY_BLOCKS) {
                double s = 0;
                for (int i = (int)blocks_.size() - MOMENTARY_BLOCKS; i < (int)blocks_.size(); i++) s += blocks_[i];
                gates_.push_back(s / MOMENTARY_BLOCKS);
            }
            sum_ = 0, count_ = 0;
        }
    }
}

void LoudnessMeter::read(double& momentary, double& shortTerm, double& integratedOut) const {
    std::vector<double> gates;
    double m = 0, s = 0;
    size_t nb;
    {
        std::lock_guard<std::mutex> g(lock_);
        nb = blocks_.size();
        size_t from = nb > (size_t)MOMENTARY_BLOCKS ? nb - MOMENTARY_BLOCKS : 0;
        for (size_t i = 0; i < nb; i++) {
            s += blocks_[i];
            if (i >= from) m += blocks_[i];
        }
        if (nb) m /= (double)(nb - from), s /= (double)nb;
        gates = gates_;
    }
    const double inf = std::numeric_limits<double>::infinity();
    momentary = nb ? toLufs(m) : -inf;
    shortTerm = nb ? toLufs(s) : -inf;
    integratedOut = integrated(gates);
}

}  // namespace loudness
