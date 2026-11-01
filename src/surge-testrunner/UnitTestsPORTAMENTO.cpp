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
 * Portamento octave limit: a glide over more than an octave starts an octave from the target.
 */
#include <cmath>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
// The gliding key (state.pkey) of a mono legato voice, once per block, from the second note's
// start: portamento 2^-1 s, linear curve.
std::vector<float> glide(int from, int to, bool limit, bool constantRate)
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &sc = surge->storage.getPatch().scene[0];
    sc.polymode.val.i = pm_mono_st;
    sc.portamento.val.f = -1;
    sc.portamento.porta_curve = porta_lin;
    sc.portamento.porta_constrate = constantRate;
    sc.portamento_octave_limit.val.b = limit;
    for (int i = 0; i < 4; ++i)
        surge->process();
    surge->playNote(0, from, 100, 0);
    for (int i = 0; i < 2000; ++i) // long enough for the first note's own glide to end
        surge->process();
    surge->playNote(0, to, 100, 0);
    std::vector<float> keys;
    for (int i = 0; i < 2000; ++i)
    {
        surge->process();
        for (auto v : surge->voices[0])
            if (v->state.gate)
                keys.push_back(v->state.pkey);
    }
    return keys;
}

// blocks until the key is within 0.01 of the target
size_t blocksToReach(const std::vector<float> &k, float target)
{
    for (size_t i = 0; i < k.size(); ++i)
        if (std::fabs(k[i] - target) < 0.01)
            return i;
    return k.size();
}
} // namespace

TEST_CASE("Portamento Octave Limit", "[portamento]")
{
    // half a second at 48 kHz in 32-sample blocks: 750 blocks
    const float octaveBlocks = 0.5f * 48000 / BLOCK_SIZE;
    for (bool constantRate : {false, true})
    {
        INFO("constant rate " << constantRate);
        for (auto [from, to] : {std::pair{48, 72}, {48, 84}, {84, 48}, {48, 65}})
        {
            INFO(from << " to " << to);
            float dir = to > from ? 1.f : -1.f;
            auto k = glide(from, to, true, constantRate);
            REQUIRE(k.size() == 2000);
            // it starts an octave from the target, not at the first note
            REQUIRE(k[0] == Catch::Approx(to - 12 * dir).margin(0.1));
            // and takes one octave's time in both modes
            REQUIRE(blocksToReach(k, to) == Catch::Approx(octaveBlocks).margin(3));
            // off: it starts at the first note
            auto off = glide(from, to, false, constantRate);
            REQUIRE(off[0] == Catch::Approx(from).margin(0.1));
        }
        // an octave or less is unchanged
        for (int to : {55, 60})
        {
            INFO("48 to " << to);
            auto on = glide(48, to, true, constantRate), off = glide(48, to, false, constantRate);
            REQUIRE(on == off);
        }
    }

    // reset to off when a patch lacks it, and round trips
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    patch.scene[1].portamento_octave_limit.val.b = true;
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(!patch.scene[1].portamento_octave_limit.val.b);
    patch.scene[0].portamento_octave_limit.val.b = true;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].portamento_octave_limit.val.b = false;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(patch.scene[0].portamento_octave_limit.val.b);
}
