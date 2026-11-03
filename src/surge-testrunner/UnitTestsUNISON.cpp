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
 * Voice unison: a note starts several voices, detuned and panned symmetrically.
 */
#include <algorithm>
#include <cmath>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
std::shared_ptr<SurgeSynthesizer> unisonSurge(int n, float detune, float spread)
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &sc = surge->storage.getPatch().scene[0];
    sc.osc[0].retrigger.val.b = true;
    sc.drift.val.f = 0.f;
    sc.unison_voices.val.i = n;
    sc.unison_detune.val.f = detune;
    sc.unison_spread.val.f = spread;
    surge->seedRandom(1234);
    return surge;
}

std::vector<SurgeVoice *> voicesOf(SurgeSynthesizer &s)
{
    std::vector<SurgeVoice *> v(s.voices[0].begin(), s.voices[0].end());
    std::sort(v.begin(), v.end(),
              [](auto *a, auto *b) { return a->state.detune < b->state.detune; });
    return v;
}

int countNoteEnds(SurgeSynthesizer &s, int32_t id)
{
    int n = 0;
    for (int i = 0; i < s.hostNoteEndedDuringBlockCount; i++)
        n += s.endedHostNoteIds[i] == id;
    return n;
}
} // namespace

TEST_CASE("Unison Voices Are Detuned And Panned Symmetrically", "[unison]")
{
    auto surge = unisonSurge(4, 0.08f, 0.3f);
    surge->process();
    surge->playNote(0, 60, 100, 0, 77);
    surge->process();
    auto v = voicesOf(*surge);
    REQUIRE(v.size() == 4);
    float k[4] = {-2.f, -2.f / 3, 2.f / 3, 2.f};
    for (int i = 0; i < 4; i++)
    {
        INFO("voice " << i);
        REQUIRE(v[i]->state.detune == Catch::Approx(0.08f * k[i]));
        REQUIRE(v[i]->state.unisonPan == Catch::Approx(std::clamp(0.3f * k[i], -1.f, 1.f)));
        REQUIRE(v[i]->state.unisonGain == Catch::Approx(0.5f));
        REQUIRE(v[i]->state.unisonGroup == v[0]->state.unisonGroup);
        REQUIRE(v[i]->host_note_id == 77);
    }

    // one note end for the four voices, after all have finished
    surge->releaseNote(0, 60, 0, 77);
    int ends = 0;
    for (int b = 0; b < 4000 && !surge->voices[0].empty(); b++)
    {
        surge->process();
        ends += countNoteEnds(*surge, 77);
    }
    for (int b = 0; b < 4; b++)
    {
        surge->process();
        ends += countNoteEnds(*surge, 77);
    }
    REQUIRE(surge->voices[0].empty());
    REQUIRE(ends == 1);
}

TEST_CASE("Unison Of One Voice Changes Nothing", "[unison]")
{
    auto render = [](int n) {
        auto surge = unisonSurge(n, 0.2f, 0.7f);
        std::vector<float> out;
        for (int b = 0; b < 100; b++)
        {
            if (b == 2)
                surge->playNote(0, 60, 100, 0);
            surge->process();
            out.insert(out.end(), surge->output[0], surge->output[0] + BLOCK_SIZE);
            out.insert(out.end(), surge->output[1], surge->output[1] + BLOCK_SIZE);
        }
        return out;
    };
    auto def = Surge::Headless::createSurge(48000);
    REQUIRE(def->storage.getPatch().scene[0].unison_voices.val.i == 1);
    REQUIRE(render(1) == render(1));
    auto surge = unisonSurge(1, 0.2f, 0.7f);
    surge->playNote(0, 60, 100, 0);
    surge->process();
    REQUIRE(surge->voices[0].size() == 1);
    auto *v = surge->voices[0].front();
    REQUIRE(v->state.detune == 0.f);
    REQUIRE(v->state.unisonPan == 0.f);
    REQUIRE(v->state.unisonGain == 1.f);
    REQUIRE(v->state.unisonGroup == -1);
}

