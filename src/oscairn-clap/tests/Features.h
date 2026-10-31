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
} // namespace oscairn_features

#endif
