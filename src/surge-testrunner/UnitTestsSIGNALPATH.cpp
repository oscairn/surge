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
 * Voice signal path additions: mixer channel polarity and the post-filter route.
 */
#include <cmath>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
// A saw on oscillator 1, seeded, retriggered, no drift, scene highpass off.
std::shared_ptr<SurgeSynthesizer> sawSurge()
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &sc = surge->storage.getPatch().scene[0];
    sc.osc[0].retrigger.val.b = true;
    sc.drift.val.f = 0.f;
    sc.lowcut.deactivated = true;
    surge->seedRandom(1234);
    return surge;
}

std::vector<float> render(SurgeSynthesizer &s, int note = 48, int blocks = 200)
{
    std::vector<float> out;
    for (int b = 0; b < blocks; b++)
    {
        if (b == 2)
            s.playNote(0, note, 100, 0);
        s.process();
        out.insert(out.end(), s.output[0], s.output[0] + BLOCK_SIZE);
    }
    return out;
}

double rms(const std::vector<float> &v, size_t from)
{
    double a = 0;
    for (auto i = from; i < v.size(); i++)
        a += v[i] * v[i];
    return std::sqrt(a / (v.size() - from));
}
} // namespace

TEST_CASE("Mixer Channel Invert Flips Polarity", "[signalpath]")
{
    for (int c : {0, 1})
    {
        INFO("channel " << c);
        auto plain = sawSurge(), flipped = sawSurge();
        for (auto *s : {plain.get(), flipped.get()})
        {
            auto &sc = s->storage.getPatch().scene[0];
            if (c == 1)
            {
                // oscillator 2 alone
                sc.mute_o1.val.b = true;
                sc.mute_o2.val.b = false;
                sc.osc[1].retrigger.val.b = true;
            }
        }
        flipped->storage.getPatch().scene[0].mix_invert[c].val.b = true;
        auto a = render(*plain), b = render(*flipped);
        REQUIRE(rms(a, 2000) > 0.01);
        double worst = 0;
        for (size_t i = 0; i < a.size(); i++)
            worst = std::max(worst, (double)std::fabs(a[i] + b[i]));
        REQUIRE(worst < 1e-6);
    }
}

TEST_CASE("Mixer Post-Filter Route Skips The Filters", "[signalpath]")
{
    auto make = [](bool post, bool filterOn) {
        auto s = sawSurge();
        auto &sc = s->storage.getPatch().scene[0];
        sc.filterunit[0].type.val.i = sst::filters::fut_lpmoog;
        sc.filterunit[0].subtype.val.i = 3;
        sc.filterunit[0].cutoff.val.f = -60; // closed
        sc.filterunit[0].type.deactivated = !filterOn;
        sc.mix_postfilter[0].val.b = post;
        return s;
    };
    auto open = make(false, false), closed = make(false, true), post = make(true, true),
         postOpen = make(true, false);
    auto ro = rms(render(*open), 2000), rc = rms(render(*closed), 2000);
    auto rp = rms(render(*post), 2000);
    REQUIRE(rc < ro * 0.1);
    // past the closed filter, the saw is exactly as loud as with no filter at all
    REQUIRE(rp == Catch::Approx(ro).margin(1e-6));
    // with the filter off, the post-filter route changes nothing
    auto a = render(*make(false, false)), b = render(*postOpen);
    double worst = 0;
    for (size_t i = 0; i < a.size(); i++)
        worst = std::max(worst, (double)std::fabs(a[i] - b[i]));
    REQUIRE(worst < 1e-6);
}

TEST_CASE("Mixer Switches Reset When A Patch Lacks Them", "[signalpath]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    for (auto &sc : patch.scene)
        for (int c = 0; c < 6; c++)
        {
            sc.mix_invert[c].val.b = true;
            sc.mix_postfilter[c].val.b = true;
        }
    // a patch saved before these switches existed
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    for (auto &sc : patch.scene)
        for (int c = 0; c < 6; c++)
        {
            REQUIRE(!sc.mix_invert[c].val.b);
            REQUIRE(!sc.mix_postfilter[c].val.b);
        }

    // and they round trip through a saved patch
    patch.scene[0].mix_invert[2].val.b = true;
    patch.scene[0].mix_postfilter[3].val.b = true;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].mix_invert[2].val.b = false;
    patch.scene[0].mix_postfilter[3].val.b = false;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(patch.scene[0].mix_invert[2].val.b);
    REQUIRE(patch.scene[0].mix_postfilter[3].val.b);
    REQUIRE(!patch.scene[0].mix_invert[0].val.b);
}
