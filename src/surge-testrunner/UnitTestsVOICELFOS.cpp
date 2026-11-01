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
 * Twelve voice LFOs, streamed with the numbering patches had when there were six.
 */
#include <set>
#include <string>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

TEST_CASE("Streamed Modulator Numbers Keep The Six-LFO Numbering", "[voicelfos]")
{
    std::set<int> streamed;
    for (int ms = 0; ms < n_modsources; ms++)
    {
        auto s = modsourceToStreamed(ms);
        REQUIRE(modsourceFromStreamed(s) == ms);
        REQUIRE(s >= 0);
        REQUIRE(s < n_modsources);
        streamed.insert(s);
    }
    REQUIRE(streamed.size() == n_modsources);

    // the numbers patches stored before voice LFOs 7-12 existed
    REQUIRE(modsourceToStreamed(ms_ampeg) == 15);
    REQUIRE(modsourceToStreamed(ms_lfo1) == 17);
    REQUIRE(modsourceToStreamed(ms_lfo6) == 22);
    REQUIRE(modsourceToStreamed(ms_slfo1) == 23);
    REQUIRE(modsourceToStreamed(ms_slfo6) == 28);
    REQUIRE(modsourceToStreamed(ms_timbre) == 29);
    REQUIRE(modsourceToStreamed(ms_latest_key) == 40);
    REQUIRE(modsourceToStreamed(ms_lfo7) == 41);
    REQUIRE(modsourceToStreamed(ms_lfo12) == 46);
    REQUIRE(modsourceFromStreamed(n_modsources) == n_modsources);

    for (int l = 0; l < n_lfos; l++)
    {
        REQUIRE(lfoStreamedIndex(lfoStreamedIndex(l)) == l);
    }
    REQUIRE(lfoStreamedIndex(5) == 5);
    REQUIRE(lfoStreamedIndex(6) == 12);   // voice LFO 7
    REQUIRE(lfoStreamedIndex(n_lfos_voice) == 6); // scene LFO 1
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
    sc.adsr[0].s.val.f = 1.f;
    surge->seedRandom(1234);
    return surge;
}

// a square voice LFO at 4 Hz moving oscillator 1's pitch, from LFO storage index l
std::vector<float> renderWithLfo(int l)
{
    auto surge = sineSurge();
    auto &sc = surge->storage.getPatch().scene[0];
    if (l >= 0)
    {
        sc.lfo[l].shape.val.i = lt_square;
        sc.lfo[l].rate.val.f = 2.f;
        sc.lfo[l].trigmode.val.i = lm_keytrigger;
        REQUIRE(surge->setModDepth01(sc.osc[0].pitch.id, (modsources)(ms_lfo1 + l), 0, 0, 0.3f));
    }
    std::vector<float> out;
    for (int b = 0; b < 400; b++)
    {
        if (b == 2)
            surge->playNote(0, 60, 100, 0);
        surge->process();
        out.insert(out.end(), surge->output[0], surge->output[0] + BLOCK_SIZE);
    }
    return out;
}
} // namespace

TEST_CASE("Voice LFOs 7-12 Modulate Like The First Six", "[voicelfos]")
{
    auto plain = renderWithLfo(-1);
    auto first = renderWithLfo(0);
    REQUIRE(first != plain);
    for (int l = 6; l < n_lfos_voice; l++)
    {
        INFO("Voice LFO " << l + 1);
        REQUIRE(renderWithLfo(l) == first);
    }

    SECTION("Only voices that use them run them")
    {
        auto surge = sineSurge();
        auto &sc = surge->storage.getPatch().scene[0];
        REQUIRE(surge->setModDepth01(sc.osc[0].pitch.id, ms_lfo9, 0, 0, 0.1f));
        surge->playNote(0, 60, 100, 0);
        surge->process();
        auto *v = surge->voices[0].front();
        for (int l = 0; l < n_lfos_voice; l++)
            REQUIRE((v->modsources[ms_lfo1 + l] != nullptr) == (l < 6 || l == 8));
    }
}

