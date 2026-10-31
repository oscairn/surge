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
 * Segment drift: random pitch and cutoff offsets, linear between nodes segment-time apart.
 */
#include <cmath>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

TEST_CASE("Segment Drift Generator", "[segmentdrift]")
{
    SECTION("Stationary distribution: linear between normal nodes")
    {
        // deviation of a point uniformly placed between two independent unit normals:
        // sqrt(mean of (1-p)^2 + p^2) = sqrt(2/3)
        double s = 0, s2 = 0;
        const int n = 20000;
        for (int i = 0; i < n; i++)
        {
            SurgeVoice::SegmentDrift d;
            d.init(0x9E3779B9u * (i + 1));
            double v = d.next(0.f);
            s += v;
            s2 += v * v;
        }
        double mean = s / n, sd = std::sqrt(s2 / n - mean * mean);
        REQUIRE(std::fabs(mean) < 0.03);
        REQUIRE(sd == Catch::Approx(std::sqrt(2.0 / 3)).margin(0.02));
    }
    SECTION("Nodes every segment, straight lines between them")
    {
        SurgeVoice::SegmentDrift d;
        d.init(1234);
        d.pos = 0.f;
        std::vector<float> v;
        for (int i = 0; i < 1000; i++)
            v.push_back(d.next(0.01f));
        int kinks = 0;
        for (size_t i = 1; i + 1 < v.size(); i++)
            kinks += std::fabs(v[i + 1] - 2 * v[i] + v[i - 1]) > 1e-4f;
        // ten segments; a node between samples shows as up to two bent steps
        REQUIRE(kinks >= 9);
        REQUIRE(kinks <= 20);
    }
}

namespace
{
std::shared_ptr<SurgeSynthesizer> sineSurge()
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &sc = surge->storage.getPatch().scene[0];
    sc.osc[0].type.val.i = ot_sine;
    surge->storage.getPatch().update_controls(false, &sc.osc[0]);
    sc.osc[0].retrigger.val.b = true;
    sc.drift.val.f = 0.f;
    sc.lowcut.deactivated = true;
    sc.adsr[0].s.val.f = 1.f;
    surge->seedRandom(1234);
    return surge;
}

// frequency of the output from upward zero crossings over blocks [from, to)
double frequency(SurgeSynthesizer &s, int from, int to)
{
    std::vector<float> out;
    for (int b = 0; b < to; b++)
    {
        if (b == 2)
            s.playNote(0, 69, 100, 0);
        s.process();
        if (b >= from)
            out.insert(out.end(), s.output[0], s.output[0] + BLOCK_SIZE);
    }
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = 1; i < out.size(); i++)
    {
        if (out[i - 1] < 0 && out[i] >= 0)
        {
            double t = i - 1 + out[i - 1] / (out[i - 1] - out[i]);
            if (first < 0)
                first = t;
            else
                n++;
            last = t;
        }
    }
    return n * 48000.0 / (last - first);
}
} // namespace

TEST_CASE("Segment Drift Moves Pitch And Cutoff", "[segmentdrift]")
{
    SECTION("Off by default")
    {
        auto surge = sineSurge();
        auto &sc = surge->storage.getPatch().scene[0];
        REQUIRE(sc.segdrift_pitch.val.f == 0.f);
        REQUIRE(sc.segdrift_offset.val.f == 0.f);
        REQUIRE(sc.segdrift_cutoff.val.f == 0.f);
        surge->playNote(0, 60, 100, 0);
        surge->process();
        auto *v = surge->voices[0].front();
        REQUIRE(!v->segDriftOn);
        for (auto x : v->segDriftValue)
            REQUIRE(x == 0.f);
        REQUIRE(frequency(*sineSurge(), 50, 400) == Catch::Approx(440.0).margin(0.05));
    }
    SECTION("Offset shifts every oscillator")
    {
        auto surge = sineSurge();
        surge->storage.getPatch().scene[0].segdrift_offset.val.f = 0.5f; // 50 cents
        double f = frequency(*surge, 50, 400);
        REQUIRE(1200 * std::log2(f / 440.0) == Catch::Approx(50.0).margin(0.2));
    }
    SECTION("Depth scales pitch per oscillator and cutoff per filter, independently")
    {
        auto surge = sineSurge();
        auto &sc = surge->storage.getPatch().scene[0];
        sc.segdrift_pitch.val.f = 0.3f;
        sc.segdrift_cutoff.val.f = 2.f;
        surge->playNote(0, 60, 100, 0);
        surge->process();
        auto *v = surge->voices[0].front();
        REQUIRE(v->segDriftOn);
        for (int i = 0; i < SurgeVoice::n_segdrift; i++)
        {
            float scale = i < n_oscs ? 0.3f : 2.f;
            auto &d = v->segDrift[i];
            REQUIRE(v->segDriftValue[i] ==
                    Catch::Approx(scale * (d.from + (d.to - d.from) * d.pos)).margin(1e-5));
            for (int j = 0; j < i; j++)
                REQUIRE(v->segDriftValue[i] != v->segDriftValue[j]);
        }
    }
}

TEST_CASE("Segment Drift Parameters Reset When A Patch Lacks Them", "[segmentdrift]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    for (auto &sc : patch.scene)
    {
        sc.segdrift_pitch.val.f = 0.3f;
        sc.segdrift_offset.val.f = 0.1f;
        sc.segdrift_cutoff.val.f = 3.f;
        sc.segdrift_time.val.f = 1.f;
    }
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    for (auto &sc : patch.scene)
    {
        REQUIRE(sc.segdrift_pitch.val.f == 0.f);
        REQUIRE(sc.segdrift_offset.val.f == 0.f);
        REQUIRE(sc.segdrift_cutoff.val.f == 0.f);
        REQUIRE(sc.segdrift_time.val.f == 2.5f);
    }
    patch.scene[0].segdrift_pitch.val.f = 0.25f;
    patch.scene[0].segdrift_time.val.f = 1.5f;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].segdrift_pitch.val.f = 0;
    patch.scene[0].segdrift_time.val.f = 2.5f;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(patch.scene[0].segdrift_pitch.val.f == 0.25f);
    REQUIRE(patch.scene[0].segdrift_time.val.f == 1.5f);
}
