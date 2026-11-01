/*
 * Features measured on mono renders, for checking the engine against the fitted tables
 * (doc/oscairn-plan.md, milestone 1). The same definitions as the measurement repository's
 * analog/features.py, kept small: times in seconds, levels in dB, frequencies in Hz.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors.
 */
#ifndef OSCAIRN_ANALOG_FEATURES_H
#define OSCAIRN_ANALOG_FEATURES_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace oscairn_features
{
struct Envelope
{
    std::vector<double> t, db;
};

// RMS level in dB over sliding windows, centred times.
inline Envelope envelopeDb(const std::vector<float> &x, double rate, double windowMs,
                           double hopMs)
{
    Envelope e;
    size_t win = std::max<size_t>(1, (size_t)(rate * windowMs / 1000)),
           hop = std::max<size_t>(1, (size_t)(rate * hopMs / 1000));
    for (size_t s = 0; s + win <= x.size(); s += hop)
    {
        double p = 0;
        for (size_t i = s; i < s + win; ++i)
            p += (double)x[i] * x[i];
        e.t.push_back((s + win / 2.0) / rate);
        e.db.push_back(10 * std::log10(std::max(p / win, 1e-24)));
    }
    return e;
}

inline double onset(const std::vector<float> &x, double rate, double thresholdDb = -60)
{
    auto e = envelopeDb(x, rate, 1.0, 0.25);
    for (size_t i = 0; i < e.t.size(); ++i)
        if (e.db[i] >= thresholdDb)
            return e.t[i];
    return -1;
}

inline double levelDb(const std::vector<float> &x, double rate, double start, double stop)
{
    double p = 0;
    size_t a = (size_t)(start * rate), b = std::min(x.size(), (size_t)(stop * rate));
    for (size_t i = a; i < b; ++i)
        p += (double)x[i] * x[i];
    return 10 * std::log10(std::max(p / std::max<size_t>(1, b - a), 1e-24));
}

// Time after `from` where the envelope first falls to `level` or below, or -1.
inline double fallsTo(const Envelope &e, double level, double from)
{
    for (size_t i = 0; i < e.t.size(); ++i)
        if (e.t[i] >= from && e.db[i] <= level)
            return e.t[i] - from;
    return -1;
}

// 10 % to 90 % amplitude rise time of the highest amplitude reached.
inline double attackTime(const std::vector<float> &x, double rate, double windowMs = 5.0)
{
    auto e = envelopeDb(x, rate, windowMs, std::min(1.0, windowMs / 4));
    double peak = *std::max_element(e.db.begin(), e.db.end());
    double lo = -1, hi = -1;
    for (size_t i = 0; i < e.t.size() && hi < 0; ++i)
    {
        if (lo < 0 && e.db[i] >= peak - 20)
            lo = e.t[i];
        if (e.db[i] >= peak + 20 * std::log10(0.9))
            hi = e.t[i];
    }
    return hi - lo;
}

// Time from the peak to 40 dB below it.
inline double decayTime(const std::vector<float> &x, double rate, double windowMs = 5.0)
{
    auto e = envelopeDb(x, rate, windowMs, windowMs / 10);
    auto k = std::max_element(e.db.begin(), e.db.end()) - e.db.begin();
    return fallsTo(e, e.db[k] - 40, e.t[k]);
}

// From where the level first falls 1 dB below the held level (searched from
// noteOff - uncertainty) to 40 dB below it.
inline double releaseTime(const std::vector<float> &x, double rate, double noteOff,
                          double uncertainty, double windowMs = 2.0)
{
    auto e = envelopeDb(x, rate, windowMs, windowMs / 8);
    std::vector<double> held;
    for (size_t i = 0; i < e.t.size(); ++i)
        if (e.t[i] > noteOff - uncertainty - 0.2 && e.t[i] < noteOff - uncertainty)
            held.push_back(e.db[i]);
    if (held.empty())
        return -1;
    std::nth_element(held.begin(), held.begin() + held.size() / 2, held.end());
    double h = held[held.size() / 2];
    double knee = fallsTo(e, h - 1, noteOff - uncertainty);
    if (knee < 0)
        return -1;
    return fallsTo(e, h - 40, noteOff - uncertainty + knee);
}

// 90 % to 10 % time of an offset c(t) falling exponentially from full, after start: ln 9
// over the slope of a straight line fitted to log(c) against time between the first 90 %
// and 10 % crossings of a 9-point median of the track. The fit is pitch_fall_time's and
// duty_fall_time's in the measurement repository.
inline double fallTime(const std::vector<double> &t, const std::vector<double> &c, double full,
                       double start)
{
    std::vector<double> tt, cc, sm;
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] > start)
        {
            tt.push_back(t[i]);
            cc.push_back(c[i]);
        }
    for (size_t i = 0; i < cc.size(); ++i) // medfilt with zero padding, as scipy's
    {
        double w[9];
        for (int k = 0; k < 9; ++k)
        {
            long j = (long)i + k - 4;
            w[k] = j < 0 || j >= (long)cc.size() ? 0 : cc[j];
        }
        std::nth_element(w, w + 4, w + 9);
        sm.push_back(w[4]);
    }
    long k0 = -1, k1 = -1;
    for (size_t i = 0; i < sm.size() && k1 < 0; ++i)
    {
        if (k0 < 0 && sm[i] <= 0.9 * full)
            k0 = (long)i;
        if (sm[i] <= 0.1 * full)
            k1 = (long)i;
    }
    if (k0 < 0 || k1 <= k0)
        return -1;
    double n = 0, st = 0, sl = 0, stt = 0, stl = 0;
    for (long i = k0; i < k1; ++i)
        if (cc[i] > 0.05 * full)
        {
            double l = std::log(cc[i]);
            n += 1;
            st += tt[i];
            sl += l;
            stt += tt[i] * tt[i];
            stl += tt[i] * l;
        }
    double slope = (n * stl - st * sl) / (n * stt - st * st);
    return std::log(9.0) / -slope;
}

