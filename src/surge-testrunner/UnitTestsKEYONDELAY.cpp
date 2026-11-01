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
 * Key-on delay: a note starts its scene's delay after its note-on, or never if released first.
 */
#include <algorithm>
#include <cmath>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
std::shared_ptr<SurgeSynthesizer> delaySurge(float delay, bool on = true)
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &sc = surge->storage.getPatch().scene[0];
    sc.keyon_delay.val.f = delay;
    sc.keyon_delay.deactivated = !on;
    surge->process();
    return surge;
}

// the first block (counted from the note-on's) with output, or -1
int firstSoundingBlock(SurgeSynthesizer &s, int blocks)
{
    for (int b = 0; b < blocks; b++)
    {
        s.process();
        auto peak = std::max(*std::max_element(s.output[0], s.output[0] + BLOCK_SIZE),
                             -*std::min_element(s.output[0], s.output[0] + BLOCK_SIZE));
        if (peak > 1e-9)
            return b;
    }
    return -1;
}

bool endedThisBlock(SurgeSynthesizer &s, int32_t id)
{
    for (int i = 0; i < s.hostNoteEndedDuringBlockCount; i++)
        if (s.endedHostNoteIds[i] == id)
            return true;
    return false;
}
} // namespace

TEST_CASE("Key-On Delay", "[keyondelay]")
{
    const int quarterSecond = (int)std::lround(0.25 * 48000 / BLOCK_SIZE); // 375 blocks

    SECTION("Off by default: the note starts at once")
    {
        auto s = Surge::Headless::createSurge(48000);
        REQUIRE(s->storage.getPatch().scene[0].keyon_delay.deactivated);
        s->playNote(0, 60, 100, 0, 1);
        REQUIRE(firstSoundingBlock(*s, 10) == 0);
        auto d = delaySurge(-2, false);
        d->playNote(0, 60, 100, 0, 1);
        REQUIRE(firstSoundingBlock(*d, 10) == 0);
    }
    SECTION("The note starts after the delay")
    {
        auto s = delaySurge(-2); // 0.25 s
        s->playNote(0, 60, 100, 0, 1);
        REQUIRE(firstSoundingBlock(*s, 2 * quarterSecond) == quarterSecond - 1);
        REQUIRE(s->delayedNoteCount == 0);
    }
    SECTION("Tempo sync scales the delay with the tempo")
    {
        auto s = delaySurge(-2);
        s->storage.getPatch().scene[0].keyon_delay.temposync = true;
        s->time_data.tempo = 60;
        s->process();
        s->playNote(0, 60, 100, 0, 1);
        REQUIRE(firstSoundingBlock(*s, 3 * quarterSecond) == 2 * quarterSecond - 1);
    }
    SECTION("A note released before it starts never sounds, and its end is reported")
    {
        auto s = delaySurge(-2);
        s->playNote(0, 60, 100, 0, 7);
        for (int b = 0; b < 10; b++)
            s->process();
        s->releaseNote(0, 60, 0, 7);
        REQUIRE(s->delayedNoteCount == 0);
        s->process();
        REQUIRE(endedThisBlock(*s, 7));
        REQUIRE(firstSoundingBlock(*s, 2 * quarterSecond) == -1);

        // the same by host note ID, and by all notes off
        s->playNote(0, 61, 100, 0, 8);
        s->releaseNoteByHostNoteID(8, 0);
        s->process();
        REQUIRE(endedThisBlock(*s, 8));
        s->playNote(0, 62, 100, 0, 9);
        s->allNotesOff();
        s->process();
        REQUIRE(endedThisBlock(*s, 9));
        REQUIRE(firstSoundingBlock(*s, 2 * quarterSecond) == -1);
    }
    SECTION("Releasing one key leaves another key's waiting note")
    {
        auto s = delaySurge(-2);
        s->playNote(0, 60, 100, 0, 1);
        s->playNote(0, 64, 100, 0, 2);
        s->releaseNote(0, 60, 0, 1);
        REQUIRE(s->delayedNoteCount == 1);
        REQUIRE(firstSoundingBlock(*s, 2 * quarterSecond) == quarterSecond - 1);
    }
    SECTION("A patch without the parameter loads with the delay off")
    {
        auto s = delaySurge(-2);
        REQUIRE(s->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
        REQUIRE(s->storage.getPatch().scene[0].keyon_delay.deactivated);
        REQUIRE(!s->storage.getPatch().scene[0].keyon_delay.temposync);
    }
}
