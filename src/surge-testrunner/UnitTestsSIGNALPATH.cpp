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
 * Voice signal path additions: mixer channel polarity, the post-filter route, the saturators
 * after the filters and before the amp, and the filter output gain.
 */
#include <cmath>
#include <complex>
#include <vector>

#include "HeadlessUtils.h"
#include "dsp/FilterCutoffRateMatch.h"
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

// A sine at 440 Hz on oscillator 1, sustain at full level.
std::shared_ptr<SurgeSynthesizer> sineSurge()
{
    auto surge = sawSurge();
    auto &sc = surge->storage.getPatch().scene[0];
    sc.osc[0].type.val.i = ot_sine;
    surge->storage.getPatch().update_controls(false, &sc.osc[0]);
    sc.adsr[0].s.val.f = 1.f;
    return surge;
}

// Level in dB of harmonics 1..n of f0, relative to the fundamental (Hann window).
std::vector<double> harmonics(const std::vector<float> &v, size_t from, double f0, int n = 6)
{
    std::vector<double> out;
    auto N = v.size() - from;
    double h1 = 0;
    for (int h = 1; h <= n; h++)
    {
        std::complex<double> acc = 0;
        for (size_t i = 0; i < N; i++)
        {
            double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
            acc += w * v[from + i] * std::polar(1.0, -2 * M_PI * h * f0 * i / 48000.0);
        }
        if (h == 1)
            h1 = std::abs(acc);
        out.push_back(20 * std::log10(std::abs(acc) / h1 + 1e-12));
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

TEST_CASE("Amp Saturator Shape And Position", "[signalpath]")
{
    // tanh, or with saturatorAlgebraicCurve u / sqrt(1 + u^2)
    bool alg = GENERATE(false, true);
    INFO("algebraic curve " << alg);
    auto curve = [alg](double u) { return alg ? u / std::sqrt(1 + u * u) : std::tanh(u); };
    auto make = [alg](float drive, float asym, float vca) {
        auto s = sineSurge();
        s->storage.saturatorAlgebraicCurve = alg;
        auto &sc = s->storage.getPatch().scene[0];
        sc.vca_level.val.f = vca;
        sc.amp_drive.deactivated = drive < -100;
        sc.amp_drive.val.f = drive;
        sc.amp_drive_asymmetry.val.f = asym;
        return s;
    };
    const size_t from = 24000;
    auto clean = render(*make(-1000, 0, 0), 69, 1000);
    REQUIRE(rms(clean, from) > 0.1);

    // Off by default
    {
        auto s = sineSurge();
        REQUIRE(s->storage.getPatch().scene[0].amp_drive.deactivated);
    }

    for (auto [drive, asym] : {std::pair{12.f, 0.f}, {12.f, 0.4f}, {24.f, -0.3f}})
    {
        INFO("drive " << drive << " asymmetry " << asym);
        auto sat = render(*make(drive, asym, 0), 69, 1000);
        // the same curve applied to the clean render divided by k, the gain after the
        // saturator (amp level, scene volume, pan), which this test doesn't need to know
        auto G = std::pow(10.f, drive / 20.f);
        auto hs = harmonics(sat, from, 440);
        std::vector<double> he;
        double bestErr = 1e9, bestK = 1, bestRms = 0;
        for (double k = 0.25; k < 2; k *= 1.01)
        {
            std::vector<float> expect(clean.size());
            for (size_t i = 0; i < clean.size(); i++)
            {
                auto t = curve(G * clean[i] / k);
                expect[i] = (t + asym * t * t) / curve(G);
            }
            auto h = harmonics(expect, from, 440);
            double err = 0;
            for (int j = 1; j < 6; j++)
                if (h[j] > -80)
                    err += (h[j] - hs[j]) * (h[j] - hs[j]);
            if (err < bestErr)
            {
                bestErr = err;
                he = h;
                bestK = k;
                bestRms = rms(expect, from);
            }
        }
        // scaled back by the curve at the drive: the level follows the curve too (without the square
        // term, whose DC the engine's blocker removes)
        if (asym == 0)
            REQUIRE(rms(sat, from) == Catch::Approx(bestK * bestRms).epsilon(0.02));
        for (int h = 1; h < 6; h++)
        {
            INFO("harmonic " << h + 1);
            if (he[h] > -80)
                REQUIRE(hs[h] == Catch::Approx(he[h]).margin(0.2));
            else
                REQUIRE(hs[h] < -70);
        }
        if (asym == 0)
            REQUIRE(hs[1] < -70); // symmetric: no even harmonics

        // before the amp gain: the amp level doesn't change the spectrum
        auto quiet = render(*make(drive, asym, -12), 69, 1000);
        auto hq = harmonics(quiet, from, 440);
        for (int h = 1; h < 6; h++)
            if (hs[h] > -80)
                REQUIRE(hq[h] == Catch::Approx(hs[h]).margin(0.05));
        REQUIRE(rms(quiet, from) == Catch::Approx(rms(sat, from) / 4).epsilon(0.01));
    }
}

TEST_CASE("Filter Saturator And Output Gain", "[signalpath]")
{
    auto make = [](bool filterOn, float drive, float gain) {
        auto s = sineSurge();
        auto &sc = s->storage.getPatch().scene[0];
        sc.filterunit[0].type.val.i = sst::filters::fut_lp24;
        sc.filterunit[0].cutoff.val.f = 60; // open
        sc.filterunit[0].type.deactivated = !filterOn;
        sc.filterunit[1].type.deactivated = true;
        sc.filterunit[0].drive.deactivated = drive < -100;
        sc.filterunit[0].drive.val.f = drive;
        sc.filterunit[0].output_gain.val.f = gain;
        return s;
    };
    const size_t from = 24000;
    auto thru = render(*make(false, -1000, 0), 69, 1000);

    // the saturator runs only while its filter does
    auto thruDriven = render(*make(false, 24, 0), 69, 1000);
    double worst = 0;
    for (size_t i = 0; i < thru.size(); i++)
        worst = std::max(worst, (double)std::fabs(thru[i] - thruDriven[i]));
    REQUIRE(worst < 1e-6);
    auto driven = harmonics(render(*make(true, 24, 0), 69, 1000), from, 440);
    REQUIRE(driven[2] > -40); // third harmonic from the saturator

    // output gain, also with the filter off
    auto thruLoud = render(*make(false, -1000, 6.0206f), 69, 1000);
    REQUIRE(rms(thruLoud, from) == Catch::Approx(2 * rms(thru, from)).epsilon(1e-4));
    auto open = render(*make(true, -1000, 0), 69, 1000);
    auto openQuiet = render(*make(true, -1000, -6.0206f), 69, 1000);
    REQUIRE(rms(openQuiet, from) == Catch::Approx(rms(open, from) / 2).epsilon(1e-4));
}

TEST_CASE("Saturator Parameters Reset When A Patch Lacks Them", "[signalpath]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    for (auto &sc : patch.scene)
    {
        sc.amp_drive.deactivated = false;
        sc.amp_drive.val.f = 10;
        sc.filterunit[1].drive.deactivated = false;
        sc.filterunit[1].output_gain.val.f = 3;
    }
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    for (auto &sc : patch.scene)
    {
        REQUIRE(sc.amp_drive.deactivated);
        REQUIRE(sc.amp_drive.val.f == 0);
        REQUIRE(sc.filterunit[1].drive.deactivated);
        REQUIRE(sc.filterunit[1].output_gain.val.f == 0);
    }
    patch.scene[0].filterunit[0].drive.deactivated = false;
    patch.scene[0].filterunit[0].drive.val.f = 7;
    patch.scene[0].amp_drive_asymmetry.val.f = 0.25f;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].filterunit[0].drive.deactivated = true;
    patch.scene[0].filterunit[0].drive.val.f = 0;
    patch.scene[0].amp_drive_asymmetry.val.f = 0;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(!patch.scene[0].filterunit[0].drive.deactivated);
    REQUIRE(patch.scene[0].filterunit[0].drive.val.f == 7);
    REQUIRE(patch.scene[0].amp_drive_asymmetry.val.f == 0.25f);
}

TEST_CASE("Pre-Filter Shaper Sits Before The Filters", "[signalpath]")
{
    auto make = [](bool shaper, float cutoff, bool filterOn, bool post) {
        auto s = sineSurge();
        auto &sc = s->storage.getPatch().scene[0];
        sc.filterunit[0].type.val.i = sst::filters::fut_lp24;
        sc.filterunit[0].cutoff.val.f = cutoff;
        sc.filterunit[0].type.deactivated = !filterOn;
        sc.filterunit[1].type.deactivated = true;
        sc.prefilter_ws.type.val.i = (int)sst::waveshapers::WaveshaperType::wst_softfold;
        sc.prefilter_ws.type.deactivated = !shaper;
        sc.prefilter_ws.drive.val.f = 12;
        sc.mix_postfilter[0].val.b = post;
        return s;
    };
    {
        auto s = sineSurge();
        REQUIRE(s->storage.getPatch().scene[0].prefilter_ws.type.deactivated);
    }
    const size_t from = 24000;
    auto folded = harmonics(render(*make(true, 0, false, false), 69, 1000), from, 440);
    REQUIRE(folded[2] > -30); // odd harmonics from the folder
    REQUIRE(folded[1] < -60); // and no even ones
    // a 24 dB low-pass at the fundamental cuts them: the shaper comes first
    auto filtered = harmonics(render(*make(true, 0, true, false), 69, 1000), from, 440);
    REQUIRE(filtered[2] < folded[2] - 25);

    // channels routed past the filters skip it too
    auto clean = render(*make(false, 0, false, true), 69, 1000);
    auto post = render(*make(true, 0, false, true), 69, 1000);
    double worst = 0;
    for (size_t i = 0; i < clean.size(); i++)
        worst = std::max(worst, (double)std::fabs(clean[i] - post[i]));
    REQUIRE(worst < 1e-6);

    // reset when a patch lacks it
    auto s = make(true, 0, false, false);
    REQUIRE(s->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(s->storage.getPatch().scene[0].prefilter_ws.type.deactivated);
    REQUIRE(s->storage.getPatch().scene[0].prefilter_ws.drive.val.f == 0);
}

TEST_CASE("VCA Velocity Amplitude Law", "[signalpath]")
{
    // held-sine level in dB at a velocity, relative to velocity 127
    auto level = [](bool law, float sens, int vel) {
        auto s = sineSurge();
        s->storage.vcaVelocityAmplitudeLaw = law;
        s->storage.getPatch().scene[0].vca_velsense.val.f = sens;
        std::vector<float> out;
        for (int b = 0; b < 1000; b++)
        {
            if (b == 2)
                s->playNote(0, 69, vel, 0);
            s->process();
            out.insert(out.end(), s->output[0], s->output[0] + BLOCK_SIZE);
        }
        return 20 * std::log10(rms(out, 24000) + 1e-12);
    };
    for (float sens : {-6.f, -12.f, -24.f})
    {
        double top = level(true, sens, 127);
        for (int vel : {1, 32, 64, 100})
        {
            INFO("sensitivity " << sens << " velocity " << vel);
            double a = 1 + sens / 12 * (1 - vel / 127.0);
            double got = level(true, sens, vel) - top;
            if (a > 0)
                REQUIRE(got == Catch::Approx(40 * std::log10(a)).margin(0.05));
            else
                REQUIRE(got < -120);
            // off: linear in dB, as before
            REQUIRE(level(false, sens, vel) - level(false, sens, 127) ==
                    Catch::Approx(sens * (1 - vel / 127.0)).margin(0.05));
        }
    }
}

TEST_CASE("Noise Tilt Is A First-Order Shelf", "[signalpath]")
{
    // noise alone, the same seed with and without the tilt: the ratio of their spectra is the
    // shelf's response (1 + g s/w) / (1 + s/w)
    auto noise = [](float tilt, float freq) {
        auto s = sawSurge();
        auto &sc = s->storage.getPatch().scene[0];
        sc.mute_o1.val.b = true;
        sc.mute_noise.val.b = false;
        sc.noise_tilt.val.f = tilt;
        sc.noise_tilt_freq.val.f = freq;
        return render(*s, 60, 1700);
    };
    auto bandPower = [](const std::vector<float> &v, size_t from, double f) {
        size_t N = 48000;
        double p = 0;
        for (double fb = std::round(f / 1.1); fb <= f * 1.1; fb += 1)
        {
            std::complex<double> acc = 0;
            for (size_t i = 0; i < N; i++)
            {
                double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
                acc += w * v[from + i] * std::polar(1.0, -2 * M_PI * fb * i / 48000.0);
            }
            p += std::norm(acc);
        }
        return p;
    };
    auto flat = noise(0, 0);
    REQUIRE(rms(flat, 2000) > 0.01);
    for (auto [tilt, note] : {std::pair{-30.f, -12.f}, {12.f, 36.f}})
    {
        auto tilted = noise(tilt, note);
        double fc = 440 * std::pow(2.0, note / 12.0), g = std::pow(10.0, tilt / 20.0);
        for (double f : {80.0, 1000.0, 8000.0})
        {
            INFO("tilt " << tilt << " dB at " << fc << " Hz, " << f << " Hz");
            double x = f / fc;
            double expect = 10 * std::log10((1 + g * g * x * x) / (1 + x * x));
            double got = 10 * std::log10(bandPower(tilted, 4000, f) / bandPower(flat, 4000, f));
            REQUIRE(got == Catch::Approx(expect).margin(0.3));
        }
    }
    // 0 dB is off: the output is untouched
    REQUIRE(noise(0, 36) == flat);

    // reset to off when a patch lacks it, and round trips
    auto surge = Surge::Headless::createSurge(48000);
    auto &patch = surge->storage.getPatch();
    patch.scene[1].noise_tilt.val.f = 6;
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(patch.scene[1].noise_tilt.val.f == 0);
    patch.scene[0].noise_tilt.val.f = -20;
    patch.scene[0].noise_tilt_freq.val.f = 30;
    void *data = nullptr;
    auto size = patch.save_patch(&data);
    std::vector<char> copy((char *)data, (char *)data + size);
    patch.scene[0].noise_tilt.val.f = 0;
    patch.scene[0].noise_tilt_freq.val.f = 0;
    patch.load_patch(copy.data(), (int)copy.size(), false);
    REQUIRE(patch.scene[0].noise_tilt.val.f == -20);
    REQUIRE(patch.scene[0].noise_tilt_freq.val.f == 30);
}

TEST_CASE("Engine Options Are Saved In The Patch", "[signalpath]")
{
    auto surge = Surge::Headless::createSurge(48000);
    auto &st = surge->storage;
    auto &patch = st.getPatch();
    auto save = [&] {
        void *data = nullptr;
        auto size = patch.save_patch(&data);
        return std::vector<char>((char *)data, (char *)data + size);
    };
    auto options = [&] {
        return std::vector<bool>{st.analogEnvelopeAttackClamp, st.vcaVelocityAmplitudeLaw,
                                 st.saturatorAlgebraicCurve, st.lfoLongRelease,
                                 st.filterCutoffRateMatch};
    };
    auto set = [&](bool a, bool b, bool c, bool d, bool e) {
        st.analogEnvelopeAttackClamp = a;
        st.vcaVelocityAmplitudeLaw = b;
        st.saturatorAlgebraicCurve = c;
        st.lfoLongRelease = d;
        st.filterCutoffRateMatch = e;
    };

    // off by default, and a patch saved with them off doesn't mention them
    REQUIRE(options() == std::vector<bool>{false, false, false, false, false});
    auto off = save();
    REQUIRE(std::string(off.begin(), off.end()).find("saturatorAlgebraicCurve") ==
            std::string::npos);

    // each round trips on its own
    for (int i = 0; i < 5; i++)
    {
        INFO("option " << i);
        set(i == 0, i == 1, i == 2, i == 3, i == 4);
        auto on = save();
        set(false, false, false, false, false);
        patch.load_patch(on.data(), (int)on.size(), false);
        REQUIRE(options() == std::vector<bool>{i == 0, i == 1, i == 2, i == 3, i == 4});
    }

    // a patch without them turns them off
    set(true, true, true, true, true);
    patch.load_patch(off.data(), (int)off.size(), false);
    REQUIRE(options() == std::vector<bool>{false, false, false, false, false});
    set(true, true, true, true, true);
    REQUIRE(surge->loadPatchByPath("resources/test-data/patches/TestInitSaw.fxp", -1, "Test"));
    REQUIRE(options() == std::vector<bool>{false, false, false, false, false});
}

TEST_CASE("Filter Cutoff Rate Match", "[signalpath]")
{
    using Surge::Filter::rateMatchedCutoff;
    // the legacy ladder clamps at 0.187 x the oversampled rate
    const int ladder = sst::filters::fut_lpmoog, ladder6 = 0;

    // unchanged at 44.1 kHz and for filter types without a table
    for (float c : {-30.f, 0.f, 40.f, 70.f})
    {
        REQUIRE(rateMatchedCutoff(c, ladder, ladder6, 44100) == c);
        REQUIRE(rateMatchedCutoff(c, sst::filters::fut_comb_pos, 0, 48000) == c);
    }
    // at 48 and 96 kHz: non-decreasing, and one cutoff above the 44.1 kHz clamp (62.74)
    for (float rate : {48000.f, 96000.f})
    {
        INFO("rate " << rate);
        for (const auto &t : Surge::Filter::rate_match::tables)
        {
            INFO("type " << t.type << " subtype " << t.subtype);
            float last = -1000;
            for (float c = -50; c <= 75; c += 0.125f)
            {
                float m = rateMatchedCutoff(c, t.type, t.subtype, rate);
                REQUIRE(m >= last);
                last = m;
            }
        }
        auto top = rateMatchedCutoff(64, ladder, ladder6, rate);
        REQUIRE(top != 64);
        REQUIRE(rateMatchedCutoff(70, ladder, ladder6, rate) == top);
        REQUIRE(rateMatchedCutoff(75, ladder, ladder6, rate) == top);
    }

    // in the voice: off, the ladder at 48 kHz opens past cutoff 64; on, cutoffs 64 and 70
    // sound the same, as they do at 44.1 kHz; at 44.1 kHz on and off are identical
    auto play = [](int rate, bool on, float cutoff) {
        auto s = Surge::Headless::createSurge(rate);
        s->storage.filterCutoffRateMatch = on;
        auto &sc = s->storage.getPatch().scene[0];
        sc.osc[0].retrigger.val.b = true;
        sc.drift.val.f = 0.f;
        sc.lowcut.deactivated = true;
        sc.filterunit[0].type.val.i = sst::filters::fut_lpmoog;
        sc.filterunit[0].subtype.val.i = 0;
        sc.filterunit[0].cutoff.val.f = cutoff;
        sc.filterunit[0].resonance.val.f = 0;
        s->seedRandom(1234);
        return render(*s, 36, 100);
    };
    REQUIRE(play(48000, false, 64) != play(48000, false, 70));
    REQUIRE(play(48000, true, 64) == play(48000, true, 70));
    REQUIRE(play(44100, false, 64) == play(44100, false, 70));
    REQUIRE(play(44100, true, 50) == play(44100, false, 50));
    REQUIRE(play(48000, true, 50) != play(48000, false, 50));
}