// 90 % to 10 % time of a sine's pitch offset falling exponentially in cents from fullCents
// (relative to fRef), after start (seconds from onset): ln 9 over the slope of a straight
// line fitted to log(cents) against time between the first 90 % and 10 % crossings of a
// 9-period median of the zero-crossing pitch track. As pitch_fall_time in the measurement
// repository.
inline double pitchFallTime(const std::vector<float> &x, double rate, double fRef,
                            double fullCents, double start)
{
    size_t a = (size_t)(onset(x, rate) * rate);
    std::vector<double> t, c;
    double prev = -1;
    for (size_t i = a; i + 1 < x.size(); ++i)
        if (x[i] < 0 && x[i + 1] >= 0) // upward crossing, interpolated (signbit as in Python)
        {
            double z = i + x[i] / (x[i] - x[i + 1]);
            if (prev >= 0)
            {
                t.push_back(((prev + z) / 2 - a) / rate);
                c.push_back(1200 * std::log2(rate / (z - prev) / fRef));
            }
            prev = z;
        }
    return fallTime(t, c, fullCents, start);
}

// 10 % to 90 % time of a sine's pitch glide of `semitones` (portamento), the glide starting
// after `after` seconds from the onset, relative to the median pitch over before0..before1 s
// from the onset: a 5-period median of the zero-crossing pitch track. As glide_times in the
// measurement repository (fit kind glide).
inline double glideTime(const std::vector<float> &x, double rate, double fRef, double semitones,
                        double after, double before0 = 0.2, double before1 = 0.9)
{
    double t0 = onset(x, rate);
    std::vector<double> t, c;
    double prev = -1;
    for (size_t i = 0; i + 1 < x.size(); ++i)
        if (x[i] < 0 && x[i + 1] >= 0)
        {
            double z = i + x[i] / (x[i] - x[i + 1]);
            if (prev >= 0)
            {
                t.push_back((prev + z) / 2 / rate);
                c.push_back(1200 * std::log2(rate / (z - prev) / fRef) / (100 * semitones));
            }
            prev = z;
        }
    std::vector<double> m(c.size());
    for (size_t i = 0; i < c.size(); ++i) // as scipy's medfilt: zeros past the ends
    {
        double w[5];
        for (int k = -2; k <= 2; ++k)
            w[k + 2] = (i + k < c.size()) ? c[i + k] : 0.0; // i + k wraps when negative
        std::sort(w, w + 5);
        m[i] = w[2];
    }
    std::vector<double> held;
    for (size_t i = 0; i < m.size(); ++i)
        if (t[i] > t0 + before0 && t[i] < t0 + before1)
            held.push_back(m[i]);
    if (held.empty())
        return -1;
    std::nth_element(held.begin(), held.begin() + held.size() / 2, held.end());
    double base = held[held.size() / 2];
    double t10 = -1;
    for (size_t i = 0; i < m.size(); ++i)
    {
        if (t[i] <= t0 + after)
            continue;
        if (t10 < 0 && m[i] - base >= 0.1)
            t10 = t[i];
        if (t10 >= 0 && m[i] - base >= 0.9)
            return t[i] - t10;
    }
    return -1;
}

