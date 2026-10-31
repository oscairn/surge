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
 * Reference renders of the factory patches, to show that engine changes leave existing
 * patches sounding the same.
 *
 * The renders are generated, never committed. Record them from a known-good build, then
 * compare later builds against them:
 *
 *   SURGE_REFERENCE_DIR=/some/dir SURGE_REFERENCE_RECORD=1 surge-testrunner "[reference]"
 *   SURGE_REFERENCE_DIR=/some/dir surge-testrunner "[reference]"
 *
 * Without SURGE_REFERENCE_DIR the test is skipped. Record several times (three or more):
 * recording renders each patch twice, and later recording runs also compare with the files
 * already there, so patches that don't render the same within a process or across processes
 * (some effects and Lua formulas keep their own random state) are left out and listed in
 * nondeterministic.txt. The voice path itself is reproducible once seeded.
 */
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#include "HeadlessUtils.h"
#include "catch2/catch_amalgamated.hpp"

namespace
{
constexpr float referenceTolerance = 1e-5f; // as in UnitTestsGOLDEN.cpp
constexpr int referenceRate = 48000;
constexpr int holdBlocks = 0.75 * referenceRate / BLOCK_SIZE;
constexpr int releaseBlocks = 0.5 * referenceRate / BLOCK_SIZE;

std::vector<float> renderReference(const fs::path &patch)
{
    auto surge = Surge::Headless::createSurge(referenceRate);
    if (!surge->loadPatchByPath(patch.u8string().c_str(), -1, "Reference"))
        return {};
    surge->seedRandom(20261030);
    std::vector<float> out;
    out.reserve((holdBlocks + releaseBlocks) * BLOCK_SIZE * 2);
    for (int b = 0; b < holdBlocks + releaseBlocks; ++b)
    {
        if (b == 0)
        {
            surge->playNote(0, 60, 100, 0);
            surge->playNote(0, 67, 80, 0);
        }
        if (b == holdBlocks)
        {
            surge->releaseNote(0, 60, 0);
            surge->releaseNote(0, 67, 0);
        }
        surge->process();
        for (int i = 0; i < BLOCK_SIZE; ++i)
        {
            out.push_back(surge->output[0][i]);
            out.push_back(surge->output[1][i]);
        }
    }
    return out;
}

std::string referenceName(const fs::path &root, const fs::path &patch)
{
    auto n = fs::relative(patch, root).generic_u8string();
    std::replace(n.begin(), n.end(), '/', '_');
    return n + ".f32";
}
} // namespace

TEST_CASE("Factory Patches Match Reference Renders", "[reference]")
{
    auto dirEnv = std::getenv("SURGE_REFERENCE_DIR");
    if (!dirEnv)
        SKIP("SURGE_REFERENCE_DIR not set");
    const bool record = std::getenv("SURGE_REFERENCE_RECORD") != nullptr;
    const fs::path dir{dirEnv}, root{"resources/data/patches_factory"};
    REQUIRE(fs::is_directory(root));
    fs::create_directories(dir);

    std::vector<fs::path> patches;
    for (auto &e : fs::recursive_directory_iterator(root))
        if (e.path().extension() == ".fxp")
            patches.push_back(e.path());
    std::sort(patches.begin(), patches.end());
    REQUIRE(!patches.empty());

    std::set<std::string> skipped;
    {
        std::ifstream in(dir / "nondeterministic.txt");
        for (std::string line; std::getline(in, line);)
            skipped.insert(line);
    }

    std::ostringstream failures;
    int compared = 0, failed = 0;

    for (auto &p : patches)
    {
        auto name = referenceName(root, p);
        auto got = renderReference(p);
        REQUIRE(!got.empty());
        std::vector<float> want(got.size());
        std::ifstream f(dir / name, std::ios::binary);
        f.read(reinterpret_cast<char *>(want.data()), want.size() * sizeof(float));
        const bool haveReference = f && f.peek() == EOF;
        if (record)
        {
            if (skipped.count(name))
                continue;
            if (renderReference(p) != got || (haveReference && want != got))
            {
                skipped.insert(name);
                continue;
            }
            std::ofstream o(dir / name, std::ios::binary);
            o.write(reinterpret_cast<const char *>(got.data()), got.size() * sizeof(float));
            continue;
        }
        if (skipped.count(name))
            continue;
        if (!haveReference)
        {
            failures << name << ": reference missing or a different length\n";
            failed++;
            continue;
        }
        float maxDiff = 0;
        for (size_t i = 0; i < got.size(); ++i)
            maxDiff = std::max(maxDiff, std::fabs(got[i] - want[i]));
        compared++;
        if (!(maxDiff <= referenceTolerance))
        {
            failures << name << ": max difference " << maxDiff << "\n";
            failed++;
        }
    }

    if (record)
    {
        std::ofstream nondet(dir / "nondeterministic.txt");
        for (auto &n : skipped)
            nondet << n << "\n";
        SUCCEED("Recorded reference renders in " << dir.u8string());
    }
    else
    {
        INFO(failures.str());
        INFO("Compared " << compared << " patches, skipped " << skipped.size());
        REQUIRE(failed == 0);
    }
}
