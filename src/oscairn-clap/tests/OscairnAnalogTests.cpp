/*
 * Tests for the Oscairn Analog CLAP instrument (doc/oscairn-plan.md, milestone 0).
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors. This file (a small CLAP test host and an allocation
 * guard) is MIT-licensed; the test executable links Surge XT and is GPL-3.0-or-later as a
 * whole.
 *
 * The test is a small CLAP host: it creates the plugin through its entry, renders with
 * events at exact sample times and checks
 *   - parameter metadata (unique IDs, finite min <= default <= max, ranges that hold for
 *     every factory patch),
 *   - byte-identical state round trips for the default patch and every factory patch,
 *   - identical renders with 37- and 512-frame blocks, and on repeat,
 *   - no heap allocation inside process(),
 *   - a velocity-0 note-on starts a note, NOTE_END for every note ID (also when a state
 *     load ends held notes),
 *   - refusal of patches with Formula modulators,
 *   - state loads and saves on one thread while another renders (run under TSan),
 *   - the fitted tables (FittedTables.h, milestone 1): rendering a table's engine value
 *     gives the feature measured on the reference at that index, within the tolerance.
 *
 * Usage: oscairn-analog-tests [factory patch directory]
 */

#include "OscairnAnalog.h"
#include "SurgeSynthesizer.h"
#include "../tools/RenderNote.h"
#include "Features.h"
#include "FittedTables.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <execinfo.h>
#include <unistd.h>

// --- allocation guard -----------------------------------------------------------------
// Replaces global operator new for this executable; the plugin is linked in statically.
static thread_local bool forbidAllocation = false;
static void *guardedAlloc(std::size_t n)
{
    if (forbidAllocation)
    {
        forbidAllocation = false; // backtrace() may allocate
        std::fputs("FAIL: heap allocation inside process()\n", stderr);
        void *frames[32];
        backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
        std::abort();
    }
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
static void *guardedAlignedAlloc(std::size_t n, std::align_val_t a)
{
    if (forbidAllocation)
    {
        std::fputs("FAIL: heap allocation inside process()\n", stderr);
        std::abort();
    }
    void *p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void *), (std::size_t)a), n ? n : 1) == 0)
        return p;
    throw std::bad_alloc();
}
void *operator new(std::size_t n) { return guardedAlloc(n); }
void *operator new[](std::size_t n) { return guardedAlloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    try
    {
        return guardedAlloc(n);
    }
    catch (...)
    {
        return nullptr;
    }
}
void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept
{
    return operator new(n, t);
}
void *operator new(std::size_t n, std::align_val_t a) { return guardedAlignedAlloc(n, a); }
void *operator new[](std::size_t n, std::align_val_t a) { return guardedAlignedAlloc(n, a); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }

