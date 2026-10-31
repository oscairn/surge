/*
 * Oscairn Analog: Surge XT's engine as a CLAP instrument for the Oscairn host.
 * GPL-3.0-or-later, see OscairnAnalog.cpp.
 */
#include "OscairnAnalog.h"

static bool entryInit(const char *path) { return oscairn_analog_entry.init(path); }
static void entryDeinit() { oscairn_analog_entry.deinit(); }
static const void *entryFactory(const char *id) { return oscairn_analog_entry.get_factory(id); }

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                               entryDeinit, entryFactory};
