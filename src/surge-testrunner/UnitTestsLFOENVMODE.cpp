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
 * The LFO envelope's analog mode follows the scene envelopes' analog mode.
 */
#include <cmath>
#include <string>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
// filter EG and voice LFO 7's envelope, per block, through a note held for `hold` blocks
std::vector<std::pair<float, float>> envelopes(bool analogLfo, float a, float d, float s, float r,
                                               int hold, float delay = -8.f, bool clamp = false)
{
    auto surge = Surge::Headless::createSurge(48000);
    surge->storage.analogEnvelopeAttackClamp = clamp;
    auto &sc = surge->storage.getPatch().scene[0];
    auto &eg = sc.adsr[1];
    eg.mode.val.b = true;
    eg.a.val.f = a;
    eg.d.val.f = d;
    eg.s.val.f = s;
    eg.r.val.f = r;
    auto &lfo = sc.lfo[6];
    lfo.shape.val.i = lt_envelope;
    lfo.envmode.val.b = analogLfo;
    lfo.delay.val.f = delay;
    lfo.attack.val.f = a;
    lfo.decay.val.f = d;
    lfo.sustain.val.f = s;
    lfo.release.val.f = r;
    REQUIRE(surge->setModDepth01(sc.osc[1].pitch.id, ms_lfo7, 0, 0, 0.1f));

    std::vector<std::pair<float, float>> out;
    // the synth decides which LFOs to process at the start of a block
    for (int b = 0; b < 4; b++)
        surge->process();
    surge->playNote(0, 60, 100, 0);
    for (int b = 0; b < hold + 300; b++)
    {
        if (b == hold)
            surge->releaseNote(0, 60, 0);
        surge->process();
        if (surge->voices[0].empty())
            break;
        auto *v = surge->voices[0].front();
        out.emplace_back(v->modsources[ms_filtereg]->get_output(0),
                         v->modsources[ms_lfo7]->get_output(2));
    }
    return out;
}
} // namespace

TEST_CASE("LFO Envelope Analog Mode Follows The Scene Envelopes", "[lfoenvmode]")
{
    for (auto [a, d, s, r] : {std::tuple{-2.f, -1.f, 0.5f, -1.5f}, {-8.f, 0.f, 0.f, -3.f},
                              {0.5f, -4.f, 0.9f, 0.f}})
    {
        INFO("attack " << a << " decay " << d << " sustain " << s << " release " << r);
        auto e = envelopes(true, a, d, s, r, 1500);
        REQUIRE(e.size() > 1510);
        float largest = 0;
        for (auto &[f, l] : e)
        {
            REQUIRE(l == Catch::Approx(f).margin(1e-6));
            largest = std::max(largest, f);
        }
        REQUIRE(largest > 0.5f);

        // the digital stages are a different curve
        auto dig = envelopes(false, a, d, s, r, 1500);
        float diff = 0;
        for (size_t i = 0; i < std::min(dig.size(), e.size()); i++)
            diff = std::max(diff, std::fabs(dig[i].second - e[i].second));
        REQUIRE(diff > 0.05f);
    }

    SECTION("Delay holds the analog envelope at zero, then it starts")
    {
        auto e = envelopes(true, -2.f, -1.f, 0.5f, -1.5f, 150, -2.f); // 0.25 s, 375 blocks
        REQUIRE(e.size() > 100);
        for (int i = 0; i < 100; i++)
            REQUIRE(e[i].second == 0.f);
    }
}

TEST_CASE("Analog Envelope Attack Clamp", "[lfoenvmode]")
{
    auto peak = [](const std::vector<std::pair<float, float>> &e) {
        float f = 0, l = 0;
        for (auto &[a, b] : e)
            f = std::max(f, a), l = std::max(l, b);
        return std::pair{f, l};
    };
    // the shortest attack overshoots by about 40 % without the clamp
    auto [f0, l0] = peak(envelopes(true, -8.f, 2.f, 1.f, -3.f, 400));
    REQUIRE(f0 > 1.3f);
    REQUIRE(l0 > 1.3f);
    for (float a : {-8.f, -6.f, -2.f})
    {
        INFO("attack " << a);
        auto e = envelopes(true, a, 2.f, 1.f, -3.f, 400, -8.f, true);
        auto [f, l] = peak(e);
        REQUIRE(f == 1.f); // held at 1 when it gets there, then at the full sustain
        REQUIRE(l == 1.f);
        for (auto &[a1, b] : e)
            REQUIRE(b == Catch::Approx(a1).margin(1e-6));
    }
    // a lower sustain and a fast decay: from 1 (in the block before the first one read here)
    // down to the sustain
    auto e = envelopes(true, -8.f, -3.f, 0.5f, -3.f, 400, -8.f, true);
    REQUIRE(peak(e).first <= 1.f);
    REQUIRE(peak(e).first > 0.95f);
    REQUIRE(e[399].first == Catch::Approx(0.25f).margin(1e-3));
}

TEST_CASE("LFO Envelope Mode Streams And Resets", "[lfoenvmode]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    auto &sc = patch.scene[0];
    REQUIRE(std::string(sc.lfo[6].envmode.get_storage_name()) == "a_lfo12_envmode");
    REQUIRE(std::string(sc.lfo[n_lfos_voice].envmode.get_storage_name()) == "a_lfo6_envmode");
    REQUIRE(!sc.lfo[0].envmode.val.b);

    sc.lfo[2].envmode.val.b = true;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);

    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(!sc.lfo[2].envmode.val.b);
    sc.lfo[2].envmode.val.b = true;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(sc.lfo[2].envmode.val.b);
    sc.lfo[2].envmode.val.b = false;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(sc.lfo[2].envmode.val.b);
}
