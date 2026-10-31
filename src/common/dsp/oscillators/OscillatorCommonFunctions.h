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

#ifndef SURGE_SRC_COMMON_DSP_OSCILLATORS_OSCILLATORCOMMONFUNCTIONS_H
#define SURGE_SRC_COMMON_DSP_OSCILLATORS_OSCILLATORCOMMONFUNCTIONS_H

#include "sst/basic-blocks/dsp/OscillatorDriftUnisonCharacter.h"
#include "DSPUtils.h"
#include "SurgeStorage.h"

namespace Surge
{
namespace Oscillator
{
/*
 * sst::basic_blocks::dsp::DriftLFO with the same filter, but drawing from its own generator
 * instead of the C library's global rand(), so drift is reproducible once storage's RNG is
 * seeded and doesn't depend on other instances or threads.
 */
struct DriftLFO
{
    inline void init(bool nzi, uint32_t seed)
    {
        rng = seed;
        d = 0;
        d2 = 0;
        if (nzi)
            d2 = 0.0005f * unif01();
    }

    inline float drift_noise(float &lastval)
    {
        constexpr float filter = 0.00001f;
        constexpr float m = 316.227766017f; // 1.f / sqrt(filter);
        float rand11 = unif01() * 2.f - 1.f;

        lastval = lastval * (1.f - filter) + rand11 * filter;

        return lastval * m;
    }

    inline float next()
    {
        d = drift_noise(d2);
        return d;
    }

    inline float val() const { return d; }

    float d{0}, d2{0};

  private:
    uint32_t rng{0x2545F491u};
    inline float unif01() // LCG (Numerical Recipes), top 24 bits, in [0, 1]
    {
        rng = rng * 1664525u + 1013904223u;
        return (float)(rng >> 8) * (1.f / 16777215.f);
    }
};

template <typename valtype>
using CharacterFilter = sst::basic_blocks::dsp::CharacterFilter<valtype, SurgeStorage>;

template <typename valtype> using UnisonSetup = sst::basic_blocks::dsp::UnisonSetup<valtype>;

/*
 * The Wavetable and Window oscillators share a unison voices parameter whose meaning flips
 * with its own deform: on a sample, SAMPLE_PLAY_COUNT makes the count a play count and the
 * oscillator collapses to one voice. Both the parameter label and the unison detune control
 * have to follow that, and neither is a parameter value, so the two hooks below read the
 * deform and the oscillator's wavetable flags directly. They are shared rather than
 * duplicated per oscillator because the two behave identically here; the instances live in
 * Oscillator.cpp.
 */
struct SampleUnisonDynamicName : public ParameterDynamicNameFunction
{
    const char *getName(const Parameter *p) const override;
};

struct SampleUnisonDetuneDeact : public ParameterDynamicDeactivationFunction
{
    bool getValue(const Parameter *p) const override;
};

extern SampleUnisonDynamicName sampleUnisonDynamicName;
extern SampleUnisonDetuneDeact sampleUnisonDetuneDeact;

// True when this parameter belongs to an oscillator whose wavetable is a sample, which is
// the only case where the play count deform means anything. The menu that offers the deform
// uses it to decide whether to show the choice at all.
bool wavetableIsSample(const Parameter *p);

} // namespace Oscillator
} // namespace Surge

#endif // SURGE_OSCILLATORCOMMONFUNCTIONS_H
