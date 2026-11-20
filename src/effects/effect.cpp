// The built-in effects' framework (daw/audio/effect.py, native/effects.cpp's shared part).
#include "effects/effect.h"

#include <algorithm>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "core/json.h"

namespace effects {

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double gainToDb(double gain) { return 20.0 * std::log10(std::max(gain, 1e-9)); }

std::string pyFloat(double x) {
    if (std::isnan(x)) return "NaN";
    if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
    if (x == 0) return std::signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    int prec = 1;
    for (; prec <= 17; prec++) {  // the shortest digits that read back as x
        snprintf(buf, sizeof buf, "%.*e", prec - 1, x);
        if (std::strtod(buf, nullptr) == x) break;
    }
    std::string s = buf;
    bool negative = s[0] == '-';
    if (negative) s = s.substr(1);
    size_t e = s.find('e');
    int exp = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; i++)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            std::string whole = digits.substr(0, std::min(digits.size(), (size_t)exp + 1));
            while ((int)whole.size() < exp + 1) whole += '0';
            std::string frac = digits.size() > (size_t)exp + 1 ? digits.substr(exp + 1) : "0";
            out = whole + "." + frac;
        } else {
            out = "0." + std::string(-exp - 1, '0') + digits;
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char ebuf[16];
        snprintf(ebuf, sizeof ebuf, "e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
        out += ebuf;
    }
    return negative ? "-" + out : out;
}

// --- parameters ---

int Param::steps() const {
    if (!choices.empty()) return std::max(1, (int)choices.size() - 1);
    return scale == Scale::Int ? (int)(high - low) : 0;
}

double Param::toNormal(double value) const {
    if (!choices.empty()) return std::min(1.0, std::max(0.0, value / std::max(1, (int)choices.size() - 1)));
    value = std::min(high, std::max(low, value));
    if (scale == Scale::Log) return std::log(value / low) / std::log(high / low);
    return (value - low) / (high - low);
}

double Param::fromNormal(double normal) const {
    normal = std::min(1.0, std::max(0.0, normal));
    if (!choices.empty()) return roundEven(normal * ((int)choices.size() - 1));
    if (scale == Scale::Log) return low * std::pow(high / low, normal);
    double value = low + normal * (high - low);
    return scale == Scale::Int ? roundEven(value) : value;
}

std::string Param::text(double value) const {
    if (!choices.empty()) {
        int i = std::min((int)choices.size() - 1, std::max(0, (int)roundEven(value)));
        return choices[i];
    }
    int d = decimals;
    if (d < 0) {
        double span = scale == Scale::Log ? std::fabs(value) : high - low;
        d = scale == Scale::Int || span >= 100 ? 0 : span >= 10 ? 1 : 2;
    }
    char buf[64];
    if (unit == "Hz" && value >= 1000) {
        snprintf(buf, sizeof buf, "%.2f kHz", value / 1000);
        return buf;
    }
    snprintf(buf, sizeof buf, "%.*f", d, value);
    return buf + (unit.empty() || unit == ":1" ? unit : " " + unit);
}

Param param(const std::string& key, const std::string& name, double low, double high, double def,
            const std::string& unit, Scale scale, int decimals) {
    Param p;
    p.key = key;
    p.name = name;
    p.low = low;
    p.high = high;
    p.def = def;
    p.unit = unit;
    p.scale = scale;
    p.decimals = decimals;
    return p;
}

Param choice(const std::string& key, const std::string& name, std::vector<std::string> choices, double def) {
    Param p;
    p.key = key;
    p.name = name;
    p.choices = std::move(choices);
    p.def = def;
    return p;
}

Param toggle(const std::string& key, const std::string& name, bool def) {
    return choice(key, name, {"Off", "On"}, def ? 1.0 : 0.0);
}

// --- building blocks ---

