/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

#ifndef SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCH_H
#define SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCH_H

/*
 * Cutoff rate match (SurgeStorage::filterCutoffRateMatch): Surge's filters compute their
 * coefficients from the cutoff over the sample rate, and several clamp the cutoff at a fraction
 * of the rate, so near the top of the range the same cutoff responds differently above 44.1 kHz
 * than at 44.1 kHz. For the filter types in FilterCutoffRateMatchTables.h, rateMatchedCutoff
 * gives the cutoff whose response at the running rate best matches the response at 44.1 kHz with
 * the given cutoff (in shape, at the harmonics of a saw at 65.4 Hz up to 16 kHz), from the tables
 * of the fitted rates (see below for other rates). Other types are returned unchanged. The tables were fitted at one
 * resonance per type (listed there).
 */

#include <algorithm>
#include <cmath>

#include "FilterCutoffRateMatchTables.h"

namespace Surge
{
namespace Filter
{
namespace rate_match
{
// The matched cutoff from one table
inline float matchedBy(const Table &t, float cutoff)
{
    const Knot *k = t.knots;
    const int n = t.count;
    if (cutoff <= k[0].cutoff)
        return cutoff + k[0].matched - k[0].cutoff;
    if (cutoff >= k[n - 1].cutoff)
    {
        // the last segment's slope: 0 where the filter clamps at 44.1 kHz
        float s = (k[n - 1].matched - k[n - 2].matched) / (k[n - 1].cutoff - k[n - 2].cutoff);
        return k[n - 1].matched + s * (cutoff - k[n - 1].cutoff);
    }
    auto hi = std::upper_bound(k, k + n, cutoff,
                               [](float c, const Knot &x) { return c < x.cutoff; });
    auto lo = hi - 1;
    float a = (cutoff - lo->cutoff) / (hi->cutoff - lo->cutoff);
    return lo->matched + a * (hi->matched - lo->matched);
}
} // namespace rate_match

// Between fitted rates (and between 44.1 kHz, where the cutoff is unchanged, and the lowest),
// the matched cutoffs are interpolated linearly in log rate; above the highest fitted rate its
// table is used. Below 44.1 kHz the cutoff is unchanged.
inline float rateMatchedCutoff(float cutoff, int type, int subtype, float sampleRate)
{
    using namespace rate_match;
    if (sampleRate <= 44101.f)
        return cutoff;
    const Table *below = nullptr, *above = nullptr; // nearest fitted rates either side
    for (const auto &t : tables)
    {
        if (t.type != type || t.subtype != subtype)
            continue;
        if (std::fabs(t.rate - sampleRate) <= 1.f)
            return matchedBy(t, cutoff);
        if (t.rate < sampleRate && (!below || t.rate > below->rate))
            below = &t;
        if (t.rate > sampleRate && (!above || t.rate < above->rate))
            above = &t;
    }
    if (!below && !above)
        return cutoff; // no table for this type
    if (!above)
        return matchedBy(*below, cutoff);
    float loRate = below ? below->rate : 44100.f;
    float lo = below ? matchedBy(*below, cutoff) : cutoff, hi = matchedBy(*above, cutoff);
    float w = std::log2(sampleRate / loRate) / std::log2(above->rate / loRate);
    return lo + w * (hi - lo);
}
} // namespace Filter
} // namespace Surge

#endif // SURGE_SRC_COMMON_DSP_FILTERCUTOFFRATEMATCH_H
