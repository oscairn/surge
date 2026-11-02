/*
 * oscairn-analog-rate-match: fits Surge's filter cutoff rate match tables
 * (src/common/dsp/FilterCutoffRateMatchTables.h, storage option filterCutoffRateMatch).
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors. This file is MIT-licensed; the executable links
 * Surge XT and is GPL-3.0-or-later as a whole.
 *
 *   oscairn-analog-rate-match fit <filter> <rate> > dir/<filter>_<rate>.txt
 *   oscairn-analog-rate-match header dir > src/common/dsp/FilterCutoffRateMatchTables.h
 *
 * fit: for each cutoff from -40 to 75 semitones in steps of 1 (and at the filter's clamp at
 * 44.1 kHz), the cutoff at <rate> whose response best matches the response at 44.1 kHz:
 * a saw at note 36 rendered through the plugin with and without the filter, the gain at each
 * harmonic up to 16 kHz within 40 dB of the largest, the RMS difference after removing the mean
 * (as the plugin tests' cutoff checks measure shape). The search scans quarter semitones around
 * the previous cutoff's offset and refines with a parabola. Prints "cutoff matched residual
 * unmatched" per line (residual: the shape error left, unmatched: the error with the cutoff
 * unchanged, both dB). header: writes the tables from every dir/<filter>_<rate>.txt, matched
 * cutoffs made non-decreasing and constant above the clamp (where the 44.1 kHz filter no
 * longer changes). The fit renders with the rate match off (the reference options off: they
 * don't change a filter's response to a steady saw).
 */

#include "RenderNote.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Filter
{
    const char *name;
    int type, subtype;
    double resonance;
    double clamp; // the cutoff (semitones) above which the filter doesn't change at 44.1 kHz
};
// The plan's filter for each reference type; clamps: the legacy ladder at 0.187 x the
// oversampled rate, the SVF at 0.22 x, the OB-Xd at 22 kHz.
const Filter filters[] = {
    {"LPF24", 2, 0, 0.06, 65.5525},  {"LPF18", 3, 2, 0, 62.7389},  {"LPF12", 3, 1, 0, 62.7389},
    {"LPF6", 3, 0, 0, 62.7389},      {"HPF24", 5, 0, 0.05, 65.5525}, {"HPF18", 35, 6, 0, 67.7263},
    {"HPF12", 20, 0, 0, 67.7263},    {"HPF6", 35, 4, 0, 67.7263},  {"BPF12", 23, 0, 0, 65.5525},
    {"BPF6", 6, 0, 0, 65.5525},
};
const int rates[] = {48000, 96000};
const int note = 36;
const double f0 = 440 * std::pow(2.0, (note - 69) / 12.0);

std::vector<float> render(const Filter *f, double cutoff, double rate)
{
    oscairn_analog::RenderSpec s{note, 100, 1, 0.1, rate, {{"a_drift", 0}, {"a_env1_mode", 1}}, {}};
    if (f)
        s.params.insert(s.params.end(), {{"a_filter1_type", f->type},
                                         {"a_filter1_subtype", f->subtype},
                                         {"a_filter1_resonance", f->resonance},
                                         {"a_filter1_cutoff", cutoff}});
    auto lr = oscairn_analog::renderNote(s);
    std::vector<float> m(lr.size() / 2);
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
    return m;
}

double onset(const std::vector<float> &x, double rate)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > 1e-3)
            return i / rate;
    return 0;
}

double magnitude(const std::vector<float> &x, size_t a, size_t n, double f, double rate)
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

// gain in dB at harmonics 1..n over 0.3-0.9 s after the onset
std::vector<double> gains(const Filter &f, double cutoff, double rate,
                          const std::vector<float> &thru, int n)
{
    auto x = render(&f, cutoff, rate);
    double t0 = onset(x, rate);
    size_t a = (size_t)((t0 + 0.3) * rate), len = (size_t)(0.6 * rate);
    std::vector<double> g(n);
    for (int h = 0; h < n; ++h)
        g[h] = 20 * std::log10(std::max(magnitude(x, a, len, (h + 1) * f0, rate), 1e-12) /
                               std::max(magnitude(thru, a, len, (h + 1) * f0, rate), 1e-12));
    return g;
}