Sos biquad(Kind kind, double freq, double sr, double q, double gainDb) {
    if (kind == Kind::Flat) return {1.0, 0, 0, 1.0, 0, 0};
    freq = std::min(std::max(freq, 1.0), sr * 0.49);
    double w = 2 * PI * freq / sr, a = std::pow(10.0, gainDb / 40);
    double cw = std::cos(w), alpha = std::sin(w) / (2 * q);
    double b[3], d[3];
    if (kind == Kind::Lowpass) {
        b[0] = (1 - cw) / 2; b[1] = 1 - cw; b[2] = (1 - cw) / 2;
        d[0] = 1 + alpha; d[1] = -2 * cw; d[2] = 1 - alpha;
    } else if (kind == Kind::Highpass) {
        b[0] = (1 + cw) / 2; b[1] = -(1 + cw); b[2] = (1 + cw) / 2;
        d[0] = 1 + alpha; d[1] = -2 * cw; d[2] = 1 - alpha;
    } else if (kind == Kind::Peak) {
        b[0] = 1 + alpha * a; b[1] = -2 * cw; b[2] = 1 - alpha * a;
        d[0] = 1 + alpha / a; d[1] = -2 * cw; d[2] = 1 - alpha / a;
    } else {
        double s = 2 * std::sqrt(a) * alpha;
        if (kind == Kind::Lowshelf) {
            b[0] = a * ((a + 1) - (a - 1) * cw + s); b[1] = 2 * a * ((a - 1) - (a + 1) * cw); b[2] = a * ((a + 1) - (a - 1) * cw - s);
            d[0] = (a + 1) + (a - 1) * cw + s; d[1] = -2 * ((a - 1) + (a + 1) * cw); d[2] = (a + 1) + (a - 1) * cw - s;
        } else {
            b[0] = a * ((a + 1) + (a - 1) * cw + s); b[1] = -2 * a * ((a - 1) + (a + 1) * cw); b[2] = a * ((a + 1) + (a - 1) * cw - s);
            d[0] = (a + 1) - (a - 1) * cw + s; d[1] = 2 * ((a - 1) - (a + 1) * cw); d[2] = (a + 1) - (a - 1) * cw - s;
        }
    }
    return {b[0] / d[0], b[1] / d[0], b[2] / d[0], 1.0, d[1] / d[0], d[2] / d[0]};
}

void linkwitzRiley(Kind kind, double freq, double sr, Sos out[2]) {
    out[0] = out[1] = biquad(kind, freq, sr, 1 / std::sqrt(2.0));
}

// scipy.signal.ellip(8, 0.05, 90, 0.45, output="sos"): 0.05 dB ripple up to 0.45 of Nyquist, 90 dB
// down from 0.6 (worked out with scipy's algorithm: ellipap, bilinear, zpk2sos "nearest")
const Sos OVERSAMPLING_SOS[4] = {
    {0.006418719305896211, 0.012206196623871788, 0.006418719305896211, 1.0, -0.8885399389085958, 0.24734327891623267},
    {1.0, 1.3577684075312757, 1.0, 1.0, -0.6387180963453178, 0.4485942848463482},
    {1.0, 0.8746614387181978, 1.0, 1.0, -0.37026473520087055, 0.6918866515252494},
    {1.0, 0.6449220349638457, 1.0, 1.0, -0.22600166520706372, 0.9004253055341144},
};

double magnitude(const Sos* sections, int count, double freq, double sr) {
    std::complex<double> e = std::polar(1.0, -2 * PI * freq / sr), h = 1.0;
    for (int i = 0; i < count; i++) {
        const Sos& s = sections[i];
        h *= (s[0] + s[1] * e + s[2] * e * e) / (s[3] + s[4] * e + s[5] * e * e);
    }
    return std::abs(h);
}

void Filters::set(const Sos* sections, int n) {
    n = std::min(n, MAX);
    if (n != count) {
        count = n;
        reset();
    }
    for (int i = 0; i < n; i++) sos[i] = sections[i];
}

void Filters::set(std::initializer_list<Sos> sections) {
    Sos s[MAX];
    int n = 0;
    for (const Sos& x : sections)
        if (n < MAX) s[n++] = x;
    set(s, n);
}

void Filters::reset() {
    for (auto& z : zi) z[0] = z[1] = z[2] = z[3] = 0;
}

void Filters::run(const float* const* in, float* const* out, int n) {
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n; i++) {
            double x = in[c][i];
            for (int s = 0; s < count; s++) {
                const Sos& f = sos[s];
                double& z0 = zi[s][2 * c];
                double& z1 = zi[s][2 * c + 1];
                double y = f[0] * x + z0;
                z0 = f[1] * x - f[4] * y + z1;
                z1 = f[2] * x - f[5] * y;
                x = y;
            }
            out[c][i] = (float)x;
        }
}

double timeCoeff(double ms, double sr, double block) { return std::exp(-block / std::max(1e-6, ms * 0.001 * sr)); }

double Envelope::follow(const float* const* x, int n, double attackMs, double releaseMs, double sr, bool rms,
                        double* out, int* count) {
    int blocks = std::max(1, n / ENVELOPE_BLOCK);
    int per = n >= ENVELOPE_BLOCK ? ENVELOPE_BLOCK : n;
    double size = (double)n / blocks;
    double attack = timeCoeff(attackMs, sr, size), release = timeCoeff(releaseMs, sr, size);
    for (int b = 0; b < blocks; b++) {
        double raw = 0;
        for (int i = b * per; i < b * per + per; i++)
            for (int c = 0; c < 2; c++) {
                double v = x[c][i];
                raw = rms ? raw + v * v : std::max(raw, std::fabs(v));
            }
        if (rms) raw = std::sqrt(raw / (2.0 * std::max(1, per)));
        double target = gainToDb(raw);
        double coeff = target > level ? attack : release;
        level = target + coeff * (level - target);
        out[b] = level;
    }
    *count = blocks;
    return size;
}