// 90 % to 10 % time of a pulse wave's duty cycle falling exponentially from base + full
// toward base, after start (seconds from onset). The duty, one value per period, is
// acos(h2 / h1) / pi from the first two harmonics over Hann windows `periods` periods long
// (a duty above 0.5 reads as 1 - duty). As duty_fall_time in the measurement repository.
inline double dutyFallTime(const std::vector<float> &x, double rate, double f0, double base,
                           double full, double start, int periods = 4)
{
    size_t a = (size_t)(onset(x, rate) * rate);
    size_t n = (size_t)std::lround(periods * rate / f0);
    std::vector<double> t, c;
    const double pi = 3.14159265358979323846;
    for (double s = 0; a + (size_t)s + n < x.size(); s += rate / f0)
    {
        size_t s0 = a + (size_t)s;
        double r1 = 0, i1 = 0, r2 = 0, i2 = 0;
        for (size_t k = 0; k < n; ++k)
        {
            double v = x[s0 + k] * (0.5 - 0.5 * std::cos(2 * pi * k / (n - 1)));
            double p = 2 * pi * f0 * k / rate;
            r1 += v * std::cos(p);
            i1 -= v * std::sin(p);
            r2 += v * std::cos(2 * p);
            i2 -= v * std::sin(2 * p);
        }
        double ratio = std::hypot(r2, i2) / std::max(std::hypot(r1, i1), 1e-12);
        t.push_back(((size_t)s + n / 2.0) / rate);
        c.push_back((std::acos(std::clamp(ratio, 0.0, 1.0)) / pi - base) / full);
    }
    return fallTime(t, c, 1.0, start);
}

// Rate of a periodic level change between start and stop: rising crossings of the mean
// (with hysteresis of a quarter of the standard deviation, so oscillator ripple doesn't
// count) by the RMS amplitude over windowMs, interpolated, over whole periods. windowMs
// should span whole oscillator periods.
inline double lfoRate(const std::vector<float> &x, double rate, double start, double stop,
                      double windowMs = 7.645)
{
    std::vector<float> seg(x.begin() + (size_t)(start * rate),
                           x.begin() + std::min(x.size(), (size_t)(stop * rate)));
    auto e = envelopeDb(seg, rate, windowMs, 0.5);
    std::vector<double> a(e.db.size());
    double mean = 0, var = 0;
    for (size_t i = 0; i < a.size(); ++i)
        mean += (a[i] = std::pow(10.0, e.db[i] / 20));
    mean /= a.size();
    for (auto v : a)
        var += (v - mean) * (v - mean);
    double h = 0.25 * std::sqrt(var / a.size());
    std::vector<double> crossings;
    bool low = false;
    for (size_t i = 1; i < a.size(); ++i)
    {
        if (a[i] < mean - h)
            low = true;
        if (low && a[i - 1] < mean && a[i] >= mean)
        {
            crossings.push_back(e.t[i - 1] + (mean - a[i - 1]) / (a[i] - a[i - 1]) *
                                                 (e.t[i] - e.t[i - 1]));
            low = false;
        }
    }
    if (crossings.size() < 3)
        return -1;
    return (crossings.size() - 1) / (crossings.back() - crossings.front());
}

// Magnitude of the component at frequency f (Goertzel over a Hann window).
inline double magnitudeAt(const std::vector<float> &x, size_t a, size_t n, double f,
                          double rate)
{
    double w = 2 * M_PI * f / rate, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double h = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        double s0 = h * x[a + i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(s1 * s1 + s2 * s2 - c * s1 * s2, 0.0));
}

// Filter gain in dB at harmonics of f0: tone through the filter over the tone without it.
inline std::vector<std::pair<double, double>> filterResponse(const std::vector<float> &filtered,
                                                             const std::vector<float> &thru,
                                                             double rate, double f0,
                                                             double start, double stop)
{
    size_t a = (size_t)(start * rate), n = (size_t)((stop - start) * rate);
    std::vector<std::pair<double, double>> out;
    for (double f = f0; f < rate / 2 * 0.95; f += f0)
        out.push_back({f, 20 * std::log10(std::max(magnitudeAt(filtered, a, n, f, rate), 1e-12) /
                                          std::max(magnitudeAt(thru, a, n, f, rate), 1e-12))});
    return out;
}