int fit(const Filter &f, double rate)
{
    const int n = (int)(16000 / f0);
    auto thru44 = render(nullptr, 0, 44100), thru = render(nullptr, 0, rate);
    std::vector<double> grid;
    for (int v = -40; v <= 75; ++v)
        grid.push_back(v);
    grid.push_back(f.clamp);
    std::sort(grid.begin(), grid.end());
    double offset = 0;
    for (double v : grid)
    {
        auto target = gains(f, v, 44100, thru44, n);
        double top = *std::max_element(target.begin(), target.end());
        std::vector<int> use;
        for (int h = 0; h < n; ++h)
            if (target[h] > top - 40)
                use.push_back(h);
        std::map<double, double> cache;
        auto err = [&](double u) {
            auto it = cache.find(u);
            if (it != cache.end())
                return it->second;
            auto g = gains(f, u, rate, thru, n);
            double m = 0, e = 0;
            for (int h : use)
                m += (g[h] - target[h]) / use.size();
            for (int h : use)
                e += (g[h] - target[h] - m) * (g[h] - target[h] - m) / use.size();
            return cache[u] = std::sqrt(e);
        };
        const double d = 0.25;
        double c = v + offset, lo = c - 2, hi = c + 2, bu = c, be = err(c);
        for (double u = lo; u <= hi + 1e-9; u += d)
            if (err(u) < be - 1e-9)
                be = err(bu = u);
        while (bu <= lo + 1e-9 && lo > c - 8)
            if (err(lo -= d) < be)
                be = err(bu = lo);
        while (bu >= hi - 1e-9 && hi < c + 8)
            if (err(hi += d) < be)
                be = err(bu = hi);
        double em = err(bu - d), ep = err(bu + d), curv = em - 2 * be + ep;
        double u = bu + (curv > 1e-9 ? 0.5 * d * (em - ep) / curv : 0);
        offset = u - v;
        std::printf("%g %.4f %.4f %.4f\n", v, u, err(u), err(v));
        std::fflush(stdout);
    }
    return 0;
}

int header(const std::string &dir)
{
    std::printf("/*\n * Surge XT - a free and open source hybrid synthesizer,\n"
                " * built by Surge Synth Team\n *\n"
                " * Surge XT is released under the GNU General Public Licence v3\n"
                " * or later (GPL-3.0-or-later). The license is found in the \"LICENSE\"\n"
                " * file in the root of this repository, or at\n"
                " * https://www.gnu.org/licenses/gpl-3.0.en.html\n */\n\n"
                "// Generated by src/oscairn-clap/tools/OscairnAnalogRateMatch.cpp (see there).\n"
                "// Do not edit.\n"
                "#ifndef SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCHTABLES_H\n"
                "#define SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCHTABLES_H\n\n"
                "namespace Surge\n{\nnamespace Filter\n{\nnamespace rate_match\n{\n"
                "struct Knot\n{\n    float cutoff, matched;\n};\n\n"
                "struct Table\n{\n    int type, subtype;\n    float rate;\n"
                "    const Knot *knots;\n    int count;\n};\n\n");
    std::vector<std::string> entries;
    for (auto &f : filters)
        for (int rate : rates)
        {
            auto path = dir + "/" + f.name + "_" + std::to_string(rate) + ".txt";
            FILE *in = std::fopen(path.c_str(), "r");
            if (!in)
                throw std::runtime_error("cannot read " + path);
            std::vector<std::pair<double, double>> k;
            double v, u, e, e0, top = -1e9, clamped = 0;
            while (std::fscanf(in, "%lf %lf %lf %lf", &v, &u, &e, &e0) == 4)
                k.push_back({v, u});
            std::fclose(in);
            std::sort(k.begin(), k.end());
            for (auto &[cv, cu] : k)
            {
                top = std::max(top, cu);
                if (std::fabs(cv - f.clamp) < 1e-6)
                    clamped = top;
                cu = cv > f.clamp ? clamped : top;
            }
            std::string var = std::string(f.name) + "_" + std::to_string(rate);
            std::printf("// %s: type %d, subtype %d, fitted at resonance %g, %d Hz\n"
                        "inline constexpr Knot %s[] = {\n",
                        f.name, f.type, f.subtype, f.resonance, rate, var.c_str());
            for (size_t i = 0; i < k.size(); ++i)
                std::printf("%s{%.4ff, %.4ff},%s", i % 4 ? " " : "    ", k[i].first, k[i].second,
                            i % 4 == 3 || i + 1 == k.size() ? "\n" : "");
            std::printf("};\n\n");
            entries.push_back("    {" + std::to_string(f.type) + ", " + std::to_string(f.subtype) +
                              ", " + std::to_string(rate) + ".f, " + var + ", " +
                              std::to_string(k.size()) + "},");
        }
    std::printf("inline constexpr Table tables[] = {\n");
    for (auto &e : entries)
        std::printf("%s\n", e.c_str());
    std::printf("};\n} // namespace rate_match\n} // namespace Filter\n} // namespace Surge\n\n"
                "#endif // SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCHTABLES_H\n");
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 4 && !std::strcmp(argv[1], "fit"))
            for (auto &f : filters)
                if (!std::strcmp(argv[2], f.name))
                    return fit(f, std::atof(argv[3]));
        if (argc == 3 && !std::strcmp(argv[1], "header"))
            return header(argv[2]);
        std::fprintf(stderr,
                     "usage: oscairn-analog-rate-match fit <filter> <rate> | header <dir>\n");
        return 2;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