// --- checks ---------------------------------------------------------------------------
static int failures = 0;
static void check(bool ok, const std::string &what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// --- a minimal host -------------------------------------------------------------------
static const clap_host_t host = {
    CLAP_VERSION_INIT,
    nullptr,
    "oscairn-analog-tests",
    "Oscairn",
    "",
    "0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

struct Event
{
    uint32_t time; // absolute sample
    union
    {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_param_value_t value;
        clap_event_note_expression_t expression;
        clap_event_midi_t midi;
    };
};

static Event noteEvent(uint32_t t, uint16_t type, int32_t id, int16_t key, double vel)
{
    Event e{};
    e.time = t;
    e.note = {};
    e.note.header = {sizeof(clap_event_note_t), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    e.note.note_id = id;
    e.note.port_index = 0;
    e.note.channel = 0;
    e.note.key = key;
    e.note.velocity = vel;
    return e;
}

static Event paramEvent(uint32_t t, clap_id id, double value)
{
    Event e{};
    e.time = t;
    e.value = {};
    e.value.header = {sizeof(clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                      CLAP_EVENT_PARAM_VALUE, 0};
    e.value.param_id = id;
    e.value.note_id = -1;
    e.value.port_index = -1;
    e.value.channel = -1;
    e.value.key = -1;
    e.value.value = value;
    return e;
}

static Event expressionEvent(uint32_t t, clap_note_expression x, int32_t id, double value)
{
    Event e{};
    e.time = t;
    e.expression = {};
    e.expression.header = {sizeof(clap_event_note_expression_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                           CLAP_EVENT_NOTE_EXPRESSION, 0};
    e.expression.expression_id = x;
    e.expression.note_id = id;
    e.expression.port_index = 0;
    e.expression.channel = 0;
    e.expression.key = -1;
    e.expression.value = value;
    return e;
}

static Event midiEvent(uint32_t t, uint8_t a, uint8_t b, uint8_t c)
{
    Event e{};
    e.time = t;
    e.midi = {};
    e.midi.header = {sizeof(clap_event_midi_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    e.midi.port_index = 0;
    e.midi.data[0] = a;
    e.midi.data[1] = b;
    e.midi.data[2] = c;
    return e;
}

struct InEvents
{
    std::vector<const clap_event_header_t *> list;
    clap_input_events_t in{this, [](const clap_input_events_t *l) {
                               return (uint32_t) static_cast<InEvents *>(l->ctx)->list.size();
                           },
                           [](const clap_input_events_t *l, uint32_t i) {
                               return static_cast<InEvents *>(l->ctx)->list[i];
                           }};
};

struct OutEvents
{
    std::vector<clap_event_note_t> noteEnds; // reserved up front: try_push must not allocate
    clap_output_events_t out{this, [](const clap_output_events_t *l,
                                      const clap_event_header_t *e) {
                                 auto self = static_cast<OutEvents *>(l->ctx);
                                 if (e->type != CLAP_EVENT_NOTE_END ||
                                     self->noteEnds.size() == self->noteEnds.capacity())
                                     return false;
                                 self->noteEnds.push_back(
                                     *reinterpret_cast<const clap_event_note_t *>(e));
                                 return true;
                             }};
};

struct Instance
{
    const clap_plugin_t *plugin{nullptr};
    const clap_plugin_params_t *params{nullptr};
    const clap_plugin_state_t *state{nullptr};
    const clap_plugin_audio_ports_t *ports{nullptr};
    const clap_plugin_note_ports_t *notePorts{nullptr};
    uint64_t position{0}; // samples rendered since activation
    OutEvents out;

    explicit Instance(double rate = 48000)
    {
        auto factory = static_cast<const clap_plugin_factory_t *>(
            oscairn_analog_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory, &host, "org.oscairn.analog");
        if (!plugin || !plugin->init(plugin))
            throw std::runtime_error("plugin creation failed");
        params = static_cast<const clap_plugin_params_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t *>(
            plugin->get_extension(plugin, CLAP_EXT_STATE));
        ports = static_cast<const clap_plugin_audio_ports_t *>(
            plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        notePorts = static_cast<const clap_plugin_note_ports_t *>(
            plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
        out.noteEnds.reserve(4096);
        activate(rate);
    }
    ~Instance()
    {
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    void activate(double rate)
    {
        if (!plugin->activate(plugin, rate, 1, 4096) || !plugin->start_processing(plugin))
            throw std::runtime_error("activation failed");
        position = 0;
    }

    // Renders frames samples in blocks of block, delivering events at their absolute times.
    std::vector<float> render(uint32_t frames, uint32_t block, std::vector<Event> events = {},
                              bool guard = true)
    {
        std::stable_sort(events.begin(), events.end(),
                         [](const Event &a, const Event &b) { return a.time < b.time; });
        std::vector<float> left(block), right(block), result;
        result.reserve(frames * 2);
        InEvents in;
        in.list.reserve(events.size());
        size_t next = 0;
        for (uint32_t done = 0; done < frames;)
        {
            uint32_t n = std::min(block, frames - done);
            in.list.clear();
            for (; next < events.size() && events[next].time < position + n; ++next)
            {
                auto &e = events[next];
                e.header.time = (uint32_t)(std::max<uint64_t>(e.time, position) - position);
                in.list.push_back(&e.header);
            }
            float *channels[2] = {left.data(), right.data()};
            clap_audio_buffer_t buffer{};
            buffer.data32 = channels;
            buffer.channel_count = 2;
            clap_process_t pr{};
            pr.steady_time = (int64_t)position;
            pr.frames_count = n;
            pr.audio_outputs = &buffer;
            pr.audio_outputs_count = 1;
            pr.in_events = &in.in;
            pr.out_events = &out.out;
            forbidAllocation = guard;
            auto status = plugin->process(plugin, &pr);
            forbidAllocation = false;
            if (status == CLAP_PROCESS_ERROR)
                throw std::runtime_error("process failed");
            for (uint32_t i = 0; i < n; ++i)
            {
                result.push_back(left[i]);
                result.push_back(right[i]);
            }
            position += n;
            done += n;
        }
        return result;
    }

    std::vector<uint8_t> save()
    {
        std::vector<uint8_t> bytes;
        clap_ostream_t os{&bytes, [](const clap_ostream_t *s, const void *b, uint64_t n) {
                              auto v = static_cast<std::vector<uint8_t> *>(s->ctx);
                              v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
                              return (int64_t)n;
                          }};
        if (!state->save(plugin, &os))
            throw std::runtime_error("state save failed");
        return bytes;
    }

    bool load(const std::vector<uint8_t> &bytes)
    {
        struct Reader
        {
            const std::vector<uint8_t> *bytes;
            size_t at;
        } r{&bytes, 0};
        clap_istream_t is{&r, [](const clap_istream_t *s, void *b, uint64_t n) {
                              auto rd = static_cast<Reader *>(s->ctx);
                              // Small reads, to exercise the plugin's read loop.
                              n = std::min<uint64_t>({n, 1000, rd->bytes->size() - rd->at});
                              std::memcpy(b, rd->bytes->data() + rd->at, n);
                              rd->at += n;
                              return (int64_t)n;
                          }};
        return state->load(plugin, &is);
    }

    std::map<std::string, clap_param_info_t> paramsByName()
    {
        std::map<std::string, clap_param_info_t> r;
        for (uint32_t i = 0; i < params->count(plugin); ++i)
        {
            clap_param_info_t info;
            params->get_info(plugin, i, &info);
            r[std::string(info.module) + "/" + info.name] = info;
        }
        return r;
    }
};

// Patch data ("sub3" onwards) from a Surge .fxp file.
static std::vector<uint8_t> patchFromFxp(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    for (size_t i = 0; i + 4 <= bytes.size(); ++i)
        if (!std::memcmp(&bytes[i], "sub3", 4))
            return {bytes.begin() + i, bytes.end()};
    return {};
}

static std::vector<std::string> factoryPatches(const std::string &dir)
{
    std::vector<std::string> r;
    for (auto &e : fs::recursive_directory_iterator(fs::path(dir)))
        if (e.path().extension() == ".fxp")
            r.push_back(e.path().u8string());
    std::sort(r.begin(), r.end());
    return r;
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

struct HeadlessLayer : SurgeSynthesizer::PluginLayer
{
    void surgeParameterUpdated(const SurgeSynthesizer::ID &, float) override {}
    void surgeMacroUpdated(long, float) override {}
};

// --- tests ----------------------------------------------------------------------------

static void testPortsAndParams(const std::vector<std::string> &patches)
{
    Instance a;
    check(a.ports && a.ports->count(a.plugin, true) == 0, "no audio input port");
    clap_audio_port_info_t port{};
    check(a.ports->count(a.plugin, false) == 1 && a.ports->get(a.plugin, 0, false, &port) &&
              port.channel_count == 2,
          "one stereo output port");
    clap_note_port_info_t np{};
    check(a.notePorts->count(a.plugin, true) == 1 && a.notePorts->get(a.plugin, 0, true, &np) &&
              np.preferred_dialect == CLAP_NOTE_DIALECT_CLAP,
          "one CLAP note input port");

    const uint32_t count = a.params->count(a.plugin);
    std::printf("parameters: %u\n", count);
    check(count > 200 && count <= 32768, "parameter count");
    std::set<clap_id> ids;
    std::map<clap_id, clap_param_info_t> infos;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info_t info{};
        check(a.params->get_info(a.plugin, i, &info), "get_info");
        check(ids.insert(info.id).second, std::string("unique ID for ") + info.name);
        check(std::isfinite(info.min_value) && std::isfinite(info.max_value) &&
                  std::isfinite(info.default_value) && info.min_value <= info.default_value &&
                  info.default_value <= info.max_value,
              std::string("finite min <= default <= max for ") + info.module + "/" + info.name);
        check(info.name[0] && info.module[0], "name and module for every parameter");
        double v;
        check(a.params->get_value(a.plugin, info.id, &v) && v >= info.min_value &&
                  v <= info.max_value,
              std::string("value in range for ") + info.name);
        char text[256];
        check(a.params->value_to_text(a.plugin, info.id, info.default_value, text, sizeof(text)),
              std::string("value text for ") + info.name);
        infos[info.id] = info;
    }
    auto moduleOf = [&](const char *name) {
        auto it = infos.find(fnv1a(name));
        return it == infos.end() ? std::string() : std::string(it->second.module);
    };
    check(moduleOf("a_lfo0_rate") == "Voice LFO 1" && moduleOf("a_lfo5_rate") == "Voice LFO 6" &&
              moduleOf("a_lfo12_rate") == "Voice LFO 7" &&
              moduleOf("a_lfo17_rate") == "Voice LFO 12" &&
              moduleOf("a_lfo6_rate") == "Scene LFO 1" && moduleOf("a_lfo11_rate") == "Scene LFO 6",
          "LFO parameters are grouped by their LFO");
    auto maxOf = [&](const char *name) {
        auto it = infos.find(fnv1a(name));
        return it == infos.end() ? -1.0 : it->second.max_value;
    };
    check(maxOf("a_env1_attack") == 8 && maxOf("a_lfo0_attack") == 8 &&
              maxOf("a_lfo17_decay") == 8 && maxOf("a_lfo0_release") == 8 &&
              maxOf("a_lfo6_attack") == 5,
          "envelopes and voice LFO attack, decay and release reach 2^8 s; scene LFOs 2^5");
    check(maxOf("a_portamento") == 3.99f, "portamento reaches 2^3.99 s");
    auto minOf = [&](const char *name) {
        auto it = infos.find(fnv1a(name));
        return it == infos.end() ? 0.0 : it->second.min_value;
    };
    check(minOf("a_vca_velsense") == -1536, "amp velocity sensitivity reaches -1536 dB (k = 128)");
    check(minOf("a_pitch") == -127 && maxOf("a_pitch") == 127 && minOf("a_pbrange_up") == 0 &&
              maxOf("a_pbrange_up") == 127 && minOf("a_pbrange_dn") == -127 &&
              maxOf("a_pbrange_dn") == 127,
          "note shift reaches +-127 semitones, bend up 127, bend down -127 to 127");

    // Ranges hold for every factory patch: a parameter exposed in Surge's own units must have
    // the same range whatever patch is loaded (those whose range follows another parameter
    // are exposed as 0..1), and values stay in range.
    HeadlessLayer layer;
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    auto refSynth = std::make_unique<SurgeSynthesizer>(&layer, config);
    auto &reference = *refSynth;
    // The plugin's envelope segments reach 2^8 s instead of Surge's 2^5 s.
    for (auto &eg : reference.storage.getPatch().scene[0].adsr)
        for (auto *t : {&eg.a, &eg.d, &eg.r})
            t->val_max.f = 8;
    for (int l = 0; l < n_lfos_voice; l++)
        for (auto *t : {&reference.storage.getPatch().scene[0].lfo[l].attack,
                        &reference.storage.getPatch().scene[0].lfo[l].decay,
                        &reference.storage.getPatch().scene[0].lfo[l].release})
            t->val_max.f = 8;
    // and portamento 2^3.99 s instead of 2^2 s
    reference.storage.getPatch().scene[0].portamento.val_max.f = 3.99f;
    // and amp velocity sensitivity down to -1536 dB
    reference.storage.getPatch().scene[0].vca_velsense.val_min.f = -1536.f;
    // and note shift and pitch bend ranges of +-127 semitones (set after each load below, as a
    // load resets the bend ranges)
    auto widenPitch = [&] {
        auto &sc = reference.storage.getPatch().scene[0];
        sc.pitch.val_min.f = -127.f;
        sc.pitch.val_max.f = 127.f;
        sc.pbrange_up.val_max.i = 127;
        sc.pbrange_dn.val_min.i = -127;
        sc.pbrange_dn.val_max.i = 127;
    };
    int loaded = 0;
    for (auto &path : patches)
    {
        auto patch = patchFromFxp(path);
        check(!patch.empty(), "factory patch has a sub3 chunk: " + path);
        if (patch.empty())
            continue;
        bool ok = a.load(patch);
        reference.loadRaw(patch.data(), (int)patch.size(), false);
        widenPitch();
        bool usesFormula = false;
        for (auto &lfo : reference.storage.getPatch().scene[0].lfo)
            usesFormula |= lfo.shape.val.i == lt_formula;
        check(ok != usesFormula, "patch loads unless it uses Formula: " + path);
        if (!ok)
            continue;
        ++loaded;
        for (auto *p : reference.storage.getPatch().param_ptr)
        {
            auto it = infos.find(fnv1a(p->get_storage_name()));
            if (it == infos.end() || p->valtype == vt_bool)
                continue;
            auto &info = it->second;
            double lo = p->valtype == vt_float ? p->val_min.f : p->val_min.i;
            double hi = p->valtype == vt_float ? p->val_max.f : p->val_max.i;
            bool unitRange = info.min_value == 0 && info.max_value == 1;
            check(unitRange || (lo == info.min_value && hi == info.max_value),
                  std::string("range of ") + info.module + "/" + info.name +
                      " doesn't depend on the patch: " + path);
        }
        for (auto &[id, info] : infos)
        {
            double v;
            check(a.params->get_value(a.plugin, id, &v) && v >= info.min_value &&
                      v <= info.max_value,
                  std::string("value of ") + info.name + " in range after loading " + path);
        }
    }
    std::printf("factory patches loaded: %d of %zu\n", loaded, patches.size());

    // Off switches are parameters too ("<storage name>.off"): the amp saturator starts off,
    // its switch turns it on, and the state carries that.
    {
        Instance b;
        auto id = fnv1a("a_amp_drive.off");
        double v = -1;
        check(infos.count(id) && b.params->get_value(b.plugin, id, &v) && v == 1,
              "the amp drive's off switch is a parameter, on (1) by default");
        b.render(256, 256, {paramEvent(0, id, 0)});
        check(b.params->get_value(b.plugin, id, &v) && v == 0, "the off switch follows its event");
        char text[64];
        check(b.params->value_to_text(b.plugin, id, 1, text, sizeof(text)) &&
                  std::string(text) == "Off",
              "off switch text");
        auto st = b.save();
        reference.loadRaw(st.data(), (int)st.size(), false);
        check(!reference.storage.getPatch().scene[0].amp_drive.deactivated,
              "the state carries the switched-on amp drive");
    }
}

static void testStateRoundTrip(const std::vector<std::string> &patches)
{
    Instance a;
    auto first = a.save();
    check(a.load(first), "load own state");
    check(a.save() == first, "default state round trip is byte-identical");
    Instance b;
    check(b.save() == first, "two fresh instances save the same bytes");

    // save -> load -> save gives the same bytes from the first save, also for older
    // patches that loading migrates.
    int tested = 0;
    for (auto &path : patches)
    {
        auto patch = patchFromFxp(path);
        if (!a.load(patch))
            continue;
        auto s1 = a.save();
        check(b.load(s1), "load saved state: " + path);
        check(b.save() == s1, "state round trip is byte-identical: " + path);
        ++tested;
    }
    std::printf("state round trips: %d\n", tested);
}

static std::vector<Event> testEvents(const std::map<std::string, clap_param_info_t> &params)
{
    std::vector<Event> ev;
    ev.push_back(noteEvent(101, CLAP_EVENT_NOTE_ON, 1, 60, 0.8));
    ev.push_back(noteEvent(1033, CLAP_EVENT_NOTE_ON, 2, 64, 0.5));
    ev.push_back(noteEvent(1033, CLAP_EVENT_NOTE_ON, 3, 64, 0.7)); // same key, new ID
    ev.push_back(expressionEvent(5000, CLAP_NOTE_EXPRESSION_TUNING, 2, 0.37));
    ev.push_back(midiEvent(7001, 0xE0, 0x00, 0x50)); // pitch bend
    ev.push_back(midiEvent(15003, 0xB0, 64, 127));  // sustain: releases go to the hold buffer
    ev.push_back(midiEvent(60001, 0xB0, 64, 0));
    ev.push_back(noteEvent(20011, CLAP_EVENT_NOTE_OFF, 1, 60, 0.3));
    ev.push_back(noteEvent(30001, CLAP_EVENT_NOTE_OFF, 2, 64, 0));
    ev.push_back(noteEvent(40007, CLAP_EVENT_NOTE_CHOKE, 3, 64, 0));
    ev.push_back(noteEvent(41003, CLAP_EVENT_NOTE_ON, 4, 67, 1.0));
    ev.push_back(midiEvent(45000, 0x90, 48, 90)); // MIDI note
    ev.push_back(midiEvent(70000, 0x80, 48, 0));
    ev.push_back(noteEvent(80000, CLAP_EVENT_NOTE_OFF, 4, 67, 0.5));
    auto cutoff = params.find("Filter 1/Cutoff");
    if (cutoff != params.end())
        for (uint32_t t = 2001; t < 60000; t += 997)
            ev.push_back(paramEvent(t, cutoff->second.id,
                                    cutoff->second.min_value +
                                        (cutoff->second.max_value - cutoff->second.min_value) *
                                            (0.3 + 0.4 * std::sin(t * 0.001))));
    auto type = params.find("Oscillator 1/Type");
    if (type != params.end())
        ev.push_back(paramEvent(50001, type->second.id, 1)); // sine: respawns the oscillator
    return ev;
}

static void testBlockSizes(const std::vector<std::string> &patches)
{
    std::vector<std::vector<uint8_t>> states{{}}; // {} = default patch
    // A few factory patches with oscillator drift and randomness.
    for (auto name : {"Init Saw", "Analog Brass", "Noise Sweep", "Polysynth"})
        for (auto &p : patches)
            if (p.find(name) != std::string::npos)
            {
                states.push_back(patchFromFxp(p));
                break;
            }

    for (auto &st : states)
    {
        for (double rate : {44100.0, 48000.0, 96000.0})
        {
            std::vector<std::vector<float>> renders;
            for (uint32_t block : {512u, 37u, 512u, 1u})
            {
                Instance a(rate);
                if (!st.empty())
                    a.load(st);
                auto ev = testEvents(a.paramsByName());
                uint32_t frames = block == 1 ? 24000 : (uint32_t)(rate * 2.5);
                renders.push_back(a.render(frames, block, ev));
            }
            float peak = 0;
            for (auto v : renders[0])
                peak = std::max(peak, std::fabs(v));
            auto label = std::to_string((int)rate) + " Hz, state " +
                         std::to_string(&st - &states[0]);
            check(peak > 1e-3f, "render is not silent: " + label);
            check(renders[0] == renders[1], "37- and 512-frame blocks render the same: " + label);
            check(renders[0] == renders[2], "rendering twice gives the same samples: " + label);
            check(std::equal(renders[3].begin(), renders[3].end(), renders[0].begin()),
                  "1-frame blocks render the same: " + label);
        }
    }
}

// A patch that doesn't turn on the reference-matching engine options plays in the plugin
// exactly as in a stock Surge synthesizer (default options and parameter ranges): every 8th
// factory patch, loaded into each (in stock Surge with the plugin's one-scene, no-effects
// rules applied by hand) and rendered note for note, bit-identical. The plugin's longer envelope, LFO, portamento and velocity ranges
// change only how far a parameter can be set, not what a stored value plays.
static void testPlainPatchesMatchStock(const std::vector<std::string> &patches)
{
    const double rate = 48000;
    const uint32_t frames = 48000;
    int compared = 0, differing = 0;
    // plus a patch where each option would show: analog amp envelope with the shortest attack,
    // velocity sensitivity, amp saturator on, an LFO envelope release at 5
    std::vector<uint8_t> built;
    {
        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto s = std::make_unique<SurgeSynthesizer>(&layer, config);
        auto &sc = s->storage.getPatch().scene[0];
        sc.adsr[0].mode.val.i = emt_analog;
        sc.adsr[0].a.val.f = sc.adsr[0].a.val_min.f;
        sc.vca_velsense.val.f = -12;
        sc.amp_drive.deactivated = false;
        sc.amp_drive.val.f = 12;
        // voice LFO 1 as an envelope on pitch that never releases (release at Surge's maximum)
        sc.lfo[0].shape.val.i = lt_envelope;
        sc.lfo[0].release.val.f = 5;
        sc.adsr[0].r.val.f = 2;
        s->setModDepth01(sc.osc[0].pitch.id, ms_lfo1, 0, 0, 0.2f);
        void *data = nullptr;
        auto size = s->saveRaw(&data);
        built.assign((uint8_t *)data, (uint8_t *)data + size);
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> states;
    for (size_t i = 0; i < patches.size(); i += 8)
        states.emplace_back(patches[i], patchFromFxp(patches[i]));
    states.emplace_back("built", built);
    for (auto &[name, patch] : states)
    {
        if (patch.empty())
            continue;
        Instance a(rate);
        if (!a.load(patch))
            continue; // refused (a formula LFO)
        auto plugin = a.render(frames, 512,
                               {noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 70 / 127.0),
                                noteEvent(24000, CLAP_EVENT_NOTE_OFF, 1, 60, 0)},
                               false);

        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto stock = std::make_unique<SurgeSynthesizer>(&layer, config);
        stock->setSamplerate((float)rate);
        stock->seedRandom(0x05CA1A4E); // the plugin's render seed
        // the patch itself, with the plugin's one-scene, no-effects rules applied by hand
        stock->loadRaw(patch.data(), (int)patch.size(), false);
        auto &sp = stock->storage.getPatch();
        sp.scenemode.val.i = sm_single;
        sp.scene_active.val.i = 0;
        sp.fx_disable.val.i = (1 << n_fx_slots) - 1;
        sp.copy_globaldata(sp.globaldata);
        sp.copy_scenedata(sp.scenedata[0], sp.scenedataOrig[0], 0);
        stock->storage.perform_queued_wtloads();
        stock->audio_processing_active = true;
        std::vector<float> ref;
        ref.reserve(frames * 2);
        for (uint32_t b = 0; b * BLOCK_SIZE < frames; ++b)
        {
            if (b == 0)
                stock->playNote(0, 60, 70, 0, 1);
            if (b * BLOCK_SIZE == 24000)
                stock->releaseNote(0, 60, 0, 1);
            stock->process();
            stock->time_data.ppqPos += (double)BLOCK_SIZE * stock->time_data.tempo / (60. * rate);
            for (int k = 0; k < BLOCK_SIZE; ++k)
            {
                ref.push_back(stock->output[0][k]);
                ref.push_back(stock->output[1][k]);
            }
        }
        ref.resize(plugin.size());
        ++compared;
        if (plugin != ref)
        {
            ++differing;
            check(false, "a plain patch plays as in stock Surge: " + name);
        }
    }
    check(compared > 50, "plain patches compared: " + std::to_string(compared));

    // turned on in a patch, the options survive the plugin's state, byte for byte
    {
        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto s = std::make_unique<SurgeSynthesizer>(&layer, config);
        s->loadRaw(built.data(), (int)built.size(), false);
        s->storage.analogEnvelopeAttackClamp = true;
        s->storage.vcaVelocityAmplitudeLaw = true;
        s->storage.saturatorAlgebraicCurve = true;
        s->storage.lfoLongRelease = true;
        s->storage.filterCutoffRateMatch = true;
        void *data = nullptr;
        auto size = s->saveRaw(&data);
        std::vector<uint8_t> on((uint8_t *)data, (uint8_t *)data + size);
        Instance a, b;
        check(a.load(on), "a patch with the options on loads");
        auto saved = a.save();
        std::string text(saved.begin(), saved.end());
        check(text.find("analogEnvelopeAttackClamp") != std::string::npos &&
                  text.find("vcaVelocityAmplitudeLaw") != std::string::npos &&
                  text.find("saturatorAlgebraicCurve") != std::string::npos &&
                  text.find("lfoLongRelease") != std::string::npos &&
                  text.find("filterCutoffRateMatch") != std::string::npos,
              "the plugin's state keeps the options");
        check(b.load(saved) && b.save() == saved, "the options round trip through the state");
        auto x = a.render(24000, 512, {noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 70 / 127.0)}, false);
        Instance c;
        c.load(built);
        auto y = c.render(24000, 512, {noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 70 / 127.0)}, false);
        check(x != y, "the options change the sound of a patch that uses them");
    }
    std::printf("  plain patches: %d compared, %d differ from stock\n", compared, differing);
}

static bool hasNoteEnd(const Instance &a, int32_t id)
{
    return std::any_of(a.out.noteEnds.begin(), a.out.noteEnds.end(),
                       [&](const clap_event_note_t &n) { return n.note_id == id; });
}

static void testNotes()
{
    {
        Instance a;
        a.render(4800, 256, {noteEvent(10, CLAP_EVENT_NOTE_ON, 7, 60, 0.0)});
        check(!hasNoteEnd(a, 7), "velocity-0 note-on keeps sounding until released");
        a.render(48000 * 4, 256, {noteEvent(a.position + 5, CLAP_EVENT_NOTE_OFF, 7, 60, 0)});
        auto n = std::count_if(a.out.noteEnds.begin(), a.out.noteEnds.end(),
                               [](auto &e) { return e.note_id == 7; });
        check(n == 1, "velocity-0 note-on started a note that ends once");
    }
    {
        Instance a;
        a.render(4800, 256,
                 {noteEvent(10, CLAP_EVENT_NOTE_ON, 11, 60, 0.9),
                  noteEvent(20, CLAP_EVENT_NOTE_ON, 12, 60, 0.9)});
        a.render(48000 * 4, 256,
                 {noteEvent(a.position + 1, CLAP_EVENT_NOTE_OFF, 11, 60, 0),
                  noteEvent(a.position + 1, CLAP_EVENT_NOTE_OFF, 12, 60, 0)});
        check(hasNoteEnd(a, 11) && hasNoteEnd(a, 12), "NOTE_END for two notes on one key");
        for (auto &e : a.out.noteEnds)
            check(e.key == 60 && e.channel == 0, "NOTE_END carries the note's key and channel");
    }
    {
        Instance a;
        auto st = a.save();
        a.render(4800, 256, {noteEvent(10, CLAP_EVENT_NOTE_ON, 9, 60, 0.9)});
        check(a.load(st), "state load while a note is held");
        a.render(256, 256);
        check(hasNoteEnd(a, 9), "NOTE_END for a note ended by a state load");
    }
    // Key-on delay (0.25 s): the note starts late; released or loaded over before it starts,
    // it never sounds and still ends.
    auto peak = [](const std::vector<float> &lr, size_t from, size_t to) {
        float m = 0;
        for (size_t i = 2 * from; i < 2 * to && i < lr.size(); ++i)
            m = std::max(m, std::fabs(lr[i]));
        return m;
    };
    auto delayed = [](uint32_t t) {
        return std::vector<Event>{paramEvent(t, fnv1a("a_keyon_delay"), -2),
                                  paramEvent(t, fnv1a("a_keyon_delay.off"), 0)};
    };
    {
        Instance a;
        auto ev = delayed(0);
        ev.push_back(noteEvent(0, CLAP_EVENT_NOTE_ON, 21, 60, 0.9));
        auto lr = a.render(24000, 256, ev);
        size_t first = 0;
        while (first < lr.size() / 2 && std::fabs(lr[2 * first]) < 1e-4f)
            ++first;
        check(first >= 12000 - 64 && first <= 12000 + 64,
              "a delayed note starts after its key-on delay: " + std::to_string(first) +
                  " samples");
        a.render(48000 * 4, 256, {noteEvent(a.position + 1, CLAP_EVENT_NOTE_OFF, 21, 60, 0)});
        check(hasNoteEnd(a, 21), "NOTE_END for a delayed note");
    }
    {
        Instance a;
        auto ev = delayed(0);
        ev.push_back(noteEvent(0, CLAP_EVENT_NOTE_ON, 22, 60, 0.9));
        ev.push_back(noteEvent(4800, CLAP_EVENT_NOTE_OFF, 22, 60, 0));
        auto lr = a.render(48000, 256, ev);
        check(peak(lr, 0, 48000) == 0, "a note released before its key-on delay never sounds");
        check(hasNoteEnd(a, 22), "NOTE_END for a note released before its key-on delay");
    }
    {
        Instance a;
        a.render(256, 256, delayed(0));
        auto st = a.save();
        a.render(4800, 256, {noteEvent(a.position + 1, CLAP_EVENT_NOTE_ON, 23, 60, 0.9)});
        check(a.load(st), "state load while a note waits for its key-on delay");
        auto lr = a.render(24000, 256);
        check(peak(lr, 0, 24000) == 0 && hasNoteEnd(a, 23),
              "a state load ends a note waiting for its key-on delay");
    }
}

static void testFormulaRefused()
{
    HeadlessLayer layer;
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    auto synth = std::make_unique<SurgeSynthesizer>(&layer, config);
    auto &s = *synth;
    s.storage.getPatch().scene[0].lfo[0].shape.val.i = lt_formula;
    void *data = nullptr;
    auto size = s.saveRaw(&data);
    std::vector<uint8_t> formula((uint8_t *)data, (uint8_t *)data + size);

    Instance a;
    auto before = a.save();
    check(!a.load(formula), "a patch with a Formula modulator is refused");
    check(a.save() == before, "a refused load leaves the previous state");
    check(!a.load({'n', 'o', 'p', 'e'}), "garbage state is refused");
}

static void testConcurrentState()
{
    Instance a;
    auto st0 = a.save();
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        uint32_t t = 0;
        while (!stop.load())
        {
            int32_t id = (int32_t)(t % 1000);
            auto key = (int16_t)(48 + t % 24);
            a.render(512, 128,
                     {noteEvent(a.position + 3, CLAP_EVENT_NOTE_ON, id, key, 0.8),
                      noteEvent(a.position + 300, CLAP_EVENT_NOTE_OFF, id, key, 0)},
                     false);
            ++t;
        }
    });
    for (int i = 0; i < 40; ++i)
    {
        check(a.load(st0), "load while rendering");
        auto s = a.save();
        check(s == st0, "save while rendering gives the loaded state");
    }
    stop = true;
    audio.join();
}

// --- fitted tables -------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
static constexpr bool sanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
static constexpr bool sanitizerBuild = true;
#else
static constexpr bool sanitizerBuild = false;
#endif
#else
static constexpr bool sanitizerBuild = false;
#endif

// The sample rate the reference checks render at: the reference was measured at 44.1 kHz, and
// the checks run again at 48 and 96 kHz (see main).
static double checkRate = 44100;

static std::vector<float> renderMono(oscairn_analog::RenderSpec spec)
{
    spec.rate = checkRate;
    spec.referenceMatching = true; // every use compares against the reference
    spec.params.insert(spec.params.begin(), {{"a_drift", 0}, {"a_env1_mode", 1}});
    auto lr = oscairn_analog::renderNote(spec);
    std::vector<float> m(lr.size() / 2);
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
    return m;
}

// renderNote loads the patch and starts the note at the first sample of the first block: the
// voice must start with the loaded patch's values, not the scene data of the patch before.
// A sine LFO on pitch (1 Hz, +-100 cents) starting at phase 0.25 holds the pitch up for the
// first 0.2 s, at 0.75 down: about 6 % more or fewer periods.
static void testFirstBlockAfterLoad()
{
    namespace F = oscairn_features;
    auto periods = [](float phase) {
        auto x = renderMono({60, 100, 0.5, 0.1, 44100,
                             {{"a_osc1_type", 1}, {"a_lfo0_rate", 0}, {"a_lfo0_phase", phase}},
                             {{17, "a_osc1_pitch", 100.0 / 1400}}});
        size_t a = (size_t)(F::onset(x, 44100) * 44100), n = 0;
        for (size_t i = a; i + 1 < x.size() && i < a + 8820; ++i)
            n += x[i] < 0 && x[i + 1] >= 0;
        return (int)n;
    };
    int up = periods(0.25f), down = periods(0.75f);
    check(up - down >= 4, "a note in the first block after a load starts at the loaded LFO "
                          "phase: " + std::to_string(up) + " and " + std::to_string(down) +
                          " periods");
}

// Voice unison against the reference (sixth session, batch m3a: a sine at note 84, unison 2
// and 4 at detune 8, spread 0, 4, 8 and 15), with the converter's mapping: detune 7.98 cents
// (index x 0.9975), spread the far-side position x (0.186, 0.385, 0.717 at 4, 8, 15). Each
// partial's offset in cents and its left and right amplitude relative to a single voice,
// from a Hann-windowed DFT over the held part searched around the reference's offset.
// Proposed tolerances: 0.1 cent, 0.02.
static void testUnisonAgainstReference()
{
    const double rate = 44100, f0 = 440 * std::pow(2.0, 15 / 12.0);
    auto render = [&](int voices, double spread) {
        oscairn_analog::RenderSpec s{84, 100, 3, 0.1, rate,
                                     {{"a_drift", 0}, {"a_lowcut.off", 1}, {"a_osc1_type", 1}},
                                     {}};
        if (voices > 1)
            s.params.insert(s.params.end(), {{"a_unison_voices", voices},
                                             {"a_unison_detune", 0.0798},
                                             {"a_unison_spread", spread}});
        s.referenceMatching = true;
        return oscairn_analog::renderNote(s);
    };
    // amplitude of each channel at f over the held part (0.1 s after the start to 0.15 s
    // before the end of the hold)
    auto dft = [&](const std::vector<float> &lr, double f, double &l, double &r) {
        size_t a = (size_t)(0.1 * rate), b = (size_t)((3 - 0.15) * rate), n = b - a;
        double lr0 = 0, li = 0, rr0 = 0, ri = 0, ws = 0;
        for (size_t k = 0; k < n; ++k)
        {
            double w = 0.5 - 0.5 * std::cos(2 * M_PI * k / (n - 1)), p = 2 * M_PI * f * k / rate;
            double c = std::cos(p) * w, si = std::sin(p) * w;
            lr0 += lr[2 * (a + k)] * c;
            li += lr[2 * (a + k)] * si;
            rr0 += lr[2 * (a + k) + 1] * c;
            ri += lr[2 * (a + k) + 1] * si;
            ws += w;
        }
        l = 2 * std::hypot(lr0, li) / ws;
        r = 2 * std::hypot(rr0, ri) / ws;
    };
    // the peak within 1 cent of `cents`: 0.05-cent steps, then a parabola
    auto partial = [&](const std::vector<float> &lr, double cents, double &l, double &r) {
        double prev[3];
        std::vector<double> m;
        for (int i = -20; i <= 20; ++i)
        {
            double a, b;
            dft(lr, f0 * std::pow(2.0, (cents + 0.05 * i) / 1200), a, b);
            m.push_back(a + b);
        }
        size_t k = std::max_element(m.begin() + 1, m.end() - 1) - m.begin();
        for (int j = 0; j < 3; ++j)
            prev[j] = std::log(m[k - 1 + j]);
        double d = 0.5 * (prev[0] - prev[2]) / (prev[0] - 2 * prev[1] + prev[2]);
        double bc = cents + 0.05 * ((double)k - 20 + d);
        dft(lr, f0 * std::pow(2.0, bc / 1200), l, r);
        return bc;
    };
    double one, unused;
    dft(render(1, 0), f0, one, unused);
    struct P
    {
        double cents, l, r;
    };
    struct Case
    {
        int voices;
        double spread;
        std::vector<P> ref;
    };
    const Case cases[] = {
        {2, 0, {{-7.98, 0.707, 0.707}, {7.99, 0.707, 0.707}}},
        {2, 0.186, {{-7.98, 0.707, 0.648}, {7.99, 0.648, 0.707}}},
        {2, 0.385, {{-7.98, 0.707, 0.479}, {7.99, 0.479, 0.707}}},
        {2, 0.717, {{-7.98, 0.707, 0.131}, {7.99, 0.131, 0.707}}},
        {4, 0, {{-15.96, 0.5, 0.5}, {-5.27, 0.5, 0.5}, {5.28, 0.5, 0.5}, {15.97, 0.5, 0.5}}},
        {4, 0.186, {{-15.96, 0.5, 0.339}, {-5.27, 0.5, 0.479}, {5.28, 0.479, 0.5}, {15.97, 0.339, 0.5}}},
        {4, 0.385, {{-15.96, 0.5, 0.065}, {-5.27, 0.5, 0.42}, {5.28, 0.42, 0.5}, {15.97, 0.065, 0.5}}},
    };
    for (auto &c : cases)
    {
        auto lr = render(c.voices, c.spread);
        for (auto &p : c.ref)
        {
            double l, r, cents = partial(lr, p.cents, l, r);
            l /= one;
            r /= one;
            auto what = "unison " + std::to_string(c.voices) + " spread " + std::to_string(c.spread) +
                        " partial at " + std::to_string(p.cents) + " cents: engine " +
                        std::to_string(cents) + " cents, " + std::to_string(l) + " / " +
                        std::to_string(r);
            check(std::fabs(cents - p.cents) <= 0.1 && std::fabs(l - p.l) <= 0.02 &&
                      std::fabs(r - p.r) <= 0.02,
                  what);
        }
    }
}

// Segment drift against the reference's spread of the pitch at a note's start (sixth session:
// deviation 19.9 cents at drift 127 over 43 loads, 10.5 at 112 over 18), with the converter's
// mapping (Segment Drift Pitch = 1.22 x the deviation: a voice starts at a random point of a
// segment, whose spread is sqrt(2/3) of the nodes'). 150 notes of a sine at note 60 in one
// instance, each note's mean pitch 30-200 ms after its start. Proposed tolerance: 20 % (the
// reference's own estimate is good to about 11 % at 127). The reference's mean (+11.9 cents
// at 127) isn't modelled.
static void testDriftAgainstReference()
{
    const double rate = 44100, c4 = 440 * std::pow(2.0, -9 / 12.0);
    for (auto [deviation, loads] : {std::pair{19.9, 43}, {10.5, 18}})
    {
        Instance a(rate);
        std::vector<Event> ev{paramEvent(0, fnv1a("a_osc1_type"), 1),
                              paramEvent(0, fnv1a("a_segdrift_pitch"), 1.22 * deviation / 100)};
        const int n = 150;
        const uint32_t step = (uint32_t)(0.3 * rate);
        for (int i = 0; i < n; ++i)
        {
            ev.push_back(noteEvent(1000 + i * step, CLAP_EVENT_NOTE_ON, i, 60, 0.8));
            ev.push_back(noteEvent(1000 + i * step + (uint32_t)(0.25 * rate), CLAP_EVENT_NOTE_OFF,
                                   i, 60, 0));
        }
        auto lr = a.render(1000 + n * step, 512, ev);
        double sum = 0, sum2 = 0;
        for (int i = 0; i < n; ++i)
        {
            size_t from = 1000 + i * step + (size_t)(0.03 * rate),
                   to = 1000 + i * step + (size_t)(0.2 * rate);
            double first = -1, last = -1;
            int periods = -1;
            for (size_t k = from; k < to; ++k)
                if (lr[2 * k] < 0 && lr[2 * (k + 1)] >= 0)
                {
                    double z = k + lr[2 * k] / (lr[2 * k] - lr[2 * (k + 1)]);
                    if (first < 0)
                        first = z;
                    last = z;
                    ++periods;
                }
            double cents = 1200 * std::log2(periods * rate / (last - first) / c4);
            sum += cents;
            sum2 += cents * cents;
        }
        double sd = std::sqrt(sum2 / n - (sum / n) * (sum / n));
        check(std::fabs(sd / deviation - 1) <= 0.2,
              "segment drift spread at a note's start: reference " + std::to_string(deviation) +
                  " cents (" + std::to_string(loads) + " loads), engine " + std::to_string(sd) +
                  " over " + std::to_string(n) + " notes");
        std::printf("  drift: reference %.1f cents, engine %.2f (mean %.2f)\n", deviation, sd,
                    sum / n);
    }
}

// Segment drift on the cutoff against the reference (sixth session batch m3d and ninth
// session batch m9i: a saw at note 36 through LPF24 at cutoff index 512, Voltage Drift 96 and
// 127, 40 loads each): the spread of harmonic 9's level (Hann DFT 0.05-0.5 s after each note's
// start, at the note's own pitch), with the converter's mapping Segment Drift Cutoff = 1.22 x
// the deviation in semitones (1.37 and 6.71; harmonic 9 moves about 1.97 dB per semitone
// there). 150 notes in one instance. Proposed tolerance: 20 %, as for the pitch.
static void testCutoffDriftAgainstReference()
{
    namespace T = oscairn_tables;
    const double rate = 44100, f9 = 9 * 440 * std::pow(2.0, (36 - 69) / 12.0);
    for (auto [semitones, refDb] : {std::pair{1.12, 2.28}, {5.5, 9.80}})
    {
        Instance a(rate);
        std::vector<Event> ev{paramEvent(0, fnv1a("a_filter1_type"), 2),
                              paramEvent(0, fnv1a("a_filter1_subtype"), 0),
                              paramEvent(0, fnv1a("a_filter1_resonance"), 0.06),
                              paramEvent(0, fnv1a("a_filter1_cutoff"), T::cutoffLp24[512]),
                              paramEvent(0, fnv1a("a_lowcut.off"), 1),
                              paramEvent(0, fnv1a("a_drift"), 0),
                              paramEvent(0, fnv1a("a_segdrift_cutoff"), 1.22 * semitones)};
        const int n = 150;
        const uint32_t step = (uint32_t)(0.7 * rate);
        for (int i = 0; i < n; ++i)
        {
            ev.push_back(noteEvent(1000 + i * step, CLAP_EVENT_NOTE_ON, i, 36, 0.8));
            ev.push_back(noteEvent(1000 + i * step + (uint32_t)(0.6 * rate), CLAP_EVENT_NOTE_OFF,
                                   i, 36, 0));
        }
        auto lr = a.render(1000 + n * step, 512, ev);
        double sum = 0, sum2 = 0;
        for (int i = 0; i < n; ++i)
        {
            size_t from = 1000 + i * step + (size_t)(0.05 * rate),
                   len = (size_t)(0.45 * rate);
            double re = 0, im = 0, ws = 0;
            for (size_t k = 0; k < len; ++k)
            {
                double w = 0.5 - 0.5 * std::cos(2 * M_PI * k / (len - 1));
                double x = 0.5 * (lr[2 * (from + k)] + lr[2 * (from + k) + 1]);
                re += x * w * std::cos(2 * M_PI * f9 * k / rate);
                im += x * w * std::sin(2 * M_PI * f9 * k / rate);
                ws += w;
            }
            double db = 20 * std::log10(2 * std::hypot(re, im) / ws);
            sum += db;
            sum2 += db * db;
        }
        double sd = std::sqrt((sum2 - sum * sum / n) / (n - 1));
        check(std::fabs(sd / refDb - 1) <= 0.2,
              "segment drift spread of the cutoff: reference " + std::to_string(refDb) +
                  " dB, engine " + std::to_string(sd) + " over " + std::to_string(n) + " notes");
        std::printf("  cutoff drift: reference %.2f dB, engine %.2f\n", refDb, sd);
    }
}

// Pitch LFO start phases against the reference (sixth session, batch m3b: a sine at note 60,
// Pitch LFO Speed 176, depth raw 309, Key On Reset on), with the converter's mapping: voice
// LFO 1 at 2.397 Hz on oscillator 1's pitch, +-92.5 cents, key trigger; sine, triangle and
// square at phase 0, the saw as Surge's ramp with the depth negated at phase 0.5, and phases
// 90/180/270 degrees as 0.25/0.5/0.75 on the sine. The mean pitch of each 50 ms from 0.05 to
// 1.25 s after the onset; the RMS difference over those 24 bins must be within 6 cents (a
// proposed tolerance; the reference starts about 2 ms late, which the bins don't remove; an
// inverted LFO is 96-170 cents off).
static void testPitchLfoPhasesAgainstReference()
{
    namespace F = oscairn_features;
    const double rate = 44100, c4 = 440 * std::pow(2.0, -9 / 12.0), depth = 92.5 / 1400;
    struct Case
    {
        const char *name;
        int shape;
        double phase, sign;
        double ref[24];
    };
    const Case cases[] = {
        {"sine", 0, 0, 1, {80.9, 86.1, 44.8, -20.2, -75.7, -89.0, -53.7, 10.9, 69.5, 90.5, 63.5, 1.7,
                           -61.1, -90.3, -70.1, -11.0, 52.4, 88.4, 78.2, 25.6, -42.8, -85.7, -83.5, -37.4}},
        {"triangle", 1, 0, 1, {65.4, 73.9, 29.9, -14.0, -59.1, -79.2, -37.9, 6.6, 50.0, 81.2, 45.4, 0.2,
                               -42.7, -80.2, -52.5, -7.6, 36.2, 78.0, 60.9, 17.8, -26.5, -71.7, -68.9, -25.4}},
        {"square", 2, 0, 1, {92.6, 92.6, 92.6, -50.2, -92.6, -92.6, -92.6, 30.2, 92.6, 92.6, 92.6, 10.8,
                             -92.6, -92.6, -92.6, -35.6, 92.5, 92.6, 92.6, 72.0, -92.7, -92.6, -92.6, -92.6}},
        {"saw", 3, 0.5, -1, {32.9, 55.2, 77.2, -50.1, -63.3, -41.4, -19.0, 3.2, 25.1, 47.5, 69.6, 8.0,
                             -70.7, -48.7, -26.2, -3.9, 18.0, 39.7, 62.0, 67.6, -78.9, -56.0, -34.3, -12.8}},
        {"sine at 90 degrees", 0, 0.25, 1, {40.3, -27.7, -78.7, -87.6, -49.7, 15.8, 72.8, 89.9, 58.1, -6.0, -65.1, -90.2,
                                            -66.9, -8.6, 56.1, 89.6, 75.4, 20.9, -45.0, -86.9, -80.0, -30.1, 35.8, 82.7}},
        {"sine at 180 degrees", 0, 0.5, 1, {-81.4, -86.2, -44.2, 21.7, 75.8, 89.4, 55.4, -9.4, -69.0, -90.4, -62.3, 0.1,
                                            60.8, 90.0, 70.5, 12.4, -52.4, -88.7, -78.3, -26.8, 41.1, 85.1, 83.6, 35.6}},
        {"sine at 270 degrees", 0, 0.75, 1, {-41.0, 27.5, 79.2, 88.0, 50.8, -13.0, -71.4, -90.1, -59.5, 3.7, 64.9, 90.3,
                                             66.8, 8.9, -55.4, -89.4, -74.6, -18.2, 46.5, 86.7, 80.1, 30.0, -36.1, -82.9}},
    };
    for (auto &c : cases)
    {
        auto x = renderMono({60, 100, 1.6, 0.1, rate,
                             {{"a_lowcut.off", 1}, {"a_osc1_type", 1},
                              {"a_lfo0_rate", std::log2(2.397)}, {"a_lfo0_shape", c.shape},
                              {"a_lfo0_phase", c.phase}, {"a_lfo0_trigmode", 1}},
                             {{17, "a_osc1_pitch", c.sign * depth}}});
        double t0 = F::onset(x, rate), sum[24] = {}, err = 0;
        int count[24] = {}, prevSet = 0;
        double prev = 0;
        for (size_t i = 0; i + 1 < x.size(); ++i)
            if (x[i] < 0 && x[i + 1] >= 0)
            {
                double z = i + x[i] / (x[i] - x[i + 1]);
                if (prevSet)
                {
                    int b = (int)std::floor(((prev + z) / 2 / rate - t0) / 0.05) - 1;
                    if (b >= 0 && b < 24)
                    {
                        sum[b] += 1200 * std::log2(rate / (z - prev) / c4);
                        ++count[b];
                    }
                }
                prev = z;
                prevSet = 1;
            }
        for (int b = 0; b < 24; ++b)
        {
            double d = (count[b] ? sum[b] / count[b] : 0) - c.ref[b];
            err += d * d / 24;
        }
        err = std::sqrt(err);
        check(err <= 6, std::string("pitch LFO ") + c.name + " from the note's start: " +
                            std::to_string(err) + " cents RMS from the reference");
        std::printf("  pitch LFO %s: %.2f cents RMS\n", c.name, err);
    }
}

// Amp velocity sensitivity against the reference (batches m8c, m8d and m10a: a sine at note 60,
// sensitivity S = +64 to +255, level 0.2-0.7 s after the onset relative to velocity 127), with
// the converter's mapping Velocity > VCA Gain = -12 k dB, k = 2^((S - 128) / 64) up to +192 and
// 128 / (256 - S) above. Proposed tolerance: 0.3 dB up to +192. Above, the level moves by up to
// 1.7 dB per unit of k near silence, so the check is on k itself: k = (1 - 10^(dB / 40)) / (1 -
// v / 127) from the engine's level within 2 % of the same from the reference's (proposed; in
// dB the largest difference is 0.38, at +252, velocity 124). Where the reference is silent the
// engine is at least 60 dB down. Levels below -40 dB in the reference are not checked (S = +240
// at velocity 112 reads -51.9 dB, the law -50.4).
static void testVelocityAgainstReference()
{
    namespace F = oscairn_features;
    const double rate = 44100;
    auto level = [&](double sens, int vel) {
        auto x = renderMono({60, vel, 0.8, 0.1, rate,
                             {{"a_lowcut.off", 1}, {"a_osc1_type", 1}, {"a_vca_velsense", sens}},
                             {}});
        double t0 = F::onset(x, rate);
        return t0 < 0 ? -1000.0 : F::levelDb(x, rate, t0 + 0.2, t0 + 0.7); // silent: no onset
    };
    const double silent = -1000;
    struct Case
    {
        int s;
        std::vector<std::pair<int, double>> ref; // velocity, dB relative to velocity 127
    };
    const Case cases[] = {
        {64, {{1, -11.90}, {32, -8.16}, {64, -4.99}, {96, -2.27}, {112, -1.06}}},
        {128, {{16, -36.12}, {32, -24.08}, {64, -12.04}, {96, -4.90}, {112, -2.19}}},
        {192, {{96, -11.75}, {112, -4.71}, {120, -2.04}}},
        {224, {{112, -11.18}, {120, -4.33}, {124, -1.71}}},
        {240, {{120, -10.12}, {124, -3.61}}},
        {248, {{112, silent}, {120, -37.31}, {124, -8.18}}},
        {252, {{112, silent}, {120, silent}, {124, -24.12}}},
        {254, {{112, silent}, {120, silent}, {124, silent}}},
        {255, {{118, silent}, {122, silent}, {124, silent}, {126, silent}}},
    };
    double worst = 0, worstK = 0, worstDb = 0;
    for (auto &c : cases)
    {
        double k = c.s <= 192 ? std::pow(2.0, (c.s - 128) / 64.0) : 128.0 / (256 - c.s);
        double sens = -12 * k, top = level(sens, 127);
        for (auto [vel, ref] : c.ref)
        {
            double got = level(sens, vel) - top;
            auto kOf = [vel = vel](double db) {
                return (1 - std::pow(10.0, db / 40)) / (1 - vel / 127.0);
            };
            if (ref != silent && c.s > 192)
            {
                double dk = kOf(got) / kOf(ref) - 1;
                worstK = std::max(worstK, std::fabs(dk));
                check(std::fabs(dk) <= 0.02, "amp velocity sensitivity " + std::to_string(c.s) +
                                                 " at velocity " + std::to_string(vel) +
                                                 ": k from the reference's level " +
                                                 std::to_string(kOf(ref)) + ", engine " +
                                                 std::to_string(kOf(got)));
                worstDb = std::max(worstDb, std::fabs(got - ref));
                continue;
            }
            if (ref == silent)
            {
                check(got < -60, "amp velocity sensitivity " + std::to_string(c.s) +
                                     " at velocity " + std::to_string(vel) +
                                     ": silent in the reference, engine " + std::to_string(got));
                continue;
            }
            worst = std::max(worst, std::fabs(got - ref));
            check(std::fabs(got - ref) <= 0.3,
                  "amp velocity sensitivity " + std::to_string(c.s) + " at velocity " +
                      std::to_string(vel) + ": reference " + std::to_string(ref) +
                      " dB, engine " + std::to_string(got));
        }
    }
    std::printf("  velocity: largest difference %.3f dB up to +192; above, k %.2f %% (%.3f dB)\n",
                worst, 100 * worstK, worstDb);
}

// Note shift (scene pitch) and pitch bend ranges at their extremes, past the reference's
// (+-48 semitones of note shift, bend ranges -48 to +24): a sine's frequency from 3 s (the bend
// is smoothed) to the end of the note, from rising zero crossings, within 1 cent plus 0.05 % of
// the shift of the note the shift or bend should reach (Surge's bend settles 0.027 % short).
// +-48 is the reference's largest documented shift; it maps one to one.
static void testNoteShiftAndBendRanges()
{
    namespace F = oscairn_features;
    const double rate = 44100;
    struct Case
    {
        int note;
        const char *param;
        double value, bend, expect; // expect: the note reached
    };
    const double up = 8191 / 8192.0; // a full bend up is 8191 of 8192
    const Case cases[] = {
        {60, "a_pitch", 48, 0, 108},     {69, "a_pitch", -48, 0, 21},
        {0, "a_pitch", 127, 0, 127},     {127, "a_pitch", -127, 0, 0},
        {0, "a_pbrange_up", 127, 1, 127 * up}, {108, "a_pbrange_dn", 48, -1, 60},
        {0, "a_pbrange_dn", -127, -1, 127},
    };
    double worst = 0;
    for (auto &c : cases)
    {
        oscairn_analog::RenderSpec s{c.note, 100, 5, 0.1, rate,
                                     {{"a_lowcut.off", 1}, {"a_osc1_type", 1}, {c.param, c.value}},
                                     {}};
        s.bend = c.bend;
        auto x = renderMono(s);
        size_t a = (size_t)((F::onset(x, rate) + 3.0) * rate), b = (size_t)(4.95 * rate);
        double first = -1, last = -1;
        int n = 0;
        for (size_t i = a; i + 1 < b && i + 1 < x.size(); ++i)
            if (x[i] < 0 && x[i + 1] >= 0)
            {
                double t = i + x[i] / (x[i] - x[i + 1]);
                if (first < 0)
                    first = t;
                last = t;
                ++n;
            }
        double f = n > 1 ? (n - 1) * rate / (last - first) : 0;
        double want = 440 * std::pow(2.0, (c.expect - 69) / 12.0);
        double cents = f > 0 ? 1200 * std::log2(f / want) : 1e9;
        worst = std::max(worst, std::fabs(cents));
        double tol = 1 + 0.0005 * 100 * std::fabs(c.expect - c.note);
        check(std::fabs(cents) <= tol, std::string(c.param) + " " + std::to_string(c.value) +
                                         " at note " + std::to_string(c.note) + ": " +
                                         std::to_string(f) + " Hz, expected " +
                                         std::to_string(want));
    }
    std::printf("  note shift and bend ranges: largest difference %.2f cents\n", worst);
}

// Amp and filter saturators against the reference (fifth session, batch m2: a sine at note 48,
// drive raw 32-80, filter LPF24 at index 1004), with the converter's mapping: drive = offset +
// 0.75 dB x raw, offset -40.4 dB (amp) and -42.1 dB (filter), asymmetry 0.115 and 0.254, the
// plugin's algebraic curve. Harmonics 2-6 relative to the fundamental over 0.3-0.9 s after the
// onset; those above -50 dB in the reference within 3 dB (proposed tolerance).
static void testSaturatorsAgainstReference()
{
    namespace F = oscairn_features;
    namespace T = oscairn_tables;
    const double rate = checkRate, f0 = 440 * std::pow(2.0, -21 / 12.0);
    struct Case
    {
        bool amp;
        int raw;
        double ref[5];
    };
    const Case cases[] = {
        {true, 32, {-41.5, -51.5, -87.1, -90.3, -116.8}},
        {true, 48, {-31.0, -28.8, -53.6, -53.9, -76.2}},
        {true, 64, {-28.7, -14.3, -35.9, -24.3, -43.1}},
        {true, 80, {-36.6, -10.1, -37.7, -15.5, -39.4}},
        {false, 32, {-37.6, -57.2, -88.6, -89.3, -112.5}},
        {false, 48, {-26.4, -34.0, -54.3, -64.3, -82.1}},
        {false, 64, {-21.3, -16.6, -31.2, -29.2, -41.0}},
        {false, 80, {-27.1, -10.6, -29.3, -16.7, -32.1}},
    };
    double worst = 0;
    for (auto &c : cases)
    {
        std::string p = c.amp ? "a_amp_drive" : "a_filter1_drive";
        oscairn_analog::RenderSpec spec{48, 100, 1, 0.1, rate,
                                        {{"a_lowcut.off", 1}, {"a_osc1_type", 1},
                                         {"a_vca_level", T::ampLevel[431]},
                                         {"a_level_o1", T::osc1Level[511]},
                                         {p, (c.amp ? -40.4 : -42.1) + 0.75 * c.raw},
                                         {p + "_asym", c.amp ? 0.115 : 0.254},
                                         {p + ".off", 0}},
                                        {}};
        if (!c.amp)
            spec.params.insert(spec.params.end(), {{"a_filter1_type", 1},
                                                   {"a_filter1_cutoff", T::cutoffLp24[1004]},
                                                   {"a_filter1_resonance", 0.06}});
        auto x = renderMono(spec);
        size_t a = (size_t)((F::onset(x, rate) + 0.3) * rate), n = (size_t)(0.6 * rate);
        double h1 = F::magnitudeAt(x, a, n, f0, rate);
        for (int h = 2; h <= 6; ++h)
        {
            if (c.ref[h - 2] <= -50)
                continue;
            double got = 20 * std::log10(F::magnitudeAt(x, a, n, h * f0, rate) / h1);
            worst = std::max(worst, std::fabs(got - c.ref[h - 2]));
            check(std::fabs(got - c.ref[h - 2]) <= 3,
                  std::string(c.amp ? "amp" : "filter") + " saturator at raw " +
                      std::to_string(c.raw) + ", harmonic " + std::to_string(h) +
                      ": reference " + std::to_string(c.ref[h - 2]) + " dB, engine " +
                      std::to_string(got));
        }
    }
    std::printf("  saturators: largest difference %.2f dB\n", worst);

    // The filter saturator after every other filter type (batch m10a: a sine at note 60, LPFs at
    // index 1023, HPFs at 0, BPF12 at 537 and BPF6 at 532, raw 48 and 80), each type mapped as
    // in the cutoff tables. The reference uses one curve and asymmetry for every type; the
    // offsets differ by the engine filters' pass-band level at the note against the
    // reference's (one offset, -43.6 dB, for the LPFs, whose pass band is at unity in both).
    struct TypeCase
    {
        const char *name;
        const float *cutoff;
        int type, subtype, index;
        double resonance, offset, ref48[5], ref80[5];
    };
    const TypeCase types[] = {
        {"LPF18", T::cutoffLp18, 3, 2, 1023, 0, -43.60, {-26.4, -34.0, -54.3, -64.3, -82.1}, {-27.1, -10.6, -29.3, -16.7, -32.2}},
        {"LPF12", T::cutoffLp12, 3, 1, 1023, 0, -43.60, {-26.4, -34.0, -54.3, -64.3, -82.1}, {-27.1, -10.6, -29.3, -16.7, -32.2}},
        {"LPF6", T::cutoffLp6, 3, 0, 1023, 0, -43.60, {-26.4, -34.0, -54.3, -64.3, -82.1}, {-27.1, -10.6, -29.3, -16.7, -32.2}},
        {"HPF24", T::cutoffHp24, 5, 0, 0, 0.05, -42.52, {-26.9, -35.2, -56.0, -66.8, -85.2}, {-26.6, -10.7, -29.1, -17.0, -32.1}},
        {"HPF18", T::cutoffHp18, 35, 6, 0, 0, -38.46, {-26.9, -35.0, -55.8, -66.4, -84.6}, {-26.7, -10.7, -29.2, -16.9, -32.1}},
        {"HPF12", T::cutoffHp12, 20, 0, 0, 0, -40.49, {-26.7, -34.6, -55.2, -65.6, -83.6}, {-26.8, -10.7, -29.2, -16.8, -32.1}},
        {"HPF6", T::cutoffHp6, 35, 4, 0, 0, -38.96, {-26.6, -34.4, -54.9, -65.2, -83.2}, {-27.0, -10.6, -29.3, -16.8, -32.1}},
        {"BPF12", T::cutoffBp12, 23, 0, 537, 0, -41.71, {-35.6, -53.3, -82.9, -95.4, -119.4}, {-21.7, -14.9, -29.6, -25.7, -37.5}},
        {"BPF6", T::cutoffBp6, 6, 0, 532, 0, -45.43, {-33.5, -49.0, -76.4, -99.4, -114.7}, {-22.3, -13.5, -28.6, -22.7, -34.9}},
    };
    const double f60 = 440 * std::pow(2.0, -9 / 12.0);
    double worstType = 0;
    for (auto &c : types)
        for (int raw : {48, 80})
        {
            auto x = renderMono({60, 100, 1, 0.1, rate,
                                 {{"a_lowcut.off", 1}, {"a_osc1_type", 1},
                                  {"a_vca_level", T::ampLevel[431]},
                                  {"a_level_o1", T::osc1Level[511]},
                                  {"a_filter1_type", c.type}, {"a_filter1_subtype", c.subtype},
                                  {"a_filter1_resonance", c.resonance},
                                  {"a_filter1_cutoff", c.cutoff[c.index]},
                                  {"a_filter1_drive", c.offset + 0.75 * raw},
                                  {"a_filter1_drive_asym", 0.253},
                                  {"a_filter1_drive.off", 0}},
                                 {}});
            const double *ref = raw == 48 ? c.ref48 : c.ref80;
            size_t a = (size_t)((F::onset(x, rate) + 0.3) * rate), n = (size_t)(0.6 * rate);
            double h1 = F::magnitudeAt(x, a, n, f60, rate);
            for (int h = 2; h <= 6; ++h)
            {
                if (ref[h - 2] <= -50)
                    continue;
                double got = 20 * std::log10(F::magnitudeAt(x, a, n, h * f60, rate) / h1);
                worstType = std::max(worstType, std::fabs(got - ref[h - 2]));
                check(std::fabs(got - ref[h - 2]) <= 3,
                      std::string("filter saturator after ") + c.name + " at raw " +
                          std::to_string(raw) + ", harmonic " + std::to_string(h) +
                          ": reference " + std::to_string(ref[h - 2]) + " dB, engine " +
                          std::to_string(got));
            }
        }
    std::printf("  filter saturator after the other types: largest difference %.2f dB\n",
                worstType);
}

// Noise tone against the reference (batches m8b and m10a: noise alone at Noise Generator Out
// Level 511, tone 0-127 every 8 and 127, note 60), with the converter's mapping: Noise Tilt and
// Noise Tilt Frequency interpolated linearly between the knots below (fitted at those tones,
// tilt off at 64), and a level correction for the noise level. Half-octave bands 62.5 Hz-16 kHz
// over 0.3-2.4 s: the shape (bands minus their mean) within 1 dB RMS of the reference's, and the
// level relative to tone 64, after the correction, within 1 dB (proposed tolerances). Tones 24,
// 40, 56, 72, 88 and 104 are checks between the knots; the others are the fitting tones.
static void testNoiseToneAgainstReference()
{
    namespace F = oscairn_features;
    struct Knot
    {
        int tone;
        double freq, tilt, correction; // Surge units (semitones from 440 Hz), dB, dB
    };
    const Knot knots[] = {
        {0, -14.34, -42.19, 7.46},  {8, -7.24, -19.51, 5.84},   {16, -2.11, -14.50, 5.29},
        {32, 3.22, -8.14, 3.51},    {48, 5.37, -3.79, 2.02},    {64, 8.55, 0.00, 0.00},
        {80, 11.73, 2.54, -1.93},   {96, 26.13, 4.48, -2.78},   {112, 38.44, 7.74, -5.29},
        {120, 44.39, 10.49, -7.91}, {127, 50.10, 16.28, -14.46},
    };
    struct Ref
    {
        int tone;
        double level, shape[17];
    };
    const Ref refs[] = {
        {0, -8.56, {15.57, 15.36, 14.81, 13.56, 11.07, 10.55, 7.45, 4.01, 1.51, -1.15, -4.35, -7.02, -10.32, -13.54, -16.38, -19.27, -21.86}},
        {8, -4.31, {10.33, 10.24, 9.39, 8.99, 7.68, 6.03, 4.40, 2.32, -0.39, -2.52, -4.88, -6.44, -7.53, -8.37, -9.20, -9.71, -10.34}},
        {16, -2.23, {7.19, 7.20, 7.68, 7.16, 6.60, 5.32, 3.40, 1.68, -0.43, -2.17, -3.87, -5.14, -5.91, -6.48, -6.73, -7.45, -8.04}},
        {24, -1.38, {6.04, 5.62, 5.17, 5.61, 4.72, 3.94, 2.82, 0.97, -0.44, -2.06, -2.89, -3.93, -4.22, -4.70, -5.06, -5.48, -6.09}},
        {32, -0.80, {3.94, 4.69, 4.44, 4.03, 3.98, 3.04, 2.31, 1.10, -0.38, -1.67, -2.65, -2.78, -3.29, -3.59, -3.92, -4.37, -4.89}},
        {40, -0.32, {2.34, 3.29, 3.18, 3.14, 2.66, 2.05, 1.46, 0.51, -0.42, -0.96, -1.44, -1.84, -2.04, -2.39, -2.68, -3.19, -3.67}},
        {48, -0.06, {2.41, 1.81, 2.95, 1.90, 1.81, 1.19, 0.83, 0.57, -0.40, -0.60, -0.84, -1.41, -1.46, -1.57, -1.92, -2.33, -2.94}},
        {56, 0.02, {0.66, 1.03, 0.93, 1.27, 0.82, 1.20, 0.60, 0.44, -0.02, -0.17, -0.32, -0.41, -0.62, -0.80, -1.13, -1.50, -1.97}},
        {64, 0.00, {0.13, 1.66, 0.14, 0.33, 0.08, -0.36, -0.05, 0.30, -0.11, 0.24, 0.17, 0.08, 0.03, -0.05, -0.38, -0.88, -1.31}},
        {72, -0.20, {-1.67, -0.22, -0.61, -0.57, -0.11, 0.42, 0.36, -0.08, 0.25, 0.51, 0.67, 0.72, 0.67, 0.43, 0.16, -0.24, -0.70}},
        {80, -0.53, {-1.14, -1.23, -1.14, -1.20, -0.74, -0.69, -0.46, -0.30, 0.21, 0.73, 1.09, 1.14, 1.11, 1.14, 0.88, 0.57, 0.03}},
        {88, -0.63, {-2.56, -0.84, -1.81, -0.89, -1.37, -1.35, -0.61, -0.76, -0.03, 0.51, 1.18, 1.44, 1.77, 1.78, 1.61, 1.27, 0.66}},
        {96, -0.86, {-1.84, -1.41, -2.50, -1.24, -1.65, -1.43, -1.25, -0.74, -0.57, 0.17, 0.89, 1.50, 2.14, 2.34, 2.23, 1.91, 1.46}},
        {104, -1.28, {-1.92, -2.37, -1.86, -1.47, -1.30, -1.71, -1.91, -1.66, -1.01, -0.32, 0.56, 1.61, 2.37, 2.79, 2.98, 2.82, 2.39}},
        {112, -2.65, {-2.41, -3.26, -2.38, -2.34, -2.33, -2.12, -1.78, -1.70, -1.42, -0.88, 0.41, 1.77, 2.65, 3.62, 4.14, 4.10, 3.92}},
        {120, -4.65, {-3.46, -2.38, -2.95, -3.09, -3.30, -3.10, -2.68, -2.44, -1.94, -1.18, 0.15, 1.72, 3.09, 4.56, 5.42, 5.82, 5.77}},
        {127, -9.39, {-4.43, -5.43, -4.45, -5.17, -4.77, -4.20, -4.58, -3.73, -2.61, -1.28, 0.39, 2.52, 4.71, 6.64, 8.15, 8.97, 9.28}},
    };
    auto mapping = [&](int tone) {
        size_t i = 1;
        while (i + 1 < std::size(knots) && knots[i].tone < tone)
            ++i;
        auto &a = knots[i - 1], &b = knots[i];
        double u = std::clamp((tone - a.tone) / double(b.tone - a.tone), 0.0, 1.0);
        return Knot{tone, a.freq + u * (b.freq - a.freq), a.tilt + u * (b.tilt - a.tilt),
                    a.correction + u * (b.correction - a.correction)};
    };
    auto bands = [&](int tone) {
        auto m = mapping(tone);
        auto x = renderMono({60, 100, 2.5, 0.1, 44100,
                             {{"a_lowcut.off", 1}, {"a_mute_o1", 1}, {"a_mute_noise", 0},
                              {"a_noise_tilt", m.tilt}, {"a_noise_tilt_freq", m.freq}},
                             {}});
        return F::halfOctaveBandsDb(x, 44100, 0.3, 2.4);
    };
    auto mean = [](const std::vector<double> &v) {
        double m = 0;
        for (auto d : v)
            m += d / v.size();
        return m;
    };
    double white = mean(bands(64)), worstShape = 0, worstLevel = 0;
    for (auto &r : refs)
    {
        auto b = bands(r.tone);
        double m = mean(b), e = 0;
        for (int k = 0; k < 17; ++k)
            e += std::pow(b[k] - m - r.shape[k], 2) / 17;
        double shape = std::sqrt(e), level = m - white + mapping(r.tone).correction - r.level;
        worstShape = std::max(worstShape, shape);
        worstLevel = std::max(worstLevel, std::fabs(level));
        check(shape <= 1 && std::fabs(level) <= 1,
              "noise tone " + std::to_string(r.tone) + ": shape " + std::to_string(shape) +
                  " dB RMS from the reference, level " + std::to_string(level) + " dB");
    }
    std::printf("  noise tone: largest shape difference %.2f dB RMS, level %.2f dB\n",
                worstShape, worstLevel);
}

// Portamento "Time 2" against the reference (batches m8e and m10b: mono, fingered, Portamento
// Time 64, a sine, legato intervals over an octave): the converter's mapping is the time table
// plus Portamento Octave Limit. A glide over more than an octave jumps to an octave from the
// target and glides the rest: the straight line through the pitch track between 20 and 80 % of
// that last octave, read at the moment the pitch leaves the first note, lands within 0.5
// semitone of where the reference's does, and its slope is within 5 % (proposed tolerances).
static void testPortamentoOctaveLimitAgainstReference()
{
    namespace T = oscairn_tables;
    const double rate = 44100;
    struct Case
    {
        int from, to;
        double jump, slope; // semitones from the target, semitones per second
    };
    const Case cases[] = {{48, 65, -11.92, 36.54}, {48, 72, -11.96, 36.56},
                          {48, 84, -11.87, 36.59}, {84, 60, 11.91, 36.48},
                          {84, 67, 11.89, 36.56}};
    double worstJump = 0, worstSlope = 0;
    for (auto &c : cases)
    {
        oscairn_analog::RenderSpec s{c.from, 100, 1.6, 0.1, rate,
                                     {{"a_lowcut.off", 1}, {"a_osc1_type", 1}, {"a_polymode", 3},
                                      {"a_portamento", T::portamentoTime[64]},
                                      {"a_portamento_octave_limit", 1}},
                                     {}};
        s.legatoNote = c.to;
        s.legatoAt = 1;
        auto x = renderMono(s);
        // pitch in semitones from the first note, one value per period
        const double f = 440 * std::pow(2.0, (c.from - 69) / 12.0);
        std::vector<double> t, k;
        double prev = -1;
        for (size_t i = 0; i + 1 < x.size(); ++i)
            if (x[i] < 0 && x[i + 1] >= 0)
            {
                double z = i + x[i] / (x[i] - x[i + 1]);
                if (prev >= 0)
                {
                    t.push_back((prev + z) / 2 / rate);
                    k.push_back(12 * std::log2(rate / (z - prev) / f));
                }
                prev = z;
            }
        int iv = c.to - c.from;
        double d = iv > 0 ? 1 : -1, tj = -1;
        for (size_t i = 0; i < t.size() && tj < 0; ++i)
            if (t[i] > 0.9 && std::fabs(k[i]) > 0.3)
                tj = t[i];
        double lo = iv - 12 * d + 0.2 * 12 * d, hi = iv - 12 * d + 0.8 * 12 * d;
        double n = 0, st = 0, sk = 0, stt = 0, stk = 0;
        for (size_t i = 0; i < t.size(); ++i)
            if (tj >= 0 && t[i] > tj && d * k[i] >= d * lo && d * k[i] <= d * hi)
            {
                n += 1;
                st += t[i];
                sk += k[i];
                stt += t[i] * t[i];
                stk += t[i] * k[i];
            }
        double slope = n > 2 ? (n * stk - st * sk) / (n * stt - st * st) : 0;
        double jump = n > 2 ? (sk - slope * st) / n + slope * tj - iv : 1e9;
        worstJump = std::max(worstJump, std::fabs(jump - c.jump));
        worstSlope = std::max(worstSlope, std::fabs(std::fabs(slope) / c.slope - 1));
        check(std::fabs(jump - c.jump) <= 0.5 && std::fabs(std::fabs(slope) / c.slope - 1) <= 0.05,
              "portamento octave limit " + std::to_string(c.from) + " to " +
                  std::to_string(c.to) + ": jump to " + std::to_string(jump) +
                  " semitones from the target (reference " + std::to_string(c.jump) +
                  "), slope " + std::to_string(std::fabs(slope)) + " semitones/s (reference " +
                  std::to_string(c.slope) + ")");
    }
    std::printf("  portamento octave limit: largest jump difference %.2f semitones, slope %.1f %%\n",
                worstJump, 100 * worstSlope);
}

template <size_t N, size_t K>
static void checkTable(const char *name, const float (&table)[N],
                       const oscairn_tables::Check (&checks)[K], double tolerance,
                       const std::function<double(float)> &measure,
                       const std::function<double(double, double)> &error,
                       const std::function<bool(const oscairn_tables::Check &)> &applies =
                           [](const oscairn_tables::Check &) { return true; })
{
    for (auto &c : checks)
    {
        if (!applies(c))
            continue;
        double got = measure(table[c.index]), err = error(c.reference, got);
        check(std::fabs(err) <= tolerance, std::string(name) + " at index " +
                                               std::to_string(c.index) + ": reference " +
                                               std::to_string(c.reference) + ", engine " +
                                               std::to_string(got));
    }
}

static void testFittedTables(bool cutoffTables = true)
{
    namespace F = oscairn_features;
    namespace T = oscairn_tables;
    const double rate = checkRate;
    auto db = [](double ref, double got) { return got - ref; };
    checkTable(
        "amp level", T::ampLevel, T::ampLevelChecks, T::ampLevelTolerance,
        [&](float v) {
            auto x = renderMono({60, 100, 1, 0.1, rate, {{"a_vca_level", v}}, {}});
            double t0 = F::onset(x, rate);
            return F::levelDb(x, rate, t0 + 0.3, t0 + 0.8);
        },
        db);
    // Oscillator 1's out level at the amp level table's value for index 431 (the init amp level)
    checkTable(
        "oscillator 1 level", T::osc1Level, T::osc1LevelChecks, T::osc1LevelTolerance,
        [&](float v) {
            auto x = renderMono(
                {60, 100, 1, 0.1, rate, {{"a_vca_level", T::ampLevel[431]}, {"a_level_o1", v}}, {}});
            double t0 = F::onset(x, rate);
            return F::levelDb(x, rate, t0 + 0.3, t0 + 0.8);
        },
        db);
    auto pct = [](double ref, double got) { return 100 * (got / ref - 1); };
    // Envelope times at note 60 with one-period windows; the tolerance has a 5 ms floor.
    auto envSpec = [&](const char *name, float v) {
        double h = 1.6 * std::pow(2.0, v) + 0.5;
        bool rel = std::string(name) == "a_env1_release";
        oscairn_analog::RenderSpec s{60, 100, rel ? 0.4 : h, rel ? h : 0.1, rate, {{name, v}}, {}};
        if (std::string(name) == "a_env1_decay")
            s.params.push_back({"a_env1_sustain", 0});
        return s;
    };
    const double w = 3.822;
    // Sanitizer builds check only envelope times whose render is under 30 s: the longer
    // ones (up to 340 s of audio) took most of the sanitizer run, and the standard builds
    // check them all.
    auto shortEnough = [&](const float *table) {
        return [table](const oscairn_tables::Check &c) {
            return !sanitizerBuild || 1.6 * std::pow(2.0, table[c.index]) + 0.5 < 30;
        };
    };
    auto pctFloor = [](double floor) {
        return [floor](double ref, double got) {
            return std::fabs(got - ref) <= floor ? 0.0 : 100 * (got / ref - 1);
        };
    };
    checkTable(
        "amp EG attack", T::ampAttack, T::ampAttackChecks, T::ampAttackTolerance,
        [&](float v) { return F::attackTime(renderMono(envSpec("a_env1_attack", v)), rate, w); },
        pctFloor(0.005), shortEnough(T::ampAttack));
    checkTable(
        "amp EG decay", T::ampDecay, T::ampDecayChecks, T::ampDecayTolerance,
        [&](float v) { return F::decayTime(renderMono(envSpec("a_env1_decay", v)), rate, w); },
        pctFloor(0.005), shortEnough(T::ampDecay));
    checkTable(
        "amp EG release", T::ampRelease, T::ampReleaseChecks, T::ampReleaseTolerance,
        [&](float v) {
            auto x = renderMono(envSpec("a_env1_release", v));
            return F::releaseTime(x, rate, F::onset(x, rate) + 0.4, 0.1, w);
        },
        pctFloor(0.005), shortEnough(T::ampRelease));
    // Pitch EG decay and release: Surge's filter EG (analog mode) routed to oscillator 1's
    // pitch, a sine at note 60, offset 185.2 cents (the reference's depth raw 309).
    auto pitchSpec = [&](bool rel, float v) {
        double h = 1.5 * std::pow(2.0, v) + 0.5;
        oscairn_analog::RenderSpec s{60, 100, rel ? 0.5 : h, rel ? h : 0.1, rate,
                                     {{"a_lowcut.off", 1}, {"a_osc1_type", 1}, {"a_env2_mode", 1},
                                      {"a_env2_attack", -8}},
                                     {{16, "a_osc1_pitch", 185.2 / 1400}}};
        if (rel)
            s.params.insert(s.params.end(), {{"a_env2_sustain", 1}, {"a_env2_release", v},
                                             {"a_env1_release", 5}});
        else
            s.params.insert(s.params.end(), {{"a_env2_decay", v}, {"a_env2_sustain", 0}});
        return s;
    };
    const double c4 = 440 * std::pow(2.0, -9 / 12.0);
    checkTable(
        "pitch EG decay", T::pitchEgDecay, T::pitchEgDecayChecks, T::pitchEgDecayTolerance,
        [&](float v) { return F::pitchFallTime(renderMono(pitchSpec(false, v)), rate, c4, 185.2, 0); },
        pctFloor(0.005));
    checkTable(
        "pitch EG release", T::pitchEgRelease, T::pitchEgReleaseChecks,
        T::pitchEgReleaseTolerance,
        [&](float v) {
            return F::pitchFallTime(renderMono(pitchSpec(true, v)), rate, c4, 185.2, 0.3);
        },
        pctFloor(0.005));
    // Oscillator EG decay and release: voice LFO 1 as an analog-mode envelope on the Modern
    // pulse's width (0.254 + 0.123), a pulse at note 48 (the reference's PW EG depth +16 at
    // pulse width 64), the duty's fall measured.
    auto oscEgSpec = [&](bool rel, float v) {
        double h = 3.0 * std::pow(2.0, v) + 0.6;
        oscairn_analog::RenderSpec s{48, 100, rel ? 0.6 : h, rel ? h : 0.1, rate,
                                     {{"a_lowcut.off", 1}, {"a_osc1_type", 8}, {"a_osc1_param0", 0},
                                      {"a_osc1_param1", 1}, {"a_osc1_param3", 0.254},
                                      {"a_lfo0_shape", 6}, {"a_lfo0_envmode", 1},
                                      {"a_lfo0_attack", -8}, {"a_env1_release", 5}},
                                     {{17, "a_osc1_param3", 0.123}}};
        if (rel)
            s.params.insert(s.params.end(), {{"a_lfo0_sustain", 1}, {"a_lfo0_decay", -5},
                                             {"a_lfo0_release", v}});
        else
            s.params.insert(s.params.end(), {{"a_lfo0_decay", v}, {"a_lfo0_sustain", 0}});
        return s;
    };
    const double c3 = 440 * std::pow(2.0, -21 / 12.0);
    checkTable(
        "oscillator EG decay", T::oscEgDecay, T::oscEgDecayChecks, T::oscEgDecayTolerance,
        [&](float v) {
            return F::dutyFallTime(renderMono(oscEgSpec(false, v)), rate, c3, 0.254, 0.123, 0);
        },
        pctFloor(0.005));
    checkTable(
        "oscillator EG release", T::oscEgRelease, T::oscEgReleaseChecks,
        T::oscEgReleaseTolerance,
        [&](float v) {
            return F::dutyFallTime(renderMono(oscEgSpec(true, v)), rate, c3, 0.254, 0.123, 0.6);
        },
        pctFloor(0.005));
    // Key-on delay: the onset of a sine whose note-on is at the render's start.
    checkTable(
        "key-on delay", T::keyOnDelay, T::keyOnDelayChecks, T::keyOnDelayTolerance,
        [&](float v) {
            auto x = renderMono({60, 100, std::pow(2.0, v) + 0.3, 0.1, rate,
                                 {{"a_lowcut.off", 1}, {"a_osc1_type", 1}, {"a_keyon_delay", v},
                                  {"a_keyon_delay.off", 0}},
                                 {}});
            return F::onset(x, rate);
        },
        pctFloor(0.015));
    // Portamento: an octave glide (10-90 %) on a sine from note 84, legato, mono with
    // fingered portamento. The engine's glide doesn't depend on the note; the reference's
    // short glides were measured from note 84 and the long ones from 48.
    checkTable(
        "portamento time", T::portamentoTime, T::portamentoTimeChecks,
        T::portamentoTimeTolerance,
        [&](float v) {
            oscairn_analog::RenderSpec s{84, 100, 1.3 + 1.3 * std::pow(2.0, v), 0.1, rate,
                                         {{"a_lowcut.off", 1}, {"a_osc1_type", 1},
                                          {"a_polymode", 3}, {"a_portamento", v}},
                                         {}};
            s.legatoNote = 96;
            s.legatoAt = 1;
            const double c6 = 440 * std::pow(2.0, 15 / 12.0);
            return F::glideTime(renderMono(s), rate, c6, 12, 0.5);
        },
        pctFloor(0.005));
    // The zero-crossing rate here needs several periods and an envelope that follows the
    // modulation: checked from 1 to 40 Hz (the measurement repository checks the whole range).
    checkTable(
        "LFO speed", T::lfoSpeed, T::lfoSpeedChecks, T::lfoSpeedTolerance,
        [&](float v) {
            double hold = std::max(3.0, 6.0 / std::pow(2.0, v));
            auto x = renderMono(
                {60, 100, hold, 0.1, rate, {{"a_lfo0_rate", v}}, {{17, "a_vca_level", 0.3}}});
            double t0 = F::onset(x, rate);
            return F::lfoRate(x, rate, t0 + 0.3, x.size() / rate - 0.15);
        },
        pct, [](const T::Check &c) { return c.reference >= 1 && c.reference <= 40; });

    // Cutoff: the engine cutoff whose response best matches the reference's in shape, from
    // renders at the table's value and half a semitone either side (a parabola through the
    // three shape errors), must be within the tolerance of the table's value.
    std::map<int, std::vector<float>> thru;
    // Sanitizer builds check every 8th index: the renders take most of their time and the
    // standard builds check them all.
    const size_t stride = sanitizerBuild ? 8 : 1;
    // lastElsewhere: the last index checked at rates other than 44.1 kHz; above it the best
    // match is printed, not checked (HPF24: see the plan, eleventh session)
    auto cutoff = [&](const char *name, const auto &table, const auto &responses, double tolerance,
                      int type, int subtype, double resonance, int lastElsewhere = 1023) {
        for (size_t i = 0; i < std::size(responses); i += stride)
        {
            auto &r = responses[i];
            auto spec = [&](std::vector<std::pair<std::string, double>> p) {
                return oscairn_analog::RenderSpec{r.note, 100, 1, 0.1, rate, std::move(p), {}};
            };
            if (!thru.count(r.note))
                thru[r.note] = renderMono(spec({}));
            double v = table[r.index], e[3];
            for (int k = 0; k < 3; ++k)
            {
                auto x = renderMono(spec({{"a_filter1_type", type},
                                          {"a_filter1_subtype", subtype},
                                          {"a_filter1_resonance", resonance},
                                          {"a_filter1_cutoff", v + 0.5 * (k - 1)}}));
                double t0 = F::onset(x, rate);
                e[k] = F::shapeErrorDb(x, thru[r.note], rate,
                                       440 * std::pow(2.0, (r.note - 69) / 12.0), t0 + 0.3,
                                       t0 + 0.9, r.first, r.count, r.gainTenthDb);
            }
            double curv = e[0] - 2 * e[1] + e[2];
            double best = curv > 0 ? 0.25 * (e[0] - e[2]) / curv : 1e9; // semitones from v
            if (rate != 44100 && r.index > lastElsewhere)
            {
                std::printf("  %s cutoff at index %d, not checked at this rate: best match %.0f "
                            "cents from the table\n",
                            name, r.index, 100 * best);
                continue;
            }
            check(std::fabs(100 * best) <= tolerance,
                  std::string(name) + " cutoff at index " + std::to_string(r.index) +
                      ": best match " + std::to_string(100 * best) + " cents from the table");
        }
    };
    if (!cutoffTables)
        return;
    // Each type's Surge filter (type, subtype, resonance) as chosen in the plan.
    cutoff("LPF24", T::cutoffLp24, T::cutoffLp24Responses, T::cutoffLp24Tolerance, 2, 0, 0.06);
    cutoff("LPF18", T::cutoffLp18, T::cutoffLp18Responses, T::cutoffLp18Tolerance, 3, 2, 0);
    cutoff("LPF12", T::cutoffLp12, T::cutoffLp12Responses, T::cutoffLp12Tolerance, 3, 1, 0);
    cutoff("LPF6", T::cutoffLp6, T::cutoffLp6Responses, T::cutoffLp6Tolerance, 3, 0, 0);
    cutoff("HPF24", T::cutoffHp24, T::cutoffHp24Responses, T::cutoffHp24Tolerance, 5, 0, 0.05,
           937);
    cutoff("HPF18", T::cutoffHp18, T::cutoffHp18Responses, T::cutoffHp18Tolerance, 35, 6, 0);
    cutoff("HPF12", T::cutoffHp12, T::cutoffHp12Responses, T::cutoffHp12Tolerance, 20, 0, 0);
    cutoff("HPF6", T::cutoffHp6, T::cutoffHp6Responses, T::cutoffHp6Tolerance, 35, 4, 0);
    cutoff("BPF12", T::cutoffBp12, T::cutoffBp12Responses, T::cutoffBp12Tolerance, 23, 0, 0);
    cutoff("BPF6", T::cutoffBp6, T::cutoffBp6Responses, T::cutoffBp6Tolerance, 6, 0, 0);
}

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : SURGE_FACTORY_PATCHES;
    try
    {
        auto patches = factoryPatches(dir);
        check(!patches.empty(), "factory patches found in " + dir);
        auto run = [](const char *name, auto &&test) {
            std::printf("%s\n", name);
            std::fflush(stdout);
            test();
        };
        run("ports and parameters", [&] { testPortsAndParams(patches); });
        run("state round trips", [&] { testStateRoundTrip(patches); });
        run("block sizes", [&] { testBlockSizes(patches); });
        run("notes", [] { testNotes(); });
        run("formula refused", [] { testFormulaRefused(); });
        run("concurrent state", [] { testConcurrentState(); });
        run("plain patches match stock Surge", [&] { testPlainPatchesMatchStock(patches); });
        run("first block after a load", [] { testFirstBlockAfterLoad(); });
        run("unison against the reference", [] { testUnisonAgainstReference(); });
        run("drift against the reference", [] { testDriftAgainstReference(); });
        run("cutoff drift against the reference", [] { testCutoffDriftAgainstReference(); });
        run("pitch LFO phases against the reference", [] { testPitchLfoPhasesAgainstReference(); });
        run("velocity against the reference", [] { testVelocityAgainstReference(); });
        run("note shift and bend ranges", [] { testNoteShiftAndBendRanges(); });
        run("saturators against the reference", [] { testSaturatorsAgainstReference(); });
        run("noise tone against the reference", [] { testNoiseToneAgainstReference(); });
        run("portamento octave limit against the reference",
            [] { testPortamentoOctaveLimitAgainstReference(); });
        run("fitted tables", [] { testFittedTables(); });
        // The same checks at the plugin's other rates (the reference was measured at 44.1 kHz),
        // in the standard build only: they check numbers, not memory or threads, and would
        // add most of an hour to the TSan run. The cutoff tables hold there through the
        // filter cutoff rate match, but for HPF24 above index 937 (see the plan).
        if (!sanitizerBuild)
        {
            for (double r : {48000.0, 96000.0})
            {
                checkRate = r;
                std::printf("at %g Hz:\n", r);
                run("saturators against the reference", [] { testSaturatorsAgainstReference(); });
                run("fitted tables", [] { testFittedTables(); });
            }
            checkRate = 44100;
        }
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
