/*
 * oscairn-analog-render: renders one note through the Oscairn Analog plugin, for measuring
 * the engine against reference renders.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors. This file is MIT-licensed; the executable links
 * Surge XT and is GPL-3.0-or-later as a whole.
 *
 *   oscairn-analog-render out.wav [--note 60] [--vel 100] [--hold 1] [--tail 1] [--rate 48000]
 *                         [name=value ...] [mod:source:name=depth ...]
 *   oscairn-analog-render --list
 *
 * name is a Surge storage name (as --list prints: a_osc1_pitch, a_env1_release ...); value
 * is in Surge's own units (the plugin's parameter units); name.off=1 deactivates a parameter
 * that can be switched off (a_lowcut.off=1) and name.deform=n sets a parameter's deform type
 * (a_osc1_param2.deform=2: the Modern oscillator's third shape is a sine). mod:source:name=depth
 * adds a modulation routing from Surge modulation source number `source` (enum modsources) with
 * depth in Surge's 0..1 units. The patch is built with Surge's API, saved, and loaded into
 * the plugin as its state, so the render goes through the plugin's own path. Writes a
 * stereo 32-bit float WAV.
 */

#include "RenderNote.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void writeWav(const std::string &path, const std::vector<float> &lr, int rate)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        throw std::runtime_error("cannot write " + path);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(rate);
    u32(rate * 8);
    u16(8);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    std::fwrite(lr.data(), 4, lr.size(), f);
    std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
    using namespace oscairn_analog;
    try
    {
        if (argc == 2 && !std::strcmp(argv[1], "--list"))
        {
            for (auto &l : listParameters())
                std::printf("%s\n", l.c_str());
            return 0;
        }
        if (argc < 2)
        {
            std::fprintf(stderr, "usage: oscairn-analog-render out.wav [options] [name=value] "
                                 "[mod:source:name=depth] | --list\n");
            return 2;
        }

        std::string out = argv[1];
        RenderSpec spec;
        for (int i = 2; i < argc; ++i)
        {
            std::string a = argv[i];
            auto next = [&]() -> double {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value for " + a);
                return std::atof(argv[++i]);
            };
            if (a == "--note")
                spec.note = (int)next();
            else if (a == "--vel")
                spec.velocity = (int)next();
            else if (a == "--hold")
                spec.hold = next();
            else if (a == "--tail")
                spec.tail = next();
            else if (a == "--rate")
                spec.rate = next();
            else if (auto eq = a.find('='); eq != std::string::npos)
            {
                auto lhs = a.substr(0, eq);
                double v = std::atof(a.c_str() + eq + 1);
                if (lhs.rfind("mod:", 0) == 0)
                {
                    auto c = lhs.find(':', 4);
                    spec.mods.push_back(
                        {std::atoi(lhs.substr(4, c - 4).c_str()), lhs.substr(c + 1), v});
                }
                else
                    spec.params.emplace_back(lhs, v);
            }
            else
                throw std::runtime_error("unknown argument " + a);
        }
        writeWav(out, renderNote(spec), (int)spec.rate);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
