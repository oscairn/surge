/*
 * Oscairn Analog: Surge XT's engine as a CLAP instrument for the Oscairn host.
 * GPL-3.0-or-later, see OscairnAnalog.cpp.
 */
#ifndef SURGE_SRC_OSCAIRN_CLAP_OSCAIRNANALOG_H
#define SURGE_SRC_OSCAIRN_CLAP_OSCAIRNANALOG_H

#include <clap/clap.h>

// The plugin's entry. The module exports it as clap_entry; tests call it directly.
extern const clap_plugin_entry_t oscairn_analog_entry;

#endif
