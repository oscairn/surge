/*
 * Renders one note through the Oscairn Analog plugin from a patch given as Surge parameter
 * values and modulation routings (oscairn-analog-render and the fitted-table tests).
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors.
 */
#ifndef OSCAIRN_ANALOG_RENDER_NOTE_H
#define OSCAIRN_ANALOG_RENDER_NOTE_H

#include <string>
#include <vector>

namespace oscairn_analog
{
struct RenderSpec
{
    int note = 60, velocity = 100;
    double hold = 1, tail = 1, rate = 48000;
    // Surge storage name (a_osc1_pitch ...) and value in Surge's own units
    std::vector<std::pair<std::string, double>> params;
    struct Mod
    {
        int source; // enum modsources
        std::string target;
        double depth; // Surge's 0..1 units
    };
    std::vector<Mod> mods;
};

// Interleaved stereo samples. Throws std::runtime_error on an unknown name or a refused patch.
std::vector<float> renderNote(const RenderSpec &spec);

// Lines "name<TAB>full name<TAB>display value" for every scene parameter.
std::vector<std::string> listParameters();
} // namespace oscairn_analog

#endif
