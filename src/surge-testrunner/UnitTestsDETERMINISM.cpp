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

/*
 * With SurgeSynthesizer::seedRandom, the random parts of the engine (oscillator start phases,
 * drift, noise, random modulators, noise LFOs) give the same samples for the same seed, and
 * two instances don't share random state.
 */
#include <vector>

#include "HeadlessUtils.h"
#include "ClassicOscillator.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
std::shared_ptr<SurgeSynthesizer> randomHeavySurge(uint32_t seed)
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    auto &sc = patch.scene[0];
    sc.drift.val.f = 1.f;
    sc.osc[0].retrigger.val.b = false;
    sc.osc[0].p[ClassicOscillator::co_unison_voices].val.i = 4;
    sc.mute_noise.val.b = false;
    sc.lfo[0].shape.val.i = lt_noise;
    surge->setModDepth01(sc.filterunit[0].cutoff.id, ms_lfo1, 0, 0, 0.3f);
    surge->setModDepth01(sc.osc[0].pitch.id, ms_random_bipolar, 0, 0, 0.3f);
    surge->seedRandom(seed);
    return surge;
}

void renderBlock(SurgeSynthesizer &s, int block, std::vector<float> &out)
{
    if (block == 4)
    {
        s.playNote(0, 60, 100, 0);
        s.playNote(0, 64, 100, 0);
    }
    if (block == 150)
        s.releaseNote(0, 60, 0);
    s.process();
    for (int i = 0; i < BLOCK_SIZE; ++i)
    {
        out.push_back(s.output[0][i]);
        out.push_back(s.output[1][i]);
    }
}
} // namespace

TEST_CASE("Seeded Rendering Is Reproducible", "[determinism]")
{
    constexpr int blocks = 300;
    std::vector<float> a, b, c;

    // Interleave two instances block by block, so any shared global random state shows up.
    auto sa = randomHeavySurge(1234), sb = randomHeavySurge(1234), sc = randomHeavySurge(4321);
    for (int i = 0; i < blocks; ++i)
    {
        renderBlock(*sa, i, a);
        renderBlock(*sb, i, b);
        renderBlock(*sc, i, c);
    }

    float peak = 0;
    for (auto v : a)
        peak = std::max(peak, std::fabs(v));
    REQUIRE(peak > 0.01f);
    REQUIRE(a == b);
    REQUIRE(a != c);
}