TEST_CASE("Unison Spread Attenuates The Far Side", "[unison]")
{
    // two voices without detune, hard apart: each side hears one voice at 1/sqrt(2)
    auto level = [](int n, float spread) {
        auto surge = unisonSurge(n, 0.f, spread);
        double l = 0, r = 0;
        for (int b = 0; b < 400; b++)
        {
            if (b == 2)
                surge->playNote(0, 60, 100, 0);
            surge->process();
            if (b >= 200)
                for (int i = 0; i < BLOCK_SIZE; i++)
                {
                    l += surge->output[0][i] * surge->output[0][i];
                    r += surge->output[1][i] * surge->output[1][i];
                }
        }
        return std::make_pair(std::sqrt(l), std::sqrt(r));
    };
    auto one = level(1, 0.f);
    auto two = level(2, 0.f);
    auto apart = level(2, 1.f);
    // coherent voices: sqrt(2) the level of one voice
    REQUIRE(two.first / one.first == Catch::Approx(std::sqrt(2.0)).margin(1e-3));
    REQUIRE(apart.first / one.first == Catch::Approx(std::sqrt(0.5)).margin(1e-3));
    REQUIRE(apart.second / one.second == Catch::Approx(std::sqrt(0.5)).margin(1e-3));
}

TEST_CASE("Unison Fills The Polyphony Limit", "[unison]")
{
    auto surge = unisonSurge(4, 0.1f, 0.f);
    surge->storage.getPatch().polylimit.val.i = 6;
    surge->playNote(0, 60, 100, 0);
    surge->process();
    surge->playNote(0, 64, 100, 0);
    for (int b = 0; b < 200; b++)
        surge->process();
    // the new note's four voices play; two of the first note's are stolen
    int gated = 0, newNote = 0;
    for (auto *v : surge->voices[0])
    {
        bool on = v->state.gate && !v->state.uberrelease;
        gated += on;
        newNote += on && v->state.key == 64;
    }
    REQUIRE(gated == 6);
    REQUIRE(newNote == 4);
}

TEST_CASE("Unison In Mono Modes", "[unison]")
{
    for (auto mode : {pm_mono, pm_mono_st, pm_mono_fp, pm_mono_st_fp})
    {
        INFO("play mode " << mode);
        auto surge = unisonSurge(2, 0.1f, 0.5f);
        surge->storage.getPatch().scene[0].polymode.val.i = mode;
        surge->playNote(0, 60, 100, 0, 1);
        surge->process();
        surge->playNote(0, 64, 100, 0, 2);
        for (int b = 0; b < 200; b++)
            surge->process();
        int gated = 0;
        for (auto *v : surge->voices[0])
        {
            if (v->state.gate && !v->state.uberrelease)
            {
                gated++;
                REQUIRE(v->state.key == 64);
                REQUIRE(std::fabs(v->state.detune) == Catch::Approx(0.1f));
            }
        }
        REQUIRE(gated == 2);
    }
}

TEST_CASE("Unison Parameters Reset When A Patch Lacks Them", "[unison]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    for (auto &sc : patch.scene)
    {
        sc.unison_voices.val.i = 4;
        sc.unison_detune.val.f = 0.3f;
        sc.unison_spread.val.f = 0.5f;
    }
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    for (auto &sc : patch.scene)
    {
        REQUIRE(sc.unison_voices.val.i == 1);
        REQUIRE(sc.unison_detune.val.f == 0.f);
        REQUIRE(sc.unison_spread.val.f == 0.f);
    }
    patch.scene[0].unison_voices.val.i = 4;
    patch.scene[0].unison_detune.val.f = 0.25f;
    patch.scene[0].unison_spread.val.f = 0.5f;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].unison_voices.val.i = 1;
    patch.scene[0].unison_detune.val.f = 0;
    patch.scene[0].unison_spread.val.f = 0;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(patch.scene[0].unison_voices.val.i == 4);
    REQUIRE(patch.scene[0].unison_detune.val.f == 0.25f);
    REQUIRE(patch.scene[0].unison_spread.val.f == 0.5f);
}

TEST_CASE("Unison Mono Returns To The Held Key", "[unison]")
{
    auto surge = unisonSurge(4, 0.1f, 0.5f);
    surge->storage.getPatch().scene[0].polymode.val.i = pm_mono;
    auto gatedKeys = [&]() {
        std::vector<int> k;
        for (auto *v : surge->voices[0])
            if (v->state.gate && !v->state.uberrelease)
                k.push_back(v->state.key);
        return k;
    };
    surge->playNote(0, 48, 100, 0);
    surge->process();
    surge->playNote(0, 55, 100, 0);
    surge->process();
    surge->releaseNote(0, 55, 0);
    for (int b = 0; b < 10; b++)
        surge->process();
    REQUIRE(gatedKeys() == std::vector<int>(4, 48));

    surge->releaseNote(0, 48, 0);
    for (int b = 0; b < 10; b++)
        surge->process();
    REQUIRE(gatedKeys().empty());
}