void perFrame(const double* values, int count, double size, int n, double last, float* out) {
    int j = 0;  // the line from ends[j - 1] (0 for j = 0) to ends[j]
    for (int k = 1; k <= n; k++) {
        while (j < count && (j + 1) * size < k) j++;
        if (j >= count) {
            out[k - 1] = (float)values[count - 1];
            continue;
        }
        double x0 = j * size, x1 = (j + 1) * size;
        double y0 = j == 0 ? last : values[j - 1], y1 = values[j];
        out[k - 1] = (float)(k >= x1 ? y1 : y0 + (y1 - y0) * (k - x0) / (x1 - x0));
    }
}

double compression(double levelDb, double threshold, double ratio, double knee) {
    double over = levelDb - threshold, slope = 1.0 / ratio - 1.0;
    double hard = over > 0 ? slope * over : 0.0;
    if (knee <= 0) return hard;
    if (std::fabs(over) <= knee / 2) return slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
    return hard;
}

void FractionalDelay::allocate(double longest) {
    history.assign((size_t)longest + 2, {0.0f, 0.0f});
    head = 0;
}

void FractionalDelay::clear() {
    std::fill(history.begin(), history.end(), std::array<float, 2>{0.0f, 0.0f});
    head = 0;
}

float FractionalDelay::at(size_t index, int c, const float* const* x) const {
    size_t n = history.size();
    return index < n ? history[(head + index) % n][c] : x[c][index - n];
}

float FractionalDelay::read(int j, int c, double delay, const float* const* x, int frames) const {
    size_t n = history.size();
    double pos = n + j - std::min(delay, (double)(n - 1));
    size_t i = (size_t)std::floor(pos);
    float frac = (float)(pos - (double)i);
    size_t next = std::min(i + 1, n + frames - 1);
    return at(i, c, x) * (1 - frac) + at(next, c, x) * frac;
}

void FractionalDelay::write(const float* const* x, int frames) {
    size_t n = history.size();
    for (int j = 0; j < frames; j++) {
        history[head] = {x[0][j], x[1][j]};
        head = (head + 1) % n;
    }
}

void Rows::allocate(int n) {
    for (int c = 0; c < 2; c++) {
        data[c].assign(std::max(1, n), 0.0f);
        row[c] = data[c].data();
    }
}

// --- the effect ---

Effect::Effect(std::string id, std::string name, std::vector<Param> params, bool sidechain)
    : id_(std::move(id)), name_(std::move(name)), params_(std::move(params)), sidechain_(sidechain),
      normals_(params_.size()), values_(params_.size()) {
    for (size_t i = 0; i < params_.size(); i++) {
        values_[i] = params_[i].def;
        normals_[i] = params_[i].toNormal(params_[i].def);
    }
}

std::string Effect::path() const { return std::string("builtin:") + id_; }

double Effect::value(const std::string& k) const {
    for (size_t i = 0; i < params_.size(); i++)
        if (params_[i].key == k) return value((int)i);
    return 0;
}

int Effect::key(const char* k) const {
    for (size_t i = 0; i < params_.size(); i++)
        if (params_[i].key == k) return (int)i;
    throw std::invalid_argument(std::string("the effect has no parameter ") + k);
}

void Effect::prepare(double sampleRate, int maxBlock) {
    sr = sampleRate;
    maxBlock_ = std::max(1, maxBlock);
    in_.allocate(maxBlock_);
    side_.allocate(maxBlock_);
    allocate(sr, maxBlock_);
    start();
    quiet_ = 0;
    changed_ = true;
}

void Effect::reset() {
    start();
    quiet_ = 0;
    changed_ = true;
}

void Effect::setTransport(double tempo, int numerator, int denominator, bool playing, int64_t positionFrames, double) {
    tempo_.store(tempo, std::memory_order_relaxed);
    numerator_.store(numerator, std::memory_order_relaxed);
    denominator_.store(denominator, std::memory_order_relaxed);
    playing_.store(playing, std::memory_order_relaxed);
    position_.store(positionFrames, std::memory_order_relaxed);
}

double Effect::beatsPerBar() const {
    int num = numerator_.load(std::memory_order_relaxed), den = denominator_.load(std::memory_order_relaxed);
    if (num <= 0 || den <= 0) return 4.0;
    return std::min(64.0, std::max(0.25, num * 4.0 / den));
}