// Frequency (log-interpolated) where a low-pass gain first falls dropDb below its gain at
// the first harmonic, or a high-pass gain (searched from the top) below its top-octave
// median. -1 if it never does.
inline double corner(std::vector<std::pair<double, double>> g, double dropDb, bool highpass)
{
    double ref;
    if (highpass)
    {
        std::vector<double> top;
        for (auto &[f, d] : g)
            if (f > g.back().first / 2)
                top.push_back(d);
        std::nth_element(top.begin(), top.begin() + top.size() / 2, top.end());
        ref = top[top.size() / 2];
        std::reverse(g.begin(), g.end());
    }
    else
        ref = g[0].second;
    double level = ref - dropDb;
    for (size_t i = 1; i < g.size(); ++i)
        if (g[i].second < level)
        {
            double u = (g[i - 1].second - level) / (g[i - 1].second - g[i].second);
            return std::exp(std::log(g[i - 1].first) +
                            u * (std::log(g[i].first) - std::log(g[i - 1].first)));
        }
    return -1;
}
// RMS difference in dB between the filter's gain (filtered over thru) at harmonics first ..
// first + count - 1 of f0 and a reference gain there (tenths of a dB), after removing their
// mean difference (a gain offset).
inline double shapeErrorDb(const std::vector<float> &filtered, const std::vector<float> &thru,
                           double rate, double f0, double start, double stop, int first,
                           int count, const short *gainTenthDb)
{
    size_t a = (size_t)(start * rate), n = (size_t)((stop - start) * rate);
    std::vector<double> d(count);
    double mean = 0;
    for (int i = 0; i < count; ++i)
    {
        double f = (first + i) * f0;
        d[i] = 20 * std::log10(std::max(magnitudeAt(filtered, a, n, f, rate), 1e-12) /
                               std::max(magnitudeAt(thru, a, n, f, rate), 1e-12)) -
               0.1 * gainTenthDb[i];
        mean += d[i] / count;
    }
    double e = 0;
    for (auto v : d)
        e += (v - mean) * (v - mean) / count;
    return std::sqrt(e);
}

// Noise spectrum in half-octave bands centred on 62.5 Hz x 2^(k/2), k = 0..16 (to 16 kHz): the
// mean power spectral density over each band's bins [c / 2^0.25, c 2^0.25), in dB, Welch's
// method with 8192-sample Hann segments overlapping by half, each segment's mean removed (as
// scipy.signal.welch's defaults). Only differences between bands or renders are meaningful.
inline std::vector<double> halfOctaveBandsDb(const std::vector<float> &x, double rate,
                                             double start, double stop)
{
    const size_t N = 8192;
    std::vector<double> psd(N / 2 + 1, 0.0), re(N), im(N);
    size_t a = (size_t)(start * rate), b = std::min((size_t)(stop * rate), x.size());
    for (size_t s = a; s + N <= b; s += N / 2)
    {
        double mean = 0;
        for (size_t i = 0; i < N; ++i)
            mean += x[s + i] / (double)N;
        for (size_t i = 0; i < N; ++i)
        {
            re[i] = (x[s + i] - mean) * (0.5 - 0.5 * std::cos(2 * M_PI * i / N));
            im[i] = 0;
        }
        // iterative radix-2 FFT
        for (size_t i = 1, j = 0; i < N; ++i)
        {
            size_t bit = N >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
            {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t len = 2; len <= N; len <<= 1)
        {
            double ang = -2 * M_PI / len;
            for (size_t i = 0; i < N; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    double wr = std::cos(ang * k), wi = std::sin(ang * k);
                    double ur = re[i + k], ui = im[i + k];
                    double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                    double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                    re[i + k] = ur + vr;
                    im[i + k] = ui + vi;
                    re[i + k + len / 2] = ur - vr;
                    im[i + k + len / 2] = ui - vi;
                }
        }
        for (size_t k = 0; k <= N / 2; ++k)
            psd[k] += re[k] * re[k] + im[k] * im[k];
    }
    std::vector<double> out;
    for (int k = 0; k < 17; ++k)
    {
        double c = 62.5 * std::pow(2.0, k / 2.0), sum = 0;
        int n = 0;
        for (size_t i = 0; i <= N / 2; ++i)
        {
            double f = i * rate / N;
            if (f >= c / std::pow(2.0, 0.25) && f < c * std::pow(2.0, 0.25))
            {
                sum += psd[i];
                ++n;
            }
        }
        out.push_back(10 * std::log10(std::max(sum / std::max(n, 1), 1e-30)));
    }
    return out;
}
} // namespace oscairn_features

#endif