TEST_CASE("Voice LFOs 7-12 Stream And Reset", "[voicelfos]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    auto &sc = patch.scene[0];

    REQUIRE(std::string(sc.lfo[6].rate.get_storage_name()) == "a_lfo12_rate");
    REQUIRE(std::string(sc.lfo[n_lfos_voice].rate.get_storage_name()) == "a_lfo6_rate");
    REQUIRE(std::string(sc.lfo[5].rate.get_storage_name()) == "a_lfo5_rate");

    sc.lfo[6].rate.val.f = 1.5f;
    sc.lfo[6].shape.val.i = lt_stepseq;
    patch.stepsequences[0][6].steps[3] = 0.7f;
    strxcpy(patch.LFOBankLabel[0][6][0], "seven", CUSTOM_CONTROLLER_LABEL_SIZE);
    sc.lfo[n_lfos_voice].rate.val.f = -2.f;
    sc.lfo[n_lfos_voice].shape.val.i = lt_stepseq;
    patch.stepsequences[0][n_lfos_voice].steps[2] = 0.4f;
    REQUIRE(surge->setModDepth01(sc.osc[0].pitch.id, ms_lfo7, 0, 0, 0.25f));
    REQUIRE(surge->setModDepth01(sc.filterunit[0].cutoff.id, ms_slfo1, 0, 0, 0.5f));
    REQUIRE(surge->setModDepth01(sc.filterunit[0].cutoff.id, ms_timbre, 0, 0, 0.125f));

    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::string stream((char *)data, (char *)data + size);

    // the older numbers for the older sources; voice LFO 7 after them
    REQUIRE(stream.find("<a_lfo12_rate") != std::string::npos);
    REQUIRE(stream.find("source=\"41\"") != std::string::npos);
    REQUIRE(stream.find("source=\"23\"") != std::string::npos);
    REQUIRE(stream.find("source=\"29\"") != std::string::npos);
    REQUIRE(stream.find("source=\"35\"") == std::string::npos);
    REQUIRE(stream.find("<sequence scene=\"0\" i=\"12\"") != std::string::npos);
    REQUIRE(stream.find("<sequence scene=\"0\" i=\"6\"") != std::string::npos);
    REQUIRE(stream.find("<label lfo=\"12\" idx=\"0\" scene=\"0\" v=\"seven\"") != std::string::npos);

    // loading a patch without them resets voice LFOs 7-12
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(sc.lfo[6].rate.val.f == sc.lfo[6].rate.val_default.f);
    REQUIRE(sc.lfo[6].shape.val.i == sc.lfo[6].shape.val_default.i);
    REQUIRE(!sc.lfo[6].rate.deactivated);
    REQUIRE(patch.stepsequences[0][6].steps[3] == 0.f);
    REQUIRE(patch.LFOBankLabel[0][6][0][0] == 0);
    REQUIRE(surge->getModulationsFromSource(0, ms_lfo7).empty());

    surge->loadRaw(stream.data(), (int)stream.size(), false);
    REQUIRE(sc.lfo[6].rate.val.f == 1.5f);
    REQUIRE(sc.lfo[6].shape.val.i == lt_stepseq);
    REQUIRE(patch.stepsequences[0][6].steps[3] == 0.7f);
    REQUIRE(std::string(patch.LFOBankLabel[0][6][0]) == "seven");
    REQUIRE(sc.lfo[n_lfos_voice].rate.val.f == -2.f);
    REQUIRE(patch.stepsequences[0][n_lfos_voice].steps[2] == 0.4f);
    REQUIRE(surge->getModDepth01(sc.osc[0].pitch.id, ms_lfo7, 0, 0) == Catch::Approx(0.25f));
    REQUIRE(surge->getModDepth01(sc.filterunit[0].cutoff.id, ms_slfo1, 0, 0) ==
            Catch::Approx(0.5f));
    REQUIRE(surge->getModDepth01(sc.filterunit[0].cutoff.id, ms_timbre, 0, 0) ==
            Catch::Approx(0.125f));
}