bool Effect::silent(const float* const* x, int n) {
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n; i++)
            if (std::fabs(x[c][i]) > SILENT) return false;
    return true;
}

void Effect::process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) {
    if (maxBlock_ <= 0) prepare(sr);  // (never prepared: made usable, as the twins are on creation)
    for (int done = 0; done < frames;) {
        int n = std::min(maxBlock_, frames - done);
        const float* i[2] = {in[0] + done, in[1] + done};
        const float* s[2] = {nullptr, nullptr};
        if (sidechain && sidechain[0]) {
            s[0] = sidechain[0] + done;
            s[1] = (sidechain[1] ? sidechain[1] : sidechain[0]) + done;
        }
        float* o[2] = {out[0] + done, out[1] + done};
        chunk(i, s[0] ? s : nullptr, o, n);
        if (playing()) position_.fetch_add(n, std::memory_order_relaxed);  // (the next piece's place)
        done += n;
    }
}

void Effect::chunk(const float* const in[2], const float* const side[2], float* const out[2], int n) {
    float* const* x = in_.rows();
    for (int c = 0; c < 2; c++) std::copy(in[c], in[c] + n, x[c]);
    const float* const* s = nullptr;
    if (side) {
        float* const* rows = side_.rows();
        for (int c = 0; c < 2; c++) std::copy(side[c], side[c] + n, rows[c]);
        s = rows;
    }
    if (changed_.exchange(false)) changed();
    bool silentIn = silent(x, n) && (!s || silent(s, n));
    if (silentIn && asleep()) {
        for (int c = 0; c < 2; c++) std::fill(out[c], out[c] + n, 0.0f);
        return;
    }
    if (n > 0) block(x, s, out, n);
    if (!silentIn || !silent(out, n)) {
        quiet_ = 0;
    } else {
        quiet_ += n;
        if (asleep()) {
            start();  // (asleep from now on: what it held is cleared)
            // (and its coefficients worked out again on waking: start() may have set some back —
            // the EQ's output gain — which the Python program forgot until a parameter changed)
            changed_ = true;
        }
    }
}

void Effect::render(const std::vector<plug::NoteMessage>&, float* const out[2], int frames) {
    for (int c = 0; c < 2; c++) std::fill(out[c], out[c] + frames, 0.0f);
}

std::vector<plug::ParamInfo> Effect::parameters() {
    std::vector<plug::ParamInfo> list;
    for (size_t i = 0; i < params_.size(); i++) {
        plug::ParamInfo info;
        info.id = (uint32_t)i;
        info.name = params_[i].name;
        info.units = params_[i].unit;
        info.defaultValue = params_[i].toNormal(params_[i].def);
        info.stepCount = params_[i].steps();
        list.push_back(info);
    }
    return list;
}

double Effect::getParam(uint32_t id) { return id < params_.size() ? normals_[id].load() : 0.0; }

void Effect::setParam(uint32_t id, double value) {
    if (id >= params_.size()) return;
    double normal = std::min(1.0, std::max(0.0, value));
    if (std::isnan(value)) normal = 0;
    normals_[id] = normal;
    values_[id] = params_[id].fromNormal(normal);
    changed_ = true;
}

std::string Effect::paramText(uint32_t id, double value) {
    return id < params_.size() ? params_[id].text(params_[id].fromNormal(value)) : "";
}

std::vector<uint32_t> Effect::edited() {
    std::vector<uint32_t> e;
    e.swap(edits_);
    return e;
}

void Effect::windowEdit(uint32_t id, double value) {
    setParam(id, value);
    edits_.push_back(id);
}

std::string Effect::componentState() {  // json.dumps({key: value, …}): the Python program's text exactly
    std::string s = "{";
    for (size_t i = 0; i < params_.size(); i++) {
        if (i) s += ", ";
        s += "\"" + params_[i].key + "\": " + pyFloat(value((int)i));
    }
    return s + "}";
}

void Effect::setState(const std::string& component, const std::string&) {
    std::string error;
    Json values = Json::parse(component.empty() ? "{}" : component, &error);
    if (!error.empty() || !values.isObject()) return;
    for (size_t i = 0; i < params_.size(); i++) {
        const Json& j = values[params_[i].key];
        if (j.isNumber())
            setParam((uint32_t)i, params_[i].toNormal(j.toDouble()));
        else if (j.isBool())  // (Python: isinstance(True, int))
            setParam((uint32_t)i, params_[i].toNormal(j.toBool() ? 1.0 : 0.0));
    }
}

}  // namespace effects
