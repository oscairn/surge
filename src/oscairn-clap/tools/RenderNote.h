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
    // Surge storage name (a_osc1_pitch ...) and value in Surge's own units; name.off with a
    // value of 1 deactivates the parameter (a_lowcut.off)
    std::vector<std::pair<std::string, double>> params;
    struct Mod
    {
        int source; // enum modsources
        std::string target;
        double depth; // Surge's 0..1 units
    };
    std::vector<Mod> mods;
    // A second note at legatoAt s (for portamento), the first released 0.1 s later; the
    // second is released at hold. None when legatoNote < 0.
    int legatoNote = -1;
    double legatoAt = 0;
    // Pitch bend (-1..1, sent as MIDI from the start of the render; 0: none)
    double bend = 0;
    // Turn on, in the patch, the engine options that match the reference (the analog attack
    // clamp, the amplitude velocity law, the algebraic saturators, the long LFO release and the
    // filter cutoff rate match), as a converted Part does
    bool referenceMatching = false;
};

// Interleaved stereo samples. Throws std::runtime_error on an unknown name or a refused patch.
std::vector<float> renderNote(const RenderSpec &spec);

// Lines "name<TAB>full name<TAB>display value" for every scene parameter.
std::vector<std::string> listParameters();
} // namespace oscairn_analog

#endif
