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
 *   - oscairn.voice-budget: voices given up past the host's limits, in the SDK's order,
 *     faded without a click, and reported per note,
 *   - CLAP reset: at once, and the instance then plays as a fresh one,
 *   - the fitted tables (FittedTables.h, milestone 1): rendering a table's engine value
 *     gives the feature measured on the reference at that index, within the tolerance.
 *
 * Usage: oscairn-analog-tests [factory patch directory]
 * (OSCAIRN_TEST_ONLY and OSCAIRN_TEST_SKIP select tests: see main.)
 */

#include "OscairnAnalog.h"
#include "SurgeSynthesizer.h"
#include "../tools/RenderNote.h"
#include "Features.h"
#include "ModernOscillatorFastKernels.h"
#include "FittedTables.h"
#include "sst/plugininfra/cpufeatures.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <execinfo.h>
#include <unistd.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include <oscairn-clap/assets.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/ui.h>
#include <oscairn-clap/voice-budget.h>

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
    std::function<void(uint32_t)> beforeProcess; // called with each process() call's frames
    OutEvents out;
    // Output ports given to process() (Main and the sound buses after it), and what render()
    // last rendered on each, interleaved; each is filled with 1 before a call, so a port the
    // plugin doesn't write shows.
    uint32_t outputs{1};
    std::vector<std::vector<float>> port;

    explicit Instance(double rate = 48000, const clap_host_t *h = &host)
    {
        auto factory = static_cast<const clap_plugin_factory_t *>(
            oscairn_analog_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory, h, "org.oscairn.analog");
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
        std::vector<std::vector<float>> bufs(2 * outputs, std::vector<float>(block));
        port.assign(outputs, {});
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
            std::vector<float *> channels(2 * outputs);
            std::vector<clap_audio_buffer_t> buffer(outputs);
            for (uint32_t k = 0; k < outputs; ++k)
            {
                channels[2 * k] = k ? bufs[2 * k].data() : left.data();
                channels[2 * k + 1] = k ? bufs[2 * k + 1].data() : right.data();
                std::fill(channels[2 * k], channels[2 * k] + n, 1.f);
                std::fill(channels[2 * k + 1], channels[2 * k + 1] + n, 1.f);
                buffer[k].data32 = &channels[2 * k];
                buffer[k].channel_count = 2;
            }
            clap_process_t pr{};
            pr.steady_time = (int64_t)position;
            pr.frames_count = n;
            pr.audio_outputs = buffer.data();
            pr.audio_outputs_count = outputs;
            pr.in_events = &in.in;
            pr.out_events = &out.out;
            if (beforeProcess)
                beforeProcess(n);
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
            for (uint32_t k = 0; k < outputs; ++k)
                for (uint32_t i = 0; i < n; ++i)
                {
                    port[k].push_back(channels[2 * k][i]);
                    port[k].push_back(channels[2 * k + 1][i]);
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

// The patch data of a plugin state: after its first line ("oscairn-analog-state-1 <bus>").
static std::vector<uint8_t> patchOfState(const std::vector<uint8_t> &state)
{
    auto end = std::find(state.begin(), state.end(), (uint8_t)'\n');
    return end == state.end() ? std::vector<uint8_t>{} : std::vector<uint8_t>(end + 1, state.end());
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
    bool buses = a.ports->count(a.plugin, false) == 9;
    for (uint32_t k = 0; buses && k < 9; ++k)
        buses = a.ports->get(a.plugin, k, false, &port) && port.channel_count == 2 &&
                port.id == k && (port.flags & CLAP_AUDIO_PORT_IS_MAIN) == (k ? 0u : 1u) &&
                std::string(port.name) == (k ? "Sound bus " + std::to_string(k) : "Main");
    check(buses, "a stereo main output and eight stereo sound buses");
    clap_note_port_info_t np{};
    check(a.notePorts->count(a.plugin, true) == 1 && a.notePorts->get(a.plugin, 0, true, &np) &&
              np.preferred_dialect == CLAP_NOTE_DIALECT_CLAP,
          "one CLAP note input port");

    // oscairn.ui: the roles Oscairn guessed from the names before (declaring any turns its
    // guessing off), but the voice LFOs' Hold; scene A's amp and filter envelopes as views 1 and
    // 2 (each stage its field, index 0); no other parameter has a hint.
    {
        auto ui = static_cast<const oscairn_plugin_ui_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_UI));
        check(ui && ui->param_hint, "oscairn.ui");
        check(a.plugin->get_extension(a.plugin, "oscairn.ui/draft-5") &&
                  !a.plugin->get_extension(a.plugin, "oscairn.ui/draft-3"),
              "oscairn.ui is draft 5, not draft 3");
        check(ui && !ui->algorithm, "no FM operator view: no algorithm call");
        // role, view, field, index
        const std::map<std::string, std::vector<uint32_t>> want{
            {"Scene/Volume", {OSCAIRN_ROLE_LEVEL, 0, 0, 0}},
            {"Scene/Pan", {OSCAIRN_ROLE_PAN, 0, 0, 0}},
            {"Filter 1/Cutoff", {OSCAIRN_ROLE_CUTOFF, 0, 0, 0}},
            {"Filter 1/Resonance", {OSCAIRN_ROLE_RESONANCE, 0, 0, 0}},
            {"Amp Envelope/Attack", {OSCAIRN_ROLE_ATTACK, 1, OSCAIRN_FIELD_ATTACK, 0}},
            {"Amp Envelope/Decay", {OSCAIRN_ROLE_DECAY, 1, OSCAIRN_FIELD_DECAY, 0}},
            {"Amp Envelope/Sustain", {OSCAIRN_ROLE_SUSTAIN, 1, OSCAIRN_FIELD_SUSTAIN, 0}},
            {"Amp Envelope/Release", {OSCAIRN_ROLE_RELEASE, 1, OSCAIRN_FIELD_RELEASE, 0}},
            {"Filter Envelope/Attack", {OSCAIRN_ROLE_FILTER_ATTACK, 2, OSCAIRN_FIELD_ATTACK, 0}},
            {"Filter Envelope/Decay", {OSCAIRN_ROLE_FILTER_DECAY, 2, OSCAIRN_FIELD_DECAY, 0}},
            {"Filter Envelope/Sustain", {OSCAIRN_ROLE_FILTER_SUSTAIN, 2, OSCAIRN_FIELD_SUSTAIN, 0}},
            {"Filter Envelope/Release", {OSCAIRN_ROLE_FILTER_RELEASE, 2, OSCAIRN_FIELD_RELEASE, 0}},
            {"Global/Output Bus", {OSCAIRN_ROLE_OUTPUT_BUS, 0, 0, 0}}};
        std::map<std::string, std::vector<uint32_t>> got;
        for (auto &[name, info] : a.paramsByName())
        {
            oscairn_param_hint_t h{};
            if (ui && ui->param_hint(a.plugin, info.id, &h))
                got[name] = {h.role, h.view, h.field, h.index};
        }
        check(got == want, "the parameters' roles and views: " + std::to_string(got.size()) + " hints");
        auto bus = a.paramsByName()["Global/Output Bus"];
        oscairn_param_hint_t h{};
        char text[64] = "";
        double v = -1;
        check(ui && ui->param_hint(a.plugin, bus.id, &h) && h.widget == OSCAIRN_WIDGET_LIST &&
                  (bus.flags & CLAP_PARAM_IS_STEPPED) && bus.min_value == 0 &&
                  bus.max_value == 8 && bus.default_value == 0 &&
                  a.params->value_to_text(a.plugin, bus.id, 3, text, sizeof(text)) &&
                  std::string(text) == "Sound bus 3" &&
                  a.params->text_to_value(a.plugin, bus.id, "Main", &v) && v == 0 &&
                  !a.params->text_to_value(a.plugin, bus.id, "Sound bus 9", &v),
              "the Output Bus: stepped 0-8, a list, Main and Sound bus 1-8");
    }

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
        auto st = patchOfState(b.save());
        reference.loadRaw(st.data(), (int)st.size(), false);
        check(!reference.storage.getPatch().scene[0].amp_drive.deactivated,
              "the state carries the switched-on amp drive");
    }
}

// oscairn.ui views: the values a host draws a view from (get_value of its parameters) follow a
// drag (a main-thread flush), automation in process (under the allocation guard, with a note
// playing through the envelopes) and a state load.
static void testRichViews()
{
    Instance a;
    auto ui = static_cast<const oscairn_plugin_ui_t *>(
        a.plugin->get_extension(a.plugin, OSCAIRN_EXT_UI));
    std::vector<clap_param_info_t> stages;
    for (auto &[name, info] : a.paramsByName())
    {
        oscairn_param_hint_t h{};
        if (ui && ui->param_hint(a.plugin, info.id, &h) && h.view)
            stages.push_back(info);
    }
    check(stages.size() == 8, "two envelope views of four stages: " + std::to_string(stages.size()));
    auto at = [](const clap_param_info_t &p, double f) {
        return p.min_value + f * (p.max_value - p.min_value);
    };
    auto shows = [&](Instance &x, double f) {
        bool ok = true;
        for (auto &p : stages)
        {
            double v = -1e9;
            ok = ok && x.params->get_value(x.plugin, p.id, &v) &&
                 std::fabs(v - at(p, f)) <= 1e-5 * (p.max_value - p.min_value);
        }
        return ok;
    };
    std::vector<Event> drag;
    for (auto &p : stages)
        drag.push_back(paramEvent(0, p.id, at(p, 0.25)));
    InEvents in;
    for (auto &e : drag)
        in.list.push_back(&e.header);
    a.params->flush(a.plugin, &in.in, &a.out.out);
    check(shows(a, 0.25), "a drag (flush) reaches the views' values");
    std::vector<Event> ev{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8)};
    for (auto &p : stages)
        ev.push_back(paramEvent(256, p.id, at(p, 0.75)));
    ev.push_back(noteEvent(1024, CLAP_EVENT_NOTE_OFF, 1, 60, 0));
    a.render(4096, 256, ev);
    check(shows(a, 0.75), "automation in process reaches the views' values, without allocating");
    Instance b;
    check(b.load(a.save()) && shows(b, 0.75), "a loaded state reaches the views' values");

    // seconds() (draft 5): each attack, decay and release at 33 values across its range is the
    // time Surge displays for it (its value text, "62.5 ms" or "1.00 s"), within the text's
    // rounding; the sustains have none
    int times = 0;
    for (auto &p : stages)
    {
        oscairn_param_hint_t h{};
        ui->param_hint(b.plugin, p.id, &h);
        double sec = -1;
        if (h.field == OSCAIRN_FIELD_SUSTAIN)
        {
            check(!ui->seconds || !ui->seconds(b.plugin, p.id, p.default_value, &sec),
                  "no seconds for a sustain");
            continue;
        }
        ++times;
        for (int k = 0; k <= 32; ++k)
        {
            const double v = at(p, k / 32.0);
            char text[64] = "";
            check(b.params->value_to_text(b.plugin, p.id, v, text, sizeof text),
                  "a stage's value text");
            char *end = nullptr;
            double shown = std::strtod(text, &end);
            while (*end == ' ')
                ++end;
            check(!std::strcmp(end, "ms") || !std::strcmp(end, "s"),
                  std::string("a stage's value text has its unit: ") + text);
            if (!std::strcmp(end, "ms"))
                shown /= 1000;
            check(ui->seconds && ui->seconds(b.plugin, p.id, v, &sec) &&
                      std::fabs(sec - shown) <= 0.005 * shown + 5e-5,
                  std::string(p.module) + "/" + p.name + " at " + std::to_string(v) + ": " +
                      std::to_string(sec) + " s, shown as " + text);
        }
    }
    check(times == 6, "six stage times");
}

// Every step of every int Surge parameter (stepped, or exposed 0..1 as the oscillator controls
// and filter subtypes are): its value text is the text Surge shows for that int value, and the
// text reads back as a value with that text.
static void testSteppedValueTexts()
{
    Instance a;
    HeadlessLayer layer;
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    auto s = std::make_unique<SurgeSynthesizer>(&layer, config);
    std::map<clap_id, clap_param_info_t> infos;
    for (uint32_t i = 0; i < a.params->count(a.plugin); ++i)
    {
        clap_param_info_t info{};
        a.params->get_info(a.plugin, i, &info);
        infos[info.id] = info;
    }
    int steps = 0, wrong = 0, named = 0;
    std::set<std::string> want{"a_env1_attack_shape", "a_env1_decay_shape",
                               "a_env1_release_shape", "a_env1_mode", "a_filter1_type",
                               "a_filter1_subtype", "a_osc1_type"};
    for (auto *p : s->storage.getPatch().param_ptr)
    {
        if (p->valtype != vt_int || p->ctrltype == ct_none)
            continue;
        auto it = infos.find(fnv1a(p->get_storage_name()));
        if (it == infos.end())
            continue;
        auto &info = it->second;
        // stepped over Surge's ints (the bend ranges wider than stock's), or 0..1
        const bool stepped = info.flags & CLAP_PARAM_IS_STEPPED;
        check(stepped ? info.min_value <= p->val_min.i && info.max_value >= p->val_max.i
                      : info.min_value == 0 && info.max_value == 1,
              std::string(p->get_storage_name()) + ": stepped over its ints, or 0..1");
        const int lo = stepped ? (int)info.min_value : p->val_min.i,
                  hi = stepped ? (int)info.max_value : p->val_max.i;
        int wrongHere = 0;
        want.erase(p->get_storage_name());
        ++named;
        for (int k = lo; k <= hi && k <= lo + 512; ++k)
        {
            Parameter copy = *p;
            copy.val.i = k;
            const auto surge = copy.get_display();
            char text[256] = "";
            const double value =
                stepped ? k : Parameter::intScaledToFloat(k, p->val_max.i, p->val_min.i);
            a.params->value_to_text(a.plugin, info.id, value, text, sizeof text);
            ++steps;
            if (surge != text)
            {
                ++wrong;
                if (++wrongHere <= 3)
                    std::fprintf(stderr, "  %s at %d: \"%s\", Surge shows \"%s\"\n",
                                 p->get_storage_name(), k, text, surge.c_str());
                continue;
            }
            double v = -1;
            const bool read = a.params->text_to_value(a.plugin, info.id, text, &v);
            check(read || (!stepped && !p->can_setvalue_from_string()),
                  std::string(p->get_storage_name()) + " at " + std::to_string(k) + ": \"" +
                      text + "\" reads back");
            if (read && v != value)
            {
                // two steps can share a text; then it reads back as one with that text
                char back[256] = "";
                a.params->value_to_text(a.plugin, info.id, v, back, sizeof back);
                check(!std::strcmp(back, text), std::string(p->get_storage_name()) + " at " +
                                                    std::to_string(k) + ": \"" + text +
                                                    "\" reads back as " + std::to_string(v));
            }
        }
    }
    std::printf("  stepped value texts: %d parameters, %d steps, %d wrong\n", named, steps, wrong);
    for (auto &w : want)
        check(false, w + " is checked");
    check(named > 50 && wrong == 0, "every step's value text is Surge's text for it");
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

using ModernParams = std::vector<std::pair<std::string, double>>;

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
    // and one whose output depends on denormals being flushed, as stock Surge XT flushes them
    for (auto &p : patches)
        if (p.find("Plucks/Snap.fxp") != std::string::npos)
            states.emplace_back(p, patchFromFxp(p));
    states.emplace_back("built", built);
    // and Modern oscillator patches as a converted part has them (the option off): a saw, pulse
    // and sine third with unison, drift and pitch modulation, with sync, and as an FM pair
    {
        const ModernParams modern{{"a_osc1_type", 8},     {"a_osc1_param0", 0.8},
                                  {"a_osc1_param1", 0.5}, {"a_osc1_param2", -0.3},
                                  {"a_osc1_param2.deform", 2}, {"a_osc1_param5", 25},
                                  {"a_osc1_param6", 3},   {"a_drift", 0.5}};
        auto plus = [&](ModernParams more) {
            oscairn_analog::RenderSpec spec;
            spec.params = modern;
            spec.params.insert(spec.params.end(), more.begin(), more.end());
            return spec;
        };
        auto swept = plus({{"a_lfo0_rate", 4}});
        swept.mods = {{17, "a_osc1_pitch", 0.6}};
        states.emplace_back("modern unison", oscairn_analog::buildState(plus({})));
        states.emplace_back("modern swept", oscairn_analog::buildState(swept));
        states.emplace_back("modern sync",
                            oscairn_analog::buildState(plus({{"a_osc1_param4", 7}})));
        states.emplace_back("modern FM", oscairn_analog::buildState(plus(
                                             {{"a_osc2_type", 8}, {"a_fm_switch", 1},
                                              {"a_fm_depth", -6}})));
    }
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
            {
                // stock Surge XT's processBlock runs the engine with this guard
                auto fpu = sst::plugininfra::cpufeatures::FPUStateGuard();
                stock->process();
            }
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
        s->storage.modernOscillatorFast = true;
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
                  text.find("filterCutoffRateMatch") != std::string::npos &&
                  text.find("modernOscillatorFast") != std::string::npos,
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
        // A note-off naming a note by its ID with wildcard key and channel ends that note only.
        Instance a;
        a.render(4800, 256,
                 {noteEvent(10, CLAP_EVENT_NOTE_ON, 31, 60, 0.9),
                  noteEvent(20, CLAP_EVENT_NOTE_ON, 32, 64, 0.9)});
        auto off = noteEvent(a.position + 1, CLAP_EVENT_NOTE_OFF, 31, -1, 0);
        off.note.channel = -1;
        auto lr = a.render(48000 * 4, 256, {off});
        float last = 0;
        for (size_t i = lr.size() - 2 * 4800; i < lr.size(); ++i)
            last = std::max(last, std::fabs(lr[i]));
        check(hasNoteEnd(a, 31) && !hasNoteEnd(a, 32) && last > 1e-3f,
              "a note-off by ID with wildcard key and channel ends only that note");
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

// A note-on clears the comb filters' delay lines (64 KiB a voice) only for a comb filter: work
// that doesn't change the output, counted rather than timed.
static void testNoteOnWork()
{
    HeadlessLayer layer;
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    auto synth = std::make_unique<SurgeSynthesizer>(&layer, config);
    auto &s = *synth;
    s.setSamplerate(48000);
    s.audio_processing_active = true;
    auto &f = s.storage.getPatch().scene[0].filterunit[0];
    check(f.type.val.i != sst::filters::fut_comb_pos && f.type.val.i != sst::filters::fut_comb_neg,
          "the init patch's first filter isn't a comb");
    s.playNote(0, 60, 100, 0, -1);
    s.process();
    check(s.voices[0].size() == 1 && !s.voices[0].back()->combDelayCleared,
          "without a comb filter a note leaves the comb delay lines alone");
    f.type.val.i = sst::filters::fut_comb_pos;
    s.playNote(0, 64, 100, 0, -1);
    s.process();
    check(s.voices[0].size() == 2 && s.voices[0].back()->combDelayCleared,
          "a comb filter's note clears its delay lines");
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

// --- the fast Modern oscillator (ModernOscillatorFast.cpp) ---------------------------------

// How far below a's energy the energy of a - b is, in dB (-999 for identical, 999 for silence).
static double belowDb(const std::vector<float> &a, const std::vector<float> &b)
{
    double e = 0, s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        e += (double)(a[i] - b[i]) * (a[i] - b[i]), s += (double)a[i] * a[i];
    if (a.size() != b.size())
        return 999;
    return s <= 0 ? 999 : e == 0 ? -999 : 10 * std::log10(e / s);
}

// The tolerance of the fast path against the code it replaces: the difference at least this
// many dB below the signal.
static constexpr double fastToleranceDb = -100;

// The closed forms against the oscillator's own numerical second difference of the generator
// (ModernOscillator.cpp), in double: the saw and the sine, phases and increments over the range
// of notes and across every corner the stencil can reach (the wrap, the sine's centre).
static void testModernFastKernels()
{
    using namespace Surge::Oscillator::Fast;
    auto sawGen = [](double p) {
        double x = (p - 0.5) * 2;
        return (x * x * x - x) / 6;
    };
    auto sineGen = [](double p) {
        double x = (p - 0.5) * 2, x3 = x * x * x, m = 2.0 * (x < 0) - 1;
        return -(m * x3 * x + 2 * x3 - x) / 3;
    };
    auto second = [](auto gen, double p, double d) {
        double p1 = p - d, p2 = p - 2 * d;
        p1 += (p1 < 0);
        p2 += (p2 < 0);
        return (gen(p) + gen(p2) - 2 * gen(p1)) * 0.25 / (d * d);
    };
    uint64_t state = 12345;
    auto uni = [&](double lo, double hi) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return lo + (hi - lo) * ((state >> 11) * (1.0 / 9007199254740992.0));
    };
    double worstSaw = 0, worstSine = 0;
    for (int i = 0; i < 60000; ++i)
    {
        const bool sineCase = i % 2;
        // the sine's closed form is for increments up to 1/4, the saw's up to 1/2
        float d = (float)std::exp(uni(std::log(1e-5), std::log(sineCase ? 0.24 : 0.5)));
        double p;
        switch (i / 2 % 6)
        {
        case 0: p = uni(0, 1); break;
        case 1: p = uni(0, 3 * d); break;
        case 2: p = 0.5 + uni(-3 * d, 3 * d); break;
        case 3: p = 1 - uni(0, 3 * d); break;
        case 4: p = uni(0, 2 * d); break;
        default: p = uni(0, 1); break;
        }
        p = std::min(std::max(p, 0.0), 0.9999999);
        const float pf = (float)p;
        alignas(16) float P[4] = {pf, pf, pf, pf}, D[4] = {d, d, d, d}, RD[4], out[4];
        for (int k = 0; k < 4; ++k)
            RD[k] = 1.f / D[k];
        const V vp = SIMD_MM(load_ps)(P), vd = SIMD_MM(load_ps)(D), vrd = SIMD_MM(load_ps)(RD);
        SIMD_MM(store_ps)(out, sineCase ? sineClosed(vp, vd, vrd) : sawClosed(vp, vd, vrd));
        const double ref = sineCase ? second(sineGen, pf, d) : second(sawGen, pf, d),
                     err = std::fabs(out[0] - ref);
        (sineCase ? worstSine : worstSaw) = std::max(sineCase ? worstSine : worstSaw, err);
    }
    std::printf("  fast kernels: worst error saw %.2e, sine %.2e\n", worstSaw, worstSine);
    check(worstSaw < 1e-6, "the saw's closed form matches the numerical second difference");
    check(worstSine < 2e-6, "the sine's closed form matches the numerical second difference, "
                            "corners included");
}

static oscairn_analog::RenderSpec modernSpec(int note, double rate, ModernParams params,
                                             bool fast)
{
    oscairn_analog::RenderSpec s;
    s.note = note;
    s.hold = 0.12;
    s.tail = 0.02;
    s.rate = rate;
    s.fastOscillator = fast;
    s.params = {{"a_osc1_type", 8}};
    s.params.insert(s.params.end(), params.begin(), params.end());
    return s;
}

// With the engine option off the oscillator plays as before; on, a sawtooth, pulse and sine
// third component play within fastToleranceDb of that for every wave, unison count, drift,
// pitch and level modulation, pulse width modulation and portamento, at every sample rate and
// from the lowest key to the highest.
static void testModernFastAgainstSlow()
{
    const std::vector<double> rates = sanitizerBuild
                                          ? std::vector<double>{48000, 192000}
                                          : std::vector<double>{44100, 48000, 88200, 96000,
                                                                176400, 192000};
    const std::pair<const char *, ModernParams> waves[] = {
        {"saw", {{"a_osc1_param0", 0.8}, {"a_osc1_param1.off", 1}}},
        {"pulse", {{"a_osc1_param0.off", 1}, {"a_osc1_param1", 0.9}, {"a_osc1_param3", 0.3}}},
        {"saw and pulse",
         {{"a_osc1_param0", 0.8}, {"a_osc1_param1", 0.5}, {"a_osc1_param3", 0.35}}},
        {"saw, pulse and sine",
         {{"a_osc1_param0", 0.8}, {"a_osc1_param1", 0.5}, {"a_osc1_param2", -0.3},
          {"a_osc1_param2.deform", 2}}},
        {"sine", {{"a_osc1_param0.off", 1}, {"a_osc1_param1.off", 1}, {"a_osc1_param2", 0.9},
                  {"a_osc1_param2.deform", 2}}},
    };
    double worst = -999;
    int compared = 0;
    std::map<std::string, double> worstBy; // by wave or feature, and by sample rate
    auto compare = [&](const std::string &what, double rate, int note, ModernParams params,
                       auto &&adjust) {
        auto a = modernSpec(note, rate, params, false), b = modernSpec(note, rate, params, true);
        adjust(a), adjust(b);
        auto slow = oscairn_analog::renderNote(a), fast = oscairn_analog::renderNote(b);
        const double db = belowDb(slow, fast);
        const std::string name = what + " at note " + std::to_string(note) + ", " +
                                 std::to_string((int)rate) + " Hz";
        check(db <= fastToleranceDb,
              "the fast Modern oscillator stays within " + std::to_string((int)fastToleranceDb) +
                  " dB of the slow: " + name + " is at " + std::to_string(db) + " dB");
        check(slow != fast, "the fast path plays: " + name);
        worst = std::max(worst, db);
        for (auto key : {what, std::to_string((int)rate) + " Hz"})
            worstBy[key] = worstBy.count(key) ? std::max(worstBy[key], db) : db;
        ++compared;
    };
    auto same = [](oscairn_analog::RenderSpec &) {};
    for (double rate : rates)
    {
        for (auto &[name, params] : waves)
            for (int note : {0, 36, 72, 127})
                compare(name, rate, note, params, same);
    }
    const ModernParams all = waves[3].second;
    for (double rate : sanitizerBuild ? rates : std::vector<double>{44100, 96000, 192000})
    {
        for (int voices : {2, 3, 4, 5, 7, 16})
        {
            auto p = all;
            p.insert(p.end(), {{"a_osc1_param5", 25}, {"a_osc1_param6", voices}});
            compare("unison " + std::to_string(voices), rate, 48, p, same);
        }
        // the unison detune in Hz rather than cents
        auto hertz = all;
        hertz.insert(hertz.end(), {{"a_osc1_param5.absolute", 1}, {"a_osc1_param5", 1.2},
                                   {"a_osc1_param6", 4}});
        compare("absolute unison detune", rate, 48, hertz, same);
        auto drifting = all;
        drifting.insert(drifting.end(), {{"a_drift", 0.5}, {"a_osc1_param6", 3}});
        compare("drift", rate, 48, drifting, same);
        // a pitch sweep (16 Hz, 0.6 of the range) and a fast one at a high key; the pulse
        // width, the saw's level and the sine's level moved by an LFO; portamento
        auto lfo = all;
        lfo.insert(lfo.end(), {{"a_lfo0_rate", 4}});
        compare("pitch sweep", rate, 60, lfo,
                [](auto &s) { s.mods = {{17, "a_osc1_pitch", 0.6}}; });
        lfo.back() = {"a_lfo0_rate", 6};
        compare("fast pitch sweep", rate, 100, lfo,
                [](auto &s) { s.mods = {{17, "a_osc1_pitch", 1.0}}; });
        // a sweep two octaves up from note 100 through a quarter of the rate the voice runs at
        // (23 kHz at 44.1 kHz), where the sine's closed form ends
        auto high = lfo;
        high.push_back({"a_osc1_octave", 2});
        compare("sweep past a quarter of the rate", rate, 100, high,
                [](auto &s) { s.mods = {{17, "a_osc1_pitch", 1.0}}; });
        compare("pulse width modulation", rate, 52, lfo,
                [](auto &s) { s.mods = {{17, "a_osc1_param3", 0.8}}; });
        compare("level modulation", rate, 52, lfo, [](auto &s) {
            s.mods = {{17, "a_osc1_param0", 0.8}, {17, "a_osc1_param2", 0.9}};
        });
        auto porta = all;
        porta.push_back({"a_portamento", -2});
        compare("portamento", rate, 40, porta, [](auto &s) {
            s.legatoNote = 76;
            s.legatoAt = 0.05;
        });
    }
    std::printf("  fast Modern oscillator: %d renders compared, worst difference %.1f dB\n",
                compared, worst);
    for (auto &[key, db] : worstBy)
        std::printf("    %-24s %.1f dB\n", key.c_str(), db);
}

// What the fast path doesn't play falls back to the slow path whole: with the option on, a
// voice with sync, a sub-octave, a triangle or square third component that sounds, or an
// oscillator in an FM pair (either way round) is bit for bit what it is with the option off.
static void testModernFastFallsBack()
{
    const ModernParams saws{{"a_osc1_param0", 0.8}, {"a_osc1_param1", 0.5}};
    auto with = [&](ModernParams more) {
        auto p = saws;
        p.insert(p.end(), more.begin(), more.end());
        return p;
    };
    const std::pair<const char *, ModernParams> cases[] = {
        {"sync", with({{"a_osc1_param4", 7}})},
        {"sub-octave sine", with({{"a_osc1_param2", 0.5}, {"a_osc1_param2.deform", 1026}})},
        {"sub-octave triangle", with({{"a_osc1_param2", 0.5}, {"a_osc1_param2.deform", 1024}})},
        {"triangle third", with({{"a_osc1_param2", 0.5}, {"a_osc1_param2.deform", 0}})},
        {"square third", with({{"a_osc1_param2", 0.5}, {"a_osc1_param2.deform", 1}})},
        // osc 1 is FM'd by a sine (2 > 1); and osc 2, a Modern oscillator, modulates a sine
        // note 127 two octaves up, 50 kHz: past a quarter of the 96 kHz the voice runs at (and
        // past what the host rate carries, so what is left of the sound is tiny, but not zero)
        {"sine above a quarter of the rate",
         with({{"a_osc1_param2", 0.5}, {"a_osc1_param2.deform", 2}, {"a_osc1_octave", 2}})},
        {"FM carrier", with({{"a_fm_switch", 1}, {"a_fm_depth", -6}, {"a_osc2_type", 1}})},
        {"FM source",
         {{"a_osc1_type", 1}, {"a_osc2_type", 8}, {"a_osc2_param0", 0.8}, {"a_osc2_param1", 0.5},
          {"a_fm_switch", 1}, {"a_fm_depth", -6}}},
        {"FM source, 3 > 2 > 1",
         {{"a_osc1_type", 1}, {"a_osc2_type", 1}, {"a_osc3_type", 8}, {"a_osc3_param0", 0.8},
          {"a_osc3_param1", 0.5}, {"a_fm_switch", 2}, {"a_fm_depth", -6}}},
    };
    for (auto &[name, params] : cases)
        for (int note : {38, 80, 127})
        {
            const bool beyond = std::string(name).rfind("sine above", 0) == 0;
            if (beyond != (note == 127))
                continue;
            oscairn_analog::RenderSpec a, b;
            for (auto *s : {&a, &b})
            {
                s->note = note;
                s->hold = 0.12;
                s->tail = 0.02;
                s->params = {{"a_osc1_type", 8}};
                s->params.insert(s->params.end(), params.begin(), params.end());
            }
            b.fastOscillator = true;
            auto slow = oscairn_analog::renderNote(a), fast = oscairn_analog::renderNote(b);
            double peak = 0;
            for (float x : slow)
                peak = std::max(peak, (double)std::fabs(x));
            check(beyond ? peak > 0 : peak > 0.01, std::string("a sound to compare: ") + name);
            check(slow == fast, std::string("with ") + name + " (note " + std::to_string(note) +
                                    ") the fast option plays the slow path bit for bit");
        }
}

// Parameters changed during a note, which moves a voice between the paths: a triangle third
// component brought in and out, sync turned on and off and modulated, and an FM routing
// switched on. Played through the plugin (with its allocation guard) with and without the
// option: within the tolerance of the slow path throughout, and no step between samples
// larger than the slow path's own.
static void testModernFastMidNote()
{
    const double rate = 48000;
    auto build = [&](bool fast, ModernParams more, bool lfoOnSync, bool sweep) {
        oscairn_analog::RenderSpec s;
        s.fastOscillator = fast;
        s.params = {{"a_osc1_type", 8},      {"a_osc1_param0", 0.8}, {"a_osc1_param1", 0.5},
                    {"a_osc1_param5", 20},   {"a_osc1_param6", 3},   {"a_osc2_type", 1}};
        s.params.insert(s.params.end(), more.begin(), more.end());
        if (lfoOnSync)
        {
            // a sine LFO starting negative: sync (its positive part) comes in and out
            s.params.insert(s.params.end(), {{"a_lfo0_rate", 2}, {"a_lfo0_phase", 0.5}});
            s.mods = {{17, "a_osc1_param4", 0.6}};
        }
        if (sweep)
        {
            // the pitch swept by an LFO (16 Hz, 0.6 of the range) all along
            s.params.push_back({"a_lfo1_rate", 4});
            s.mods.push_back({18, "a_osc1_pitch", 0.6});
        }
        return oscairn_analog::buildState(s);
    };
    struct Case
    {
        const char *name;
        ModernParams more;
        bool lfoOnSync;
        bool sweep;
        std::vector<std::pair<double, std::pair<const char *, double>>> changes; // s, name, value
    };
    const Case cases[] = {
        {"a triangle third brought in and out", {{"a_osc1_param2", 0}}, false, false,
         {{0.1, {"a_osc1_param2", 0.6}}, {0.2, {"a_osc1_param2", 0}}}},
        {"sync turned on and off", {}, false, false,
         {{0.1, {"a_osc1_param4", 7}}, {0.2, {"a_osc1_param4", 0}}}},
        {"sync turned on in a pitch sweep", {}, false, true,
         {{0.1, {"a_osc1_param4", 7}}, {0.2, {"a_osc1_param4", 0}}}},
        {"sync modulated", {}, true, false, {}},
        {"FM switched on and off",
         {{"a_osc2_type", 8}, {"a_osc2_param0", 0.8}, {"a_osc2_param1", 0.5}},
         false, false,
         {{0.1, {"a_fm_switch", 1}}, {0.2, {"a_fm_switch", 0}}}},
    };
    for (auto &c : cases)
    {
        std::vector<std::vector<float>> out;
        for (bool fast : {false, true})
        {
            Instance a(rate);
            check(a.load(build(fast, c.more, c.lfoOnSync, c.sweep)), "the patch loads");
            std::vector<Event> ev{paramEvent(0, fnv1a("a_fm_depth"), -6),
                                  noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 50, 0.8),
                                  noteEvent((uint32_t)(0.3 * rate), CLAP_EVENT_NOTE_OFF, 1, 50, 0)};
            for (auto &[t, change] : c.changes)
                ev.push_back(paramEvent((uint32_t)(t * rate), fnv1a(change.first), change.second));
            out.push_back(a.render((uint32_t)(0.34 * rate), 512, ev));
        }
        auto steps = [](const std::vector<float> &x) {
            float m = 0;
            for (size_t i = 2; i < x.size(); i += 2)
                m = std::max(m, std::fabs(x[i] - x[i - 2]));
            return m;
        };
        const double db = belowDb(out[0], out[1]);
        std::printf("  fast Modern oscillator, %s: %.1f dB, largest step %.4f (slow %.4f)\n",
                    c.name, db, steps(out[1]), steps(out[0]));
        check(db <= fastToleranceDb, std::string("the fast path within the tolerance when ") +
                                         c.name + ": " + std::to_string(db) + " dB");
        check(steps(out[1]) <= steps(out[0]) * 1.001f + 1e-5f,
              std::string("no larger step than the slow path's when ") + c.name);
        check(db > -999, std::string("the fast path played at the start when ") + c.name);
    }
}

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
        {"HPF24", T::cutoffHp24, 5, 2, 0, 0.05, -44.02, {-26.9, -35.2, -56.0, -66.8, -85.2}, {-26.6, -10.7, -29.1, -17.0, -32.1}},
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
        pctFloor(0.005),
        [&](const T::Check &c) { return c.index >= 100 && shortEnough(T::ampDecay)(c); });
    // Below index 100 the references are means of 3-8 repeats at note 96, measured with
    // one-period windows at that note and the scene highpass off (its step response lengthens
    // short envelopes); these averaged checks have a 2 ms floor (Jacob's decision, twelfth
    // session). Attack and release don't pass below 100 and 112 (see the plan).
    checkTable(
        "amp EG decay, averaged", T::ampDecay, T::ampDecayChecks, T::ampDecayTolerance,
        [&](float v) {
            auto s = envSpec("a_env1_decay", v);
            s.note = 96;
            s.params.push_back({"a_lowcut.off", 1});
            return F::decayTime(renderMono(s), rate, 0.956);
        },
        pctFloor(0.002), [](const T::Check &c) { return c.index < 100; });
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
    auto cutoff = [&](const char *name, const auto &table, const auto &responses, double tolerance,
                      int type, int subtype, double resonance) {
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
    cutoff("HPF24", T::cutoffHp24, T::cutoffHp24Responses, T::cutoffHp24Tolerance, 5, 2, 0.05);
    cutoff("HPF18", T::cutoffHp18, T::cutoffHp18Responses, T::cutoffHp18Tolerance, 35, 6, 0);
    cutoff("HPF12", T::cutoffHp12, T::cutoffHp12Responses, T::cutoffHp12Tolerance, 20, 0, 0);
    cutoff("HPF6", T::cutoffHp6, T::cutoffHp6Responses, T::cutoffHp6Tolerance, 35, 4, 0);
    cutoff("BPF12", T::cutoffBp12, T::cutoffBp12Responses, T::cutoffBp12Tolerance, 23, 0, 0);
    cutoff("BPF6", T::cutoffBp6, T::cutoffBp6Responses, T::cutoffBp6Tolerance, 6, 0, 0);
}

// CLAP presets. Discovery: the plugin's provider declares Surge's factory patch folder (found
// as Surge finds its data, here the source tree's through PIPELINE_OVERRIDE_DATA_HOME) for .fxp
// files, and reports each file as one preset named after it, for this plugin. Loading: every
// 8th factory patch loaded with preset-load plays and saves exactly as the same patch loaded
// as the plugin's state; a refused preset is reported to the host and keeps the patch.
struct PresetHost
{
    std::vector<std::string> loaded, errors;
    int rescans{0};
};
static PresetHost presetHost;
static const clap_host_preset_load_t hostPresetLoad{
    [](const clap_host_t *, uint32_t, const char *location, const char *, int32_t,
       const char *msg) {
        presetHost.errors.push_back(std::string(location ? location : "") + ": " + msg);
    },
    [](const clap_host_t *, uint32_t, const char *location, const char *load_key) {
        presetHost.loaded.push_back(std::string(location) + (load_key ? "#key" : ""));
    }};
static const clap_host_params_t hostParams{
    [](const clap_host_t *, clap_param_rescan_flags flags) {
        presetHost.rescans += (flags & CLAP_PARAM_RESCAN_VALUES) != 0;
    },
    [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t *) {}};
static const clap_host_t presetClapHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "oscairn-analog-tests",
    "Oscairn",
    "",
    "0",
    [](const clap_host_t *, const char *id) -> const void * {
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD))
            return &hostPresetLoad;
        if (!std::strcmp(id, CLAP_EXT_PARAMS))
            return &hostParams;
        return nullptr;
    },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

static void testPresets(const std::string &dir)
{
    auto factory = static_cast<const clap_preset_discovery_factory_t *>(
        oscairn_analog_entry.get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    check(factory && factory->count(factory) == 1, "one preset discovery provider");
    if (!factory || factory->count(factory) != 1)
        return;
    check(oscairn_analog_entry.get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == factory,
          "the draft preset discovery factory ID gives the same factory");

    struct Indexer
    {
        std::vector<std::string> extensions;
        std::vector<clap_preset_discovery_location_t> locations;
        std::vector<std::string> paths;
    } ix;
    clap_preset_discovery_indexer_t indexer{
        CLAP_VERSION_INIT, "oscairn-analog-tests", "Oscairn", "", "0", &ix,
        [](const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_filetype_t *t) {
            static_cast<Indexer *>(i->indexer_data)->extensions.push_back(t->file_extension);
            return true;
        },
        [](const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_location_t *l) {
            auto x = static_cast<Indexer *>(i->indexer_data);
            x->locations.push_back(*l);
            x->paths.push_back(l->location ? l->location : "");
            return true;
        },
        [](const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) {
            return true;
        },
        [](const clap_preset_discovery_indexer_t *, const char *) -> const void * {
            return nullptr;
        }};
    // Surge's data folder: the source tree's (the factory patches' parent)
    auto data = fs::path(dir).parent_path().u8string();
    setenv("PIPELINE_OVERRIDE_DATA_HOME", data.c_str(), 1);
    auto provider =
        factory->create(factory, &indexer, factory->get_descriptor(factory, 0)->id);
    bool initialized = provider && provider->init(provider);
    unsetenv("PIPELINE_OVERRIDE_DATA_HOME");
    check(initialized, "the preset provider initializes");
    if (!initialized)
    {
        if (provider)
            provider->destroy(provider);
        return;
    }
    check(ix.extensions == std::vector<std::string>{"fxp"}, "the provider declares .fxp files");
    std::string factoryFolder;
    for (size_t i = 0; i < ix.locations.size(); ++i)
    {
        auto &l = ix.locations[i];
        std::error_code ec;
        if (l.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
            (l.flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) &&
            fs::equivalent(fs::path(ix.paths[i]), fs::path(dir), ec))
            factoryFolder = ix.paths[i];
    }
    check(!factoryFolder.empty(), "the provider declares the factory patch folder");

    // The factory patches through Oscairn's asset library (oscairn.assets draft 3): an indexer
    // whose library_root gives a folder (or nothing); the provider declares the folder's
    // patches_factory, else none; Surge's data override still wins.
    {
        struct Assets
        {
            std::string root;
            bool installed{true}; // false: the root is written, but the answer is false
            std::vector<std::string> asked;
            std::vector<std::string> paths;
            std::vector<uint32_t> flags;
            oscairn_host_assets_t table{};
            const void *owner{nullptr};
            const void *indexer{nullptr};
        } as;
        as.table.resolve = nullptr; // as Oscairn's indexer gives it
        as.table.library_root = [](const void *owner, const char *id, const char *min, char *path,
                                   uint32_t capacity) {
            auto *x = static_cast<const Assets *>(
                static_cast<const clap_preset_discovery_indexer_t *>(owner)->indexer_data);
            auto *a = const_cast<Assets *>(x);
            a->owner = owner;
            a->asked.push_back(std::string(id) + " " + (min ? min : ""));
            if (a->root.size() + 1 > capacity)
                return false;
            std::snprintf(path, capacity, "%s", a->root.c_str());
            return a->installed;
        };
        clap_preset_discovery_indexer_t ai{
            CLAP_VERSION_INIT, "oscairn-analog-tests", "Oscairn", "", "0", &as,
            [](const clap_preset_discovery_indexer_t *, const clap_preset_discovery_filetype_t *) {
                return true;
            },
            [](const clap_preset_discovery_indexer_t *i,
               const clap_preset_discovery_location_t *l) {
                auto x = static_cast<Assets *>(i->indexer_data);
                x->paths.push_back(l->location ? l->location : "");
                x->flags.push_back(l->flags);
                return true;
            },
            [](const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) {
                return true;
            },
            [](const clap_preset_discovery_indexer_t *i, const char *id) -> const void * {
                return !std::strcmp(id, OSCAIRN_EXT_ASSETS)
                           ? &static_cast<Assets *>(i->indexer_data)->table
                           : nullptr;
            }};
        // the factory folder the provider declares ("" for none)
        auto declared = [&](const std::string &root, bool installed = true) {
            as.root = root;
            as.installed = installed;
            as.asked.clear();
            as.paths.clear();
            as.flags.clear();
            auto pr = factory->create(factory, &ai, factory->get_descriptor(factory, 0)->id);
            bool ok = pr && pr->init(pr);
            if (pr)
                pr->destroy(pr);
            std::string f;
            for (size_t i = 0; ok && i < as.paths.size(); ++i)
                if (as.flags[i] & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT)
                    f = as.paths[i];
            return ok ? f : std::string("(init failed)");
        };
        auto root =
            fs::temp_directory_path() / ("oscairn-analog-library-" + std::to_string(getpid()));
        fs::create_directories(root / "patches_factory");
        auto installed = declared(root.u8string());
        check(installed == (root / "patches_factory").u8string() && as.owner == &ai &&
                  as.asked == std::vector<std::string>{"org.oscairn.analog-factory 1.0.0"},
              "with the asset library installed the provider declares its patches_factory: " +
                  installed);
        // not installed: no factory folder (Surge XT's own folders are never looked at)
        std::error_code ec;
        auto missing = declared(root.u8string(), false);
        check(missing.empty() && as.asked.size() == 1,
              "without the asset library the provider declares no factory folder: " + missing);
        // Surge's data override wins over the library
        setenv("SURGE_DATA_HOME", data.c_str(), 1);
        auto overridden = declared(root.u8string());
        unsetenv("SURGE_DATA_HOME");
        check(fs::equivalent(fs::path(overridden), fs::path(dir), ec),
              "Surge's data override wins over the asset library: " + overridden);
        fs::remove_all(root, ec);
    }

    struct Receiver
    {
        std::vector<std::string> names;
        std::vector<std::string> plugins;
        int errors{0};
    };
    auto metadata = [&](const std::string &path, Receiver &r) {
        clap_preset_discovery_metadata_receiver_t m{};
        m.receiver_data = &r;
        m.on_error = [](const clap_preset_discovery_metadata_receiver_t *m, int32_t,
                        const char *) { ++static_cast<Receiver *>(m->receiver_data)->errors; };
        m.begin_preset = [](const clap_preset_discovery_metadata_receiver_t *m, const char *name,
                            const char *load_key) {
            static_cast<Receiver *>(m->receiver_data)
                ->names.push_back(std::string(name ? name : "") + (load_key ? "#key" : ""));
            return true;
        };
        m.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t *m,
                             const clap_universal_plugin_id_t *id) {
            static_cast<Receiver *>(m->receiver_data)
                ->plugins.push_back(std::string(id->abi) + ":" + id->id);
        };
        m.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t *,
                                const char *) {};
        m.set_flags = [](const clap_preset_discovery_metadata_receiver_t *, uint32_t) {};
        m.add_creator = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        m.set_description = [](const clap_preset_discovery_metadata_receiver_t *,
                               const char *) {};
        m.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t *,
                              clap_timestamp, clap_timestamp) {};
        m.add_feature = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        m.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t *, const char *,
                              const char *) {};
        return provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                      path.c_str(), &m);
    };
    // Every .fxp in the declared folder, as a host crawls it: one preset each, named after the
    // file, for this plugin.
    int listed = 0;
    if (!factoryFolder.empty())
    {
        std::vector<std::string> crawled;
        for (auto &e : fs::recursive_directory_iterator(fs::path(factoryFolder)))
            if (e.is_regular_file() && e.path().extension() == ".fxp")
                crawled.push_back(e.path().u8string());
        check(crawled.size() == factoryPatches(dir).size(),
              "the crawl finds every factory patch");
        for (auto &path : crawled)
        {
            Receiver r;
            bool ok = metadata(path, r);
            bool right = ok && r.names == std::vector<std::string>{fs::path(path).stem().u8string()} &&
                         r.plugins == std::vector<std::string>{"clap:org.oscairn.analog"} &&
                         !r.errors;
            check(right, "the provider lists a factory patch: " + path);
            listed += right;
        }
    }
    {
        auto bogus = (fs::temp_directory_path() / "oscairn-analog-tests-not-a-patch.fxp").u8string();
        std::ofstream(bogus, std::ios::binary) << "not a patch";
        Receiver r;
        check(!metadata(bogus, r) && r.names.empty() && r.errors == 1,
              "the provider refuses a file that isn't a Surge patch");
        fs::remove(bogus);
    }
    provider->destroy(provider);

    // preset-load against loading the same patch as state
    int compared = 0;
    auto patches = factoryPatches(dir);
    std::vector<Event> notes{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8),
                             noteEvent(24000, CLAP_EVENT_NOTE_OFF, 1, 60, 0)};
    for (size_t i = 0; i < patches.size(); i += 8)
    {
        auto &path = patches[i];
        presetHost = {};
        Instance a(48000, &presetClapHost), b;
        auto presetLoad = static_cast<const clap_plugin_preset_load_t *>(
            a.plugin->get_extension(a.plugin, CLAP_EXT_PRESET_LOAD));
        check(presetLoad && a.plugin->get_extension(a.plugin, CLAP_EXT_PRESET_LOAD_COMPAT) ==
                                presetLoad,
              "the plugin offers preset-load");
        if (!presetLoad)
            return;
        auto before = a.save();
        bool ok = presetLoad->from_location(a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                            path.c_str(), nullptr);
        bool stateOk = b.load(patchFromFxp(path));
        check(ok == stateOk, "preset-load accepts what a state load accepts: " + path);
        if (!ok)
        {
            check(presetHost.errors.size() == 1 && presetHost.loaded.empty() &&
                      a.save() == before,
                  "a refused preset is reported and keeps the patch: " + path);
            continue;
        }
        check(presetHost.loaded == std::vector<std::string>{path} && presetHost.errors.empty() &&
                  presetHost.rescans == 1,
              "the host is told the preset loaded and to rescan values: " + path);
        check(a.save() == b.save(), "preset-load gives the state a state load gives: " + path);
        check(a.render(48000, 512, notes, false) == b.render(48000, 512, notes, false),
              "preset-load plays as a state load: " + path);
        ++compared;
    }
    check(compared > 50, "presets compared: " + std::to_string(compared));

    // what it refuses
    presetHost = {};
    Instance a(48000, &presetClapHost);
    auto presetLoad = static_cast<const clap_plugin_preset_load_t *>(
        a.plugin->get_extension(a.plugin, CLAP_EXT_PRESET_LOAD));
    auto before = a.save();
    auto missing = (fs::path(dir) / "no such patch.fxp").u8string();
    check(!presetLoad->from_location(a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                     missing.c_str(), nullptr) &&
              !presetLoad->from_location(a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                         patches[0].c_str(), "key") &&
              !presetLoad->from_location(a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
                                         nullptr, "Init Saw") &&
              presetHost.errors.size() == 3 && presetHost.loaded.empty() && a.save() == before,
          "preset-load refuses a missing file, a load key and a plugin location");
    std::printf("  presets: %d factory patches listed, %d loaded and compared\n", listed,
                compared);
}

// --- sound contexts -------------------------------------------------------------------

static size_t heapInUse()
{
#ifdef __APPLE__
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st);
    return st.size_in_use;
#else
    return mallinfo2().uordblks;
#endif
}

struct Sounds
{
    Instance &a;
    const oscairn_plugin_sound_contexts_t *x;
    explicit Sounds(Instance &i)
        : a(i), x(static_cast<const oscairn_plugin_sound_contexts_t *>(
                    i.plugin->get_extension(i.plugin, OSCAIRN_EXT_SOUND_CONTEXTS)))
    {
        if (!x)
            throw std::runtime_error("no sound contexts");
    }
    uint32_t preset(const std::string &path, std::string *why = nullptr)
    {
        char message[256] = "";
        auto id = x->prepare_preset(a.plugin, "preset", CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                    path.c_str(), nullptr, message, sizeof(message));
        if (why)
            *why = message;
        return id;
    }
    std::vector<uint8_t> snapshot()
    {
        std::vector<uint8_t> bytes;
        clap_ostream_t os{&bytes, [](const clap_ostream_t *s, const void *b, uint64_t n) {
                              auto v = static_cast<std::vector<uint8_t> *>(s->ctx);
                              v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
                              return (int64_t)n;
                          }};
        if (!x->state->save(a.plugin, &os))
            throw std::runtime_error("snapshot save failed");
        return bytes;
    }
    bool restore(const std::vector<uint8_t> &bytes)
    {
        struct Reader
        {
            const std::vector<uint8_t> *bytes;
            size_t at;
        } r{&bytes, 0};
        clap_istream_t is{&r, [](const clap_istream_t *s, void *b, uint64_t n) {
                              auto rd = static_cast<Reader *>(s->ctx);
                              n = std::min<uint64_t>({n, 1000, rd->bytes->size() - rd->at});
                              std::memcpy(b, rd->bytes->data() + rd->at, n);
                              rd->at += n;
                              return (int64_t)n;
                          }};
        return x->state->load(a.plugin, &is);
    }
    // oscairn.sound-snapshots: a snapshot's sounds prepared beside the playing ones.
    bool prepareSnapshot(const std::vector<uint8_t> &bytes, uint32_t (&ids)[16],
                         std::string *why = nullptr)
    {
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        if (!snaps)
            throw std::runtime_error("no sound snapshots");
        struct Reader
        {
            const std::vector<uint8_t> *bytes;
            size_t at;
        } r{&bytes, 0};
        clap_istream_t is{&r, [](const clap_istream_t *s, void *b, uint64_t n) {
                              auto rd = static_cast<Reader *>(s->ctx);
                              n = std::min<uint64_t>({n, 1000, rd->bytes->size() - rd->at});
                              std::memcpy(b, rd->bytes->data() + rd->at, n);
                              rd->at += n;
                              return (int64_t)n;
                          }};
        char message[256] = "";
        bool ok = snaps->prepare(a.plugin, &is, ids, message, sizeof(message));
        if (why)
            *why = message;
        return ok;
    }
    uint32_t prepared()
    {
        uint32_t n = 0;
        oscairn_sound_context_info_t info;
        for (uint32_t k = 0; k < x->capacity(a.plugin); ++k)
            n += x->get(a.plugin, k, &info);
        return n;
    }
};

// Native sound contexts: a prepared sound renders exactly as loading its patch does; a note
// held across a switch keeps its sound while new notes take the new one; snapshots, discards,
// capacity and memory.
static void testSoundContexts(const std::vector<std::string> &patches)
{
    std::string path;
    for (auto &p : patches)
        if (p.find("Polysynth") != std::string::npos)
            path = p;
    check(!path.empty(), "a factory patch to prepare");
    if (path.empty())
        return;
    const uint32_t frames = 48000;
    std::vector<Event> notes{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8),
                             noteEvent(100, CLAP_EVENT_NOTE_ON, 2, 64, 0.6),
                             noteEvent(24000, CLAP_EVENT_NOTE_OFF, 1, 60, 0),
                             noteEvent(26000, CLAP_EVENT_NOTE_OFF, 2, 64, 0)};
    {
        Instance a;
        Sounds s(a);
        check(s.x->capacity(a.plugin) == 16, "sixteen sound contexts");
        check(a.plugin->get_extension(a.plugin, "oscairn.sound-snapshots/draft-4") &&
                  !a.plugin->get_extension(a.plugin, "oscairn.sound-snapshots/draft-3"),
              "oscairn.sound-snapshots is draft 4, not draft 3");
        oscairn_sound_context_info_t info{};
        check(s.x->get(a.plugin, 0, &info) && info.id == 0 && info.selected_channels == 0xffff &&
                  std::string(info.name) == "Default",
              "the default sound is selected on every channel");
    }
    // Prepared from a preset and selected on every channel, against the patch loaded as state.
    for (size_t i = 0; i < patches.size(); i += 64)
    {
        Instance a, b;
        Sounds s(a);
        std::string why;
        auto id = s.preset(patches[i], &why);
        bool loaded = b.load(patchFromFxp(patches[i]));
        check((id != CLAP_INVALID_ID) == loaded,
              "prepare_preset accepts what a state load accepts: " + patches[i] + " " + why);
        if (id == CLAP_INVALID_ID)
            continue;
        check(s.x->select(a.plugin, id, -1), "select a prepared sound");
        check(a.render(frames, 512, notes, false) == b.render(frames, 512, notes, false),
              "a prepared sound renders as loading its patch: " + patches[i]);
    }
    // Prepared from parameter values, against the same values sent before the first block.
    {
        Instance a, b;
        Sounds s(a);
        const clap_id pitch = fnv1a("a_osc1_pitch"), level = fnv1a("a_vca_level");
        oscairn_sound_parameter_t set[] = {{pitch, 7}, {level, -6}};
        char message[256] = "";
        auto id = s.x->prepare(a.plugin, "dark", set, 2, nullptr, nullptr, message,
                               sizeof(message));
        check(id != CLAP_INVALID_ID && s.x->select(a.plugin, id, -1),
              std::string("prepare from parameter values: ") + message);
        // the reference: the same values set through the parameters, saved and loaded as state
        {
            Instance c;
            auto e1 = paramEvent(0, pitch, 7), e2 = paramEvent(0, level, -6);
            InEvents in;
            in.list = {&e1.header, &e2.header};
            c.params->flush(c.plugin, &in.in, &c.out.out);
            check(b.load(c.save()), "the values' patch loads");
        }
        check(a.render(frames, 512, notes, false) == b.render(frames, 512, notes, false),
              "a sound prepared from values renders as those values saved and loaded");
        oscairn_sound_parameter_t bad[] = {{pitch, 1e9}};
        check(s.x->prepare(a.plugin, "bad", bad, 1, nullptr, nullptr, message,
                           sizeof(message)) == CLAP_INVALID_ID &&
                  s.x->prepare(a.plugin, "text", nullptr, 0, "patch", nullptr, message,
                               sizeof(message)) == CLAP_INVALID_ID,
              "out-of-range values and patch text are refused");
    }
    // A switch on channel 0 mid-note: the held note stays on the default sound, the next note
    // plays the prepared one. The output is the sum of each alone.
    {
        auto run = [&](bool first, bool second) {
            Instance a;
            Sounds s(a);
            auto id = s.preset(path);
            std::vector<float> out;
            auto add = [&](std::vector<float> v) { out.insert(out.end(), v.begin(), v.end()); };
            add(a.render(9600, 512,
                         first ? std::vector<Event>{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8)}
                               : std::vector<Event>{},
                         false));
            if (second)
                s.x->select(a.plugin, id, 0);
            std::vector<Event> ev;
            if (first)
                ev.push_back(noteEvent(30000, CLAP_EVENT_NOTE_OFF, 1, 60, 0));
            if (second)
            {
                ev.push_back(noteEvent(14400, CLAP_EVENT_NOTE_ON, 2, 67, 0.8));
                ev.push_back(noteEvent(30000, CLAP_EVENT_NOTE_OFF, 2, 67, 0));
            }
            add(a.render(96000 - 9600, 512, ev, false));
            oscairn_sound_context_info_t info{};
            s.x->get(a.plugin, 1, &info);
            return std::make_pair(out, info);
        };
        auto [both, info] = run(true, true);
        auto held = run(true, false).first, next = run(false, true).first;
        bool sum = both.size() == held.size() && both.size() == next.size();
        float nextPeak = 0;
        for (size_t i = 0; sum && i < both.size(); ++i)
        {
            sum = both[i] == held[i] + next[i];
            nextPeak = std::max(nextPeak, std::fabs(next[i]));
        }
        check(sum, "a note held across a switch keeps its sound; the next note takes the new one");
        check(nextPeak > 1e-3f, "the note after the switch sounds");
        check(info.selected_channels == 1, "the prepared sound is selected on channel 0");
    }
    // Snapshots: two sounds, one selected on channel 3, round trip and render the same.
    {
        Instance a, b;
        Sounds s(a), t(b);
        auto id = s.preset(path);
        s.x->select(a.plugin, id, 3); // a snapshot includes the queued selection
        auto snap = s.snapshot();
        check(t.restore(snap), "a snapshot loads");
        check(t.snapshot() == snap, "a snapshot round trips byte for byte");
        std::vector<Event> ev{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8),
                              noteEvent(10, CLAP_EVENT_NOTE_ON, 2, 62, 0.8),
                              noteEvent(24000, CLAP_EVENT_NOTE_OFF, 1, 60, 0),
                              noteEvent(24000, CLAP_EVENT_NOTE_OFF, 2, 62, 0)};
        ev[1].note.channel = 3;
        ev[3].note.channel = 3;
        check(a.render(frames, 512, ev, false) == b.render(frames, 512, ev, false),
              "a loaded snapshot plays as the saved one");
        auto broken = snap;
        broken[broken.size() / 2] = 'x';
        check(!t.restore(broken) && t.snapshot() == snap,
              "a damaged snapshot is refused and changes nothing");
    }
    // oscairn.sound-snapshots: the next Performance's sounds prepared beside the playing ones
    // and selected at the cue. The incoming snapshot: its default sound a factory patch loaded
    // as state, and the preset sound selected on channel 3.
    std::vector<uint8_t> incoming;
    {
        Instance b;
        Sounds t(b);
        check(b.load(patchFromFxp(patches[0])), "the incoming default patch loads");
        t.x->select(b.plugin, t.preset(path), 3);
        incoming = t.snapshot();
    }
    std::vector<Event> snapNotes{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8),
                                 noteEvent(10, CLAP_EVENT_NOTE_ON, 2, 62, 0.8),
                                 noteEvent(24000, CLAP_EVENT_NOTE_OFF, 1, 60, 0),
                                 noteEvent(24000, CLAP_EVENT_NOTE_OFF, 2, 62, 0)};
    snapNotes[1].note.channel = snapNotes[3].note.channel = 3;
    {
        Instance a, c;
        Sounds s(a), u(c);
        uint32_t ids[16];
        auto t0 = std::chrono::steady_clock::now();
        std::string why;
        bool ok = s.prepareSnapshot(incoming, ids, &why);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                      .count();
        std::printf("  sound snapshots: two sounds prepared beside the playing one in %.1f ms\n",
                    ms);
        check(ok && s.prepared() == 3, "a snapshot's two sounds are prepared beside: " + why);
        check(ids[0] != 0 && ids[3] != ids[0] && ids[3] != CLAP_INVALID_ID,
              "each channel gets the new sound it selects in the snapshot");
        for (int16_t ch = 0; ch < 16; ++ch)
            s.x->select(a.plugin, ids[ch], ch);
        check(u.restore(incoming), "the snapshot loads in another instance");
        check(a.render(frames, 512, snapNotes, false) == c.render(frames, 512, snapNotes, false),
              "the prepared snapshot plays as the snapshot loaded");
    }
    // A note held across the switch keeps the outgoing sound; the next note takes the incoming.
    {
        auto run = [&](bool first, bool second) {
            Instance a;
            Sounds s(a);
            std::vector<float> out;
            auto add = [&](std::vector<float> v) { out.insert(out.end(), v.begin(), v.end()); };
            add(a.render(9600, 512,
                         first ? std::vector<Event>{noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8)}
                               : std::vector<Event>{},
                         false));
            if (second)
            {
                uint32_t ids[16];
                s.prepareSnapshot(incoming, ids);
                for (int16_t ch = 0; ch < 16; ++ch)
                    s.x->select(a.plugin, ids[ch], ch);
            }
            std::vector<Event> ev;
            if (first)
                ev.push_back(noteEvent(30000, CLAP_EVENT_NOTE_OFF, 1, 60, 0));
            if (second)
            {
                ev.push_back(noteEvent(14400, CLAP_EVENT_NOTE_ON, 2, 67, 0.8));
                ev.push_back(noteEvent(30000, CLAP_EVENT_NOTE_OFF, 2, 67, 0));
            }
            add(a.render(96000 - 9600, 512, ev, false));
            return out;
        };
        auto both = run(true, true), held = run(true, false), next = run(false, true);
        bool sum = both.size() == held.size() && both.size() == next.size();
        // exactly, but for what the outgoing sound still rings below -120 dB a second after its
        // last voice, when it stops rendering (here under 1e-9)
        for (size_t i = 0; sum && i < both.size(); ++i)
            sum = both[i] == held[i] + next[i] || std::fabs(both[i] - held[i] - next[i]) < 1e-9f;
        check(sum, "a note held across a snapshot switch keeps its sound; the next note takes the "
                   "incoming one");
    }
    // Draft 3's ring-out: with a ring-out buffer (four channels) before every process() call, the
    // outgoing sound (the held note) plays in channels 0-1 and the main output holds only the
    // incoming sound. At a second change (choke_retiring, then the selections) the first change's
    // outgoing sound fades out in channels 2-3 and is silent after the choke time, while the
    // second change's outgoing sound takes channels 0-1.
    struct RingRun
    {
        std::vector<float> main, ring01, ring23;
    };
    auto ringRun = [&](bool first, bool second, bool ringOut, bool again) {
        Instance a;
        Sounds s(a);
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        // the buffer stays valid through the process() call, as the host keeps it
        std::vector<float> rb[4];
        float *ch[4];
        for (int k = 0; k < 4; ++k)
        {
            rb[k].assign(512, 0.f);
            ch[k] = rb[k].data();
        }
        clap_audio_buffer_t b{};
        b.data32 = ch;
        b.channel_count = 4;
        if (ringOut)
            a.beforeProcess = [&](uint32_t) {
                for (auto &v : rb)
                    std::fill(v.begin(), v.end(), 0.f);
                snaps->ring_out(a.plugin, &b);
            };
        RingRun out;
        std::vector<Event> ev;
        if (first)
            ev.push_back(noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8));
        if (second)
            ev.push_back(noteEvent(14400, CLAP_EVENT_NOTE_ON, 2, 67, 0.8));
        // one 480-frame call at a time, so the ring-out buffer can be read after each
        for (uint32_t done = 0; done < 48000; done += 480)
        {
            auto change = [&] {
                uint32_t ids[16];
                s.prepareSnapshot(incoming, ids);
                for (int16_t c = 0; c < 16; ++c)
                    s.x->select(a.plugin, ids[c], c);
            };
            if (second && done == 9600)
                change();
            if (again && done == 30240)
            {
                check(snaps->choke_retiring(a.plugin), "choke_retiring is queued");
                change();
            }
            std::vector<Event> now;
            for (auto e : ev)
                if (e.time >= done && e.time < done + 480)
                    now.push_back(e);
            auto x = a.render(480, 512, now, false);
            out.main.insert(out.main.end(), x.begin(), x.end());
            for (uint32_t k = 0; k < 480; ++k)
            {
                out.ring01.push_back(ringOut ? rb[0][k] : 0);
                out.ring01.push_back(ringOut ? rb[1][k] : 0);
                out.ring23.push_back(ringOut ? rb[2][k] : 0);
                out.ring23.push_back(ringOut ? rb[3][k] : 0);
            }
        }
        return out;
    };
    {
        auto x = ringRun(true, true, true, false);
        auto held = ringRun(true, false, false, false).main,
             next = ringRun(false, true, false, false).main;
        bool ok = x.main.size() == held.size() && held.size() == next.size();
        float ringPeak = 0;
        for (size_t i = 0; ok && i < x.main.size(); ++i)
        {
            const bool before = i < 2 * 9600;
            ok = (before ? x.main[i] == held[i] + next[i] : x.main[i] == next[i]) &&
                 (before ? x.ring01[i] == 0
                         : x.ring01[i] == held[i] || std::fabs(x.ring01[i] - held[i]) < 1e-9f) &&
                 x.ring23[i] == 0;
            ringPeak = std::max(ringPeak, std::fabs(x.ring01[i]));
        }
        check(ok && ringPeak > 1e-3f, "with a ring-out buffer the outgoing sound rings out there and "
                                      "the main output holds the incoming one");
    }
    {
        auto x = ringRun(true, true, true, true), once = ringRun(true, true, true, false);
        const size_t cue = 2 * 30240, fadeEnd = cue + 2 * (144 + 2 * 32); // 3 ms at 48 kHz, two blocks
        bool ok = x.main.size() == once.main.size();
        float fadePeak = 0;
        for (size_t i = 0; ok && i < x.main.size(); ++i)
        {
            if (i < cue)
                ok = x.main[i] == once.main[i] && x.ring01[i] == once.ring01[i] &&
                     x.ring23[i] == 0;
            else
            {
                // the second change's outgoing sound (note 2) takes channels 0-1, as it played
                // on the main output without the second change; the second incoming sounds
                // hold no note
                ok = x.ring01[i] == once.main[i] && x.main[i] == 0 &&
                     (i < fadeEnd || x.ring23[i] == 0);
                fadePeak = std::max(fadePeak, std::fabs(x.ring23[i]));
            }
        }
        check(ok && fadePeak > 1e-3f, "at a second change the first change's tail fades out in "
                                      "channels 2-3 and the second's takes channels 0-1");
    }
    // A sound choke_retiring faded out is cut at once, its voices with it: selected again, a
    // note struck in its first block sounds at full level (Surge's all sound off would wait for
    // the sound's next blocks and cut that note).
    {
        auto play = [&](bool choked) {
            Instance a;
            Sounds s(a);
            auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
                a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
            std::vector<float> rb[4];
            float *ch[4];
            for (int k = 0; k < 4; ++k)
            {
                rb[k].assign(512, 0.f);
                ch[k] = rb[k].data();
            }
            clap_audio_buffer_t b{};
            b.data32 = ch;
            b.channel_count = 4;
            a.beforeProcess = [&](uint32_t) { snaps->ring_out(a.plugin, &b); };
            auto change = [&] {
                uint32_t ids[16];
                s.prepareSnapshot(incoming, ids);
                for (int16_t c = 0; c < 16; ++c)
                    s.x->select(a.plugin, ids[c], c);
            };
            if (choked)
            {
                a.render(9600, 480, {noteEvent(0, CLAP_EVENT_NOTE_ON, 1, 60, 0.8)}, false);
                change(); // the default sound, holding its note, rings out
                a.render(9600, 480, {}, false);
                check(snaps->choke_retiring(a.plugin), "choke_retiring is queued");
                change(); // and is choked: faded out over 3 ms, then cut
                a.render(9600, 480, {}, false);
            }
            s.x->select(a.plugin, 0, 0);
            auto x = a.render(24000, 480, {noteEvent(a.position, CLAP_EVENT_NOTE_ON, 5, 64, 0.8)},
                              false);
            double e = 0;
            for (size_t i = 2 * 4800; i < x.size(); ++i)
                e += (double)x[i] * x[i];
            return std::sqrt(e / (x.size() - 2 * 4800));
        };
        const double x = play(true), y = play(false);
        check(y > 1e-3 && x >= 0.99 * y,
              "a sound choked out and selected again plays a note at full level: " +
                  std::to_string(x) + " against " + std::to_string(y));
    }
    // Draft 4: one sound saved on its own is a snapshot holding only it, as its default on every
    // channel, playing as an instance with its patch; a sound's values while no channel selects
    // it are what an instance with its patch shows.
    {
        Instance a;
        Sounds s(a);
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        check(snaps && snaps->save_sound && snaps->sound_values, "draft 4's two calls");
        if (!snaps || !snaps->save_sound || !snaps->sound_values)
            return;
        check(a.load(patchFromFxp(patches[0])), "a default patch loads");
        auto id = s.preset(path);
        auto unselected = s.preset(patches[64]);
        s.x->select(a.plugin, id, 3);
        a.render(512, 512, {}, false); // the selection applied
        auto saveSound = [&](uint32_t sound, std::vector<uint8_t> &bytes) {
            clap_ostream_t os{&bytes, [](const clap_ostream_t *st, const void *b, uint64_t n) {
                                  auto v = static_cast<std::vector<uint8_t> *>(st->ctx);
                                  v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
                                  return (int64_t)n;
                              }};
            return snaps->save_sound(a.plugin, sound, &os);
        };
        std::vector<uint8_t> none;
        check(!saveSound(12345, none), "an unknown sound isn't saved");
        for (auto [sound, patch] : {std::pair{id, path}, std::pair{0u, patches[0]}})
        {
            std::vector<uint8_t> alone;
            check(saveSound(sound, alone), "a sound saved on its own");
            Instance b, c;
            Sounds t(b);
            check(t.restore(alone), "the sound saved on its own loads as a snapshot");
            oscairn_sound_context_info_t info{};
            check(t.prepared() == 1 && t.x->get(b.plugin, 0, &info) &&
                      info.selected_channels == 0xffff,
                  "it holds only that sound, selected on every channel");
            c.load(patchFromFxp(patch));
            auto x = b.render(frames, 512, notes, false), y = c.render(frames, 512, notes, false);
            check(x == y && x != std::vector<float>(x.size(), 0.f),
                  "it plays as an instance with the sound's patch");
        }
        // values: every parameter's, as an instance with that patch shows them
        Instance d;
        d.load(patchFromFxp(patches[64]));
        const uint32_t n = d.params->count(d.plugin);
        std::vector<oscairn_sound_parameter_t> v(n + 8);
        const auto got = snaps->sound_values(a.plugin, unselected, v.data(), (uint32_t)v.size());
        bool same = got == n;
        for (uint32_t i = 0; same && i < n; ++i)
        {
            clap_param_info_t info;
            double want = 0;
            d.params->get_info(d.plugin, i, &info);
            d.params->get_value(d.plugin, info.id, &want);
            same = v[i].id == info.id && v[i].value == want;
        }
        check(same, "an unselected sound's values are its patch's: " + std::to_string(got) +
                        " of " + std::to_string(n));
        check(snaps->sound_values(a.plugin, id, v.data(), (uint32_t)v.size()) == 0 &&
                  snaps->sound_values(a.plugin, 12345, v.data(), (uint32_t)v.size()) == 0,
              "no values for a selected or an unknown sound");
    }
    // Too few free contexts: refused, nothing added.
    {
        Instance a;
        Sounds s(a);
        for (int i = 0; i < 14; ++i)
            s.preset(path);
        uint32_t ids[16];
        std::string why;
        check(s.prepared() == 15 && !s.prepareSnapshot(incoming, ids, &why) && !why.empty() &&
                  s.prepared() == 15,
              "a snapshot needing more contexts than are free is refused, nothing added: " + why);
    }
    // Discard, capacity and refused presets.
    {
        Instance a;
        Sounds s(a);
        auto id = s.preset(path);
        s.x->select(a.plugin, id, 5);
        a.render(512, 512);
        check(!s.x->discard(a.plugin, id), "a selected sound can't be discarded");
        s.x->select(a.plugin, 0, 5);
        a.render(512, 512);
        check(s.x->discard(a.plugin, id) && !s.x->select(a.plugin, id, 0),
              "an unselected sound is discarded and its ID stays invalid");
        // a sound still sounding a note stays until its release has ended
        auto tail = s.preset(path);
        s.x->select(a.plugin, tail, 5);
        auto on = noteEvent(a.position, CLAP_EVENT_NOTE_ON, 40, 60, 0.8);
        on.note.channel = 5;
        a.render(4800, 512, {on});
        s.x->select(a.plugin, 0, 5);
        auto off = noteEvent(a.position, CLAP_EVENT_NOTE_OFF, 40, 60, 0);
        off.note.channel = 5;
        a.render(512, 512, {off});
        check(!s.x->discard(a.plugin, tail), "a sound releasing a note can't be discarded");
        a.render(48000 * 4, 512);
        check(hasNoteEnd(a, 40) && s.x->discard(a.plugin, tail),
              "once its note has ended the sound is discarded");
        uint32_t made = 0;
        while (s.preset(path) != CLAP_INVALID_ID && made < 20)
            ++made;
        std::string why;
        check(made == 15 && s.preset(path, &why) == CLAP_INVALID_ID && !why.empty(),
              "fifteen sounds beside the default, then a refusal saying why: " +
                  std::to_string(made));
        std::string formula;
        for (auto &p : patches)
            if (p.find("Formula Modulator") != std::string::npos)
                formula = p;
        Instance b;
        Sounds t(b);
        check(t.preset(formula, &why) == CLAP_INVALID_ID && !why.empty(),
              "a preset the plugin refuses isn't prepared: " + why);
    }
    // Preparing, selecting and discarding on the main thread while the audio thread plays
    // notes on four channels (what TSan checks); every prepared sound is discarded in the end.
    {
        Instance a;
        Sounds s(a);
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            uint32_t t = 0;
            while (!stop.load())
            {
                auto on = noteEvent(a.position + 3, CLAP_EVENT_NOTE_ON, (int32_t)t, 60, 0.8);
                auto off = noteEvent(a.position + 300, CLAP_EVENT_NOTE_OFF, (int32_t)t, 60, 0);
                on.note.channel = off.note.channel = (int16_t)(t % 4);
                a.render(512, 128, {on, off}, false);
                ++t;
            }
        });
        std::vector<uint32_t> ids;
        const oscairn_sound_parameter_t pitch[] = {{fnv1a("a_osc1_pitch"), 3}};
        for (int i = 0; i < 24; ++i)
        {
            char message[128];
            auto id = s.x->prepare(a.plugin, "x", pitch, 1, nullptr, nullptr, message,
                                   sizeof(message));
            if (id != CLAP_INVALID_ID)
            {
                s.x->select(a.plugin, id, (int16_t)(i % 4));
                ids.push_back(id);
            }
            oscairn_sound_context_info_t info;
            for (uint32_t k = 0; k < 16; ++k)
                s.x->get(a.plugin, k, &info);
            if (i % 6 == 5)
            {
                uint32_t snapIds[16];
                if (s.prepareSnapshot(incoming, snapIds))
                    for (auto d : {snapIds[0], snapIds[3]})
                        ids.push_back(d);
            }
            s.x->select(a.plugin, 0, (int16_t)((i + 2) % 4));
            ids.erase(std::remove_if(ids.begin(), ids.end(),
                                     [&](uint32_t d) { return s.x->discard(a.plugin, d); }),
                      ids.end());
        }
        for (int ch = 0; ch < 4; ++ch)
            s.x->select(a.plugin, 0, (int16_t)ch);
        for (int tries = 0; tries < 2000 && !ids.empty(); ++tries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ids.erase(std::remove_if(ids.begin(), ids.end(),
                                     [&](uint32_t d) { return s.x->discard(a.plugin, d); }),
                      ids.end());
        }
        stop = true;
        audio.join();
        check(ids.empty(), "every sound prepared while playing is discarded once unused");
    }
    // A wildcard note-off (every key on every channel) releases the keys each sound holds, not
    // every key of every channel (16384 releases over eight sounds, 5 ms in a 64-frame block).
    // Standard build only: a timing check, the best of five.
    if (!sanitizerBuild)
    {
        Instance a;
        Sounds s(a);
        for (int ch = 1; ch < 8; ++ch)
            check(s.x->select(a.plugin, s.preset(path), (int16_t)ch),
                  "a sound on channel " + std::to_string(ch));
        double best = 1e9;
        for (int round = 0; round < 5; ++round)
        {
            std::vector<Event> on;
            for (int ch = 0; ch < 8; ++ch)
            {
                on.push_back(noteEvent(a.position, CLAP_EVENT_NOTE_ON, 100 + ch, 60 + ch, 0.7));
                on.back().note.channel = (int16_t)ch;
            }
            a.render(4096, 4096, on);
            auto off = noteEvent(a.position, CLAP_EVENT_NOTE_OFF, -1, -1, 0);
            off.note.channel = -1;
            auto t0 = std::chrono::steady_clock::now();
            a.render(64, 64, {off});
            best = std::min(best, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
        }
        std::printf("  sound contexts: a wildcard note-off over eight sounds, best of five: "
                    "%.3f ms\n",
                    best);
        check(best < 2, "a wildcard note-off over eight sounds takes under 2 ms");
    }
    // oscairn.memory: what a sound is declared to take covers what preparing one allocates.
    for (double rate : {44100.0, 96000.0, 192000.0})
    {
        if (sanitizerBuild)
            break;
        Instance a(rate);
        Sounds s(a);
        auto mem = static_cast<const oscairn_plugin_memory_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_MEMORY));
        oscairn_memory_t before{}, after{};
        check(mem && mem->current(a.plugin, &before), "oscairn.memory");
        // The heap's in-use count moves by a few MiB between single preparations (seen: 28.7
        // to 34.3 MiB), so the median of three.
        std::vector<double> deltas;
        for (int k = 0; k < 3; ++k)
        {
            auto heap = heapInUse();
            s.preset(path);
            deltas.push_back((double)heapInUse() - (double)heap);
            if (k == 0)
                mem->current(a.plugin, &after);
        }
        std::sort(deltas.begin(), deltas.end());
        auto used = deltas[1];
        std::printf("  sound contexts: preparing a sound at %g Hz allocates %.2f MiB (%.2f-%.2f), "
                    "declared %.2f MiB\n",
                    rate, used / 1048576.0, deltas[0] / 1048576.0, deltas[2] / 1048576.0,
                    before.context_bytes / 1048576.0);
        check(before.context_bytes >= used && before.context_bytes < used * 1.25,
              "a sound's declared memory covers what it allocates, within 25 %");
        check(after.shared_bytes == before.shared_bytes + before.context_bytes,
              "a prepared sound adds a sound's memory");
    }
    // The sounds share one store of Surge's voices (128), and each takes as many from it as
    // Surge's own pool holds (64): a sound with a Polyphony Limit of 64 and the most unison voices
    // holds 64 notes, releases them and strikes 64 more, exactly as in stock Surge (which runs out
    // of voices for them), with another sound holding 64 voices beside it.
    {
        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        std::vector<uint8_t> wide;
        {
            auto s = std::make_unique<SurgeSynthesizer>(&layer, config);
            s->storage.getPatch().polylimit.val.i = 64;
            auto &u = s->storage.getPatch().scene[0].unison_voices;
            u.val.i = u.val_max.i;
            void *data = nullptr;
            auto size = s->saveRaw(&data);
            wide.assign((uint8_t *)data, (uint8_t *)data + size);
        }
        auto notes = [](int16_t ch, int32_t id, bool again) {
            std::vector<Event> ev;
            for (int16_t k = 0; k < 64; ++k)
            {
                ev.push_back(noteEvent(0, CLAP_EVENT_NOTE_ON, id + k, 30 + k, 0.8));
                ev.back().note.channel = ch;
            }
            if (again)
            {
                for (int16_t k = 0; k < 64; ++k)
                {
                    ev.push_back(noteEvent(9600, CLAP_EVENT_NOTE_OFF, id + k, 30 + k, 0));
                    ev.back().note.channel = ch;
                }
                for (int16_t k = 0; k < 64; ++k)
                {
                    ev.push_back(noteEvent(9600, CLAP_EVENT_NOTE_ON, id + 100 + k, 30 + k, 0.8));
                    ev.back().note.channel = 2;
                }
            }
            return ev;
        };
        const uint32_t n = 19200;
        Instance a, b; // b: the other sound alone
        check(a.load(wide), "a patch with a Polyphony Limit of 64 loads");
        Sounds s(a), t(b);
        const oscairn_sound_parameter_t limit[] = {{fnv1a("polylimit"), 64}};
        char message[128];
        auto other = s.x->prepare(a.plugin, "other", limit, 1, nullptr, nullptr, message,
                                  sizeof(message));
        check(s.x->select(a.plugin, other, 1), "a second sound on channel 1");
        t.x->select(b.plugin,
                    t.x->prepare(b.plugin, "other", limit, 1, nullptr, nullptr, message,
                                 sizeof(message)),
                    1);
        auto ev = notes(0, 1000, true), held = notes(1, 1, false);
        ev.insert(ev.end(), held.begin(), held.end());
        auto both = a.render(n, 512, ev, false);
        auto beside = b.render(n, 512, held, false);

        auto stock = std::make_unique<SurgeSynthesizer>(&layer, config);
        stock->setSamplerate(48000.f);
        stock->seedRandom(0x05CA1A4E); // the plugin's render seed
        stock->loadRaw(wide.data(), (int)wide.size(), false);
        auto &sp = stock->storage.getPatch();
        sp.scenemode.val.i = sm_single;
        sp.scene_active.val.i = 0;
        sp.fx_disable.val.i = (1 << n_fx_slots) - 1;
        sp.copy_globaldata(sp.globaldata);
        sp.copy_scenedata(sp.scenedata[0], sp.scenedataOrig[0], 0);
        stock->audio_processing_active = true;
        bool same = both.size() == 2 * n && beside.size() == 2 * n;
        for (uint32_t blk = 0; same && blk * BLOCK_SIZE < n; ++blk)
        {
            for (int k = 0; k < 64; ++k)
            {
                if (blk == 0)
                    stock->playNote(0, 30 + k, 102, 0, 1000 + k);
                if (blk * BLOCK_SIZE == 9600)
                    stock->releaseNote(0, 30 + k, 0, 1000 + k);
            }
            if (blk * BLOCK_SIZE == 9600)
                for (int k = 0; k < 64; ++k)
                    stock->playNote(2, 30 + k, 102, 0, 1100 + k);
            stock->process();
            stock->time_data.ppqPos += (double)BLOCK_SIZE * stock->time_data.tempo / (60. * 48000);
            for (int k = 0; same && k < BLOCK_SIZE; ++k)
                for (int c = 0; c < 2; ++c)
                {
                    const size_t i = 2 * (blk * BLOCK_SIZE + k) + c;
                    same = both[i] == 0.f + stock->output[c][k] + beside[i];
                }
        }
        check(same, "beside another sound's 64 voices, a sound takes 64 voices as stock Surge");
    }
}

// oscairn.voice-budget, called as the host calls it (audio thread, between blocks, so under the
// allocation guard).
struct Budget
{
    Instance &a;
    const oscairn_plugin_voice_budget_t *x;
    explicit Budget(Instance &i)
        : a(i), x(static_cast<const oscairn_plugin_voice_budget_t *>(
                    i.plugin->get_extension(i.plugin, OSCAIRN_EXT_VOICE_BUDGET)))
    {
        if (!x)
            throw std::runtime_error("no voice budget");
    }
    // The instance's limit and, as the host sends today, one entry for the selected sounds
    void set(uint32_t limit, bool entry = true)
    {
        oscairn_voice_budget_context_t selected{OSCAIRN_VOICE_BUDGET_SELECTED, 0, 0, 5, 0};
        forbidAllocation = true;
        x->set_limits(a.plugin, limit, entry ? &selected : nullptr, entry ? 1 : 0);
        forbidAllocation = false;
    }
    uint32_t active()
    {
        double cost = -1;
        forbidAllocation = true;
        auto n = x->active_voices(a.plugin, &cost);
        forbidAllocation = false;
        return n;
    }
    std::map<int32_t, oscairn_voice_budget_note_t> notes()
    {
        oscairn_voice_budget_note_t buf[64];
        forbidAllocation = true;
        auto n = x->notes(a.plugin, buf, 64);
        forbidAllocation = false;
        std::map<int32_t, oscairn_voice_budget_note_t> r;
        for (uint32_t i = 0; i < n; ++i)
            r[buf[i].note_id] = buf[i];
        return r;
    }
};

using ParamList = std::vector<std::pair<const char *, double>>;

static void setParams(Instance &a, const ParamList &ps)
{
    std::vector<Event> ev;
    for (auto &[name, v] : ps)
        ev.push_back(paramEvent(0, fnv1a(name), v));
    InEvents in;
    for (auto &e : ev)
        in.list.push_back(&e.header);
    a.params->flush(a.plugin, &in.in, &a.out.out);
}

static Event noteOn(uint32_t t, int32_t id, int16_t channel, int16_t key, double vel)
{
    auto e = noteEvent(t, CLAP_EVENT_NOTE_ON, id, key, vel);
    e.note.channel = channel;
    return e;
}

// Mono modes: a key struck again while another holds the mono voice. In Mono ST, H (70) taken
// legato over K (63), K released, K struck again (legato: its ID ends at once), then H and K
// released: the voice ends. Before the fix K stayed down in Surge, held there by K's first
// note, which had been released but never ended (the voice had moved to H). The same in each
// mono mode, with note IDs, as raw MIDI and under the pedal; and every kind of panic with keys down leaves
// silence.
static void testMonoKeyStruckAgain()
{
    auto peakFrom = [](const std::vector<float> &lr, size_t from) {
        float p = 0;
        for (size_t i = 2 * from; i < lr.size(); ++i)
            p = std::max(p, std::fabs(lr[i]));
        return p;
    };
    auto midiNote = [](uint32_t t, bool on, int16_t key) {
        return midiEvent(t, on ? 0x90 : 0x80, (uint8_t)key, on ? 100 : 64);
    };
    const std::pair<int, const char *> modes[] = {
        {pm_mono, "Mono"}, {pm_mono_st, "Mono ST"}, {pm_mono_fp, "Mono FP"},
        {pm_mono_st_fp, "Mono ST+FP"}};
    for (auto [mode, name] : modes)
        for (int way = 0; way < 3; ++way) // note IDs, raw MIDI, note IDs under the pedal
        {
            const bool midi = way == 1, pedal = way == 2;
            Instance a;
            clap_id playMode = CLAP_INVALID_ID;
            for (auto &[n, info] : a.paramsByName())
                if (n.size() >= 10 && n.compare(n.size() - 10, 10, "/Play Mode") == 0)
                    playMode = info.id;
            check(playMode != CLAP_INVALID_ID, "a Play Mode parameter");
            auto on = [&](uint32_t t, int32_t id, int16_t key) {
                return midi ? midiNote(t, true, key) : noteEvent(t, CLAP_EVENT_NOTE_ON, id, key, 0.8);
            };
            auto off = [&](uint32_t t, int32_t id, int16_t key) {
                return midi ? midiNote(t, false, key) : noteEvent(t, CLAP_EVENT_NOTE_OFF, id, key, 0.5);
            };
            std::vector<Event> ev{paramEvent(0, playMode, mode), on(100, 1, 63), on(4800, 3, 70),
                                  off(9600, 1, 63),  on(14400, 5, 63), off(19200, 3, 70),
                                  off(24000, 5, 63)};
            uint32_t end = 24000; // the last release
            if (pedal)
            {
                ev.push_back(midiEvent(50, 0xB0, 64, 127));
                ev.push_back(midiEvent(end = 36000, 0xB0, 64, 0));
            }
            auto lr = a.render(end + 96000, 256, ev);
            const float after = peakFrom(lr, end + 48000);
            check(peakFrom(lr, 14400) > 0.01f && after < 1e-5f,
                  std::string(name) + (midi ? " (MIDI)" : pedal ? " (pedal)" : "") +
                      ": a key struck again under another ends when both are released: peak " +
                      std::to_string(after));
        }
    // Panic in Mono ST after the six events (the note stuck before the fix): a reset, MIDI's
    // all sound off, and wildcard note-offs and chokes each leave silence, and the next note
    // plays and ends.
    for (int panic = 0; panic < 4; ++panic)
    {
        Instance a;
        clap_id playMode = CLAP_INVALID_ID;
        for (auto &[n, info] : a.paramsByName())
            if (n.size() >= 10 && n.compare(n.size() - 10, 10, "/Play Mode") == 0)
                playMode = info.id;
        a.render(26000, 256,
                 {paramEvent(0, playMode, pm_mono_st),
                  noteEvent(100, CLAP_EVENT_NOTE_ON, 1, 63, 0.8),
                  noteEvent(4800, CLAP_EVENT_NOTE_ON, 3, 70, 0.8),
                  noteEvent(9600, CLAP_EVENT_NOTE_OFF, 1, 63, 0.5),
                  noteEvent(14400, CLAP_EVENT_NOTE_ON, 5, 63, 0.8),
                  noteEvent(19200, CLAP_EVENT_NOTE_OFF, 3, 70, 0.5),
                  noteEvent(23900, CLAP_EVENT_NOTE_OFF, 5, 63, 0.5)});
        std::vector<Event> ev;
        const char *what[] = {"a reset", "all sound off", "a wildcard note-off",
                              "a wildcard choke"};
        if (panic == 0)
            a.plugin->reset(a.plugin);
        else if (panic == 1)
            ev.push_back(midiEvent(a.position, 0xB0, 120, 0));
        else
            ev.push_back(noteEvent(a.position,
                                   panic == 2 ? CLAP_EVENT_NOTE_OFF : CLAP_EVENT_NOTE_CHOKE, -1,
                                   -1, 0));
        if (panic >= 2)
            ev.back().note.channel = -1;
        auto quiet = a.render(48000, 256, ev);
        auto lr = a.render(24000 + 72000, 256,
                           {noteEvent(a.position + 100, CLAP_EVENT_NOTE_ON, 7, 63, 0.8),
                            noteEvent(a.position + 24000, CLAP_EVENT_NOTE_OFF, 7, 63, 0.5)});
        check(peakFrom(quiet, 24000) < 1e-5f && peakFrom(lr, 0) > 0.01f &&
                  peakFrom(lr, 48000) < 1e-5f,
              std::string("Mono ST: after ") + what[panic] + ", silence, and the next note ends: " +
                  std::to_string(peakFrom(quiet, 24000)) + " " +
                  std::to_string(peakFrom(lr, 48000)));
    }
}

// NOTE_END for every note that ends, exactly once. The host holds a slot for each note-on until
// the NOTE_END with its ID, key, channel and port, inside the block; reset and deactivation end
// notes without one (the host ends those itself).
struct NoteLog
{
    static constexpr uint32_t block = 128;
    Instance a;
    struct Start
    {
        int16_t channel, key, port;
    };
    std::map<int32_t, Start> started;
    std::map<int32_t, int> ended;
    std::set<int32_t> offed;    // notes the script released: the key goes up, NOTE_END or not
    std::set<int32_t> forgiven; // ended by a reset
    std::vector<Event> pending; // times in frames from the next block's start
    std::string problem;
    const oscairn_plugin_sound_contexts_t *contexts;
    const oscairn_plugin_voice_budget_t *budget;

    NoteLog()
        : contexts(static_cast<const oscairn_plugin_sound_contexts_t *>(
              a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_CONTEXTS))),
          budget(static_cast<const oscairn_plugin_voice_budget_t *>(
              a.plugin->get_extension(a.plugin, OSCAIRN_EXT_VOICE_BUDGET)))
    {
    }
    void on(uint32_t t, int32_t id, int16_t channel, int16_t key, int16_t port = 0,
            double velocity = 0.8)
    {
        auto e = noteEvent(t, CLAP_EVENT_NOTE_ON, id, key, velocity);
        e.note.channel = channel;
        e.note.port_index = port;
        pending.push_back(e);
        if (!started.count(id))
            started[id] = {channel, key, port};
    }
    void note(uint32_t t, uint16_t type, int32_t id, int16_t channel, int16_t key)
    {
        auto e = noteEvent(t, type, id, key, 0.5);
        e.note.channel = channel;
        pending.push_back(e);
    }
    void off(uint32_t t, int32_t id, int16_t channel, int16_t key)
    {
        note(t, CLAP_EVENT_NOTE_OFF, id, channel, key);
        offed.insert(id);
    }
    void midi(uint32_t t, uint8_t a_, uint8_t b, uint8_t c = 0)
    {
        pending.push_back(midiEvent(t, a_, b, c));
    }
    void param(const char *name, double v) { pending.push_back(paramEvent(0, fnv1a(name), v)); }
    void run(int blocks = 1)
    {
        for (int b = 0; b < blocks; ++b)
        {
            for (auto &e : pending)
                e.time += (uint32_t)a.position;
            a.out.noteEnds.clear();
            a.render(block, block, std::move(pending));
            pending.clear();
            for (auto &e : a.out.noteEnds)
            {
                auto s = started.find(e.note_id);
                if (s == started.end())
                {
                    if (problem.empty())
                        problem = "NOTE_END for an id never started: " + std::to_string(e.note_id);
                }
                else if (e.port_index != s->second.port || e.channel != s->second.channel ||
                         e.key != s->second.key || e.header.time >= block)
                {
                    if (problem.empty())
                        problem = "NOTE_END with wrong fields for id " + std::to_string(e.note_id);
                }
                if (++ended[e.note_id] > 1 && problem.empty())
                    problem = "NOTE_END twice for id " + std::to_string(e.note_id);
                if (forgiven.count(e.note_id) && problem.empty())
                    problem = "NOTE_END after a reset for id " + std::to_string(e.note_id);
            }
        }
    }
    std::string missing() const
    {
        std::string r;
        for (auto &[id, s] : started)
            if (!ended.count(id) && !forgiven.count(id))
                r += " " + std::to_string(id);
        return r;
    }
    // runs until every note has ended (at most `seconds`), then on a little, for late repeats
    void settle(double seconds = 12)
    {
        for (uint32_t n = 0; n * block < seconds * 48000 && !missing().empty(); ++n)
            run();
        run(100);
    }
    void verify(const std::string &what)
    {
        check(problem.empty(), what + ": " + problem);
        check(missing().empty(), what + ": no NOTE_END for ids" + missing());
    }
    // every key still down is released, whether or not its note has ended (a player lets go)
    void releaseAll()
    {
        for (auto &[id, s] : started)
            if (!offed.count(id))
                off(0, id, s.channel, s.key);
        run(2);
    }
    void reset()
    {
        for (auto &[id, s] : started)
            if (!ended.count(id))
                forgiven.insert(id);
        a.plugin->reset(a.plugin);
    }
    uint32_t addSound(int16_t channel)
    {
        char message[256] = {};
        auto id = contexts->prepare(a.plugin, "second", nullptr, 0, nullptr, nullptr, message,
                                    sizeof message);
        check(id != CLAP_INVALID_ID && contexts->select(a.plugin, id, channel), "a second sound");
        run();
        return id;
    }
    void limit(uint32_t voices, std::vector<oscairn_voice_budget_context_t> entries = {})
    {
        budget->set_limits(a.plugin, voices, entries.data(), (uint32_t)entries.size());
    }
    uint32_t voices()
    {
        double cost = 0;
        return budget->active_voices(a.plugin, &cost);
    }
};

// The device's voice caps, OSCAIRN_ANALOG_MAX_VOICES for the instance and ..._PER_SOUND for each
// sound, read when an instance is created: they stand where the host sets no limit and bound a
// higher one. Voices go in the budget's order, whole notes at a time.
struct CapEnv
{
    CapEnv(const char *instance, const char *perSound = nullptr)
    {
        set("OSCAIRN_ANALOG_MAX_VOICES", instance);
        set("OSCAIRN_ANALOG_MAX_VOICES_PER_SOUND", perSound);
    }
    ~CapEnv()
    {
        set("OSCAIRN_ANALOG_MAX_VOICES", nullptr);
        set("OSCAIRN_ANALOG_MAX_VOICES_PER_SOUND", nullptr);
    }
    static void set(const char *name, const char *v)
    {
        if (v)
            setenv(name, v, 1);
        else
            unsetenv(name);
    }
};

static void testDeviceCap()
{
    // a sine; velocity sets the gain (-24 dB at 0); a release of 8 s so released notes ring on
    const ParamList sine{{"a_osc1_type", 1},
                         {"a_lowcut.off", 1},
                         {"a_vca_velsense", -24},
                         {"a_env1_release", 3}};
    auto setup = [&](NoteLog &t, const ParamList &extra = {}) {
        for (auto &[name, v] : sine)
            if (!std::any_of(extra.begin(), extra.end(),
                             [&, name = name](auto &e) { return !std::strcmp(e.first, name); }))
                t.param(name, v);
        for (auto &[name, v] : extra)
            t.param(name, v);
        t.run();
    };
    // held notes only: the oldest go first, a cap of 3 keeps the last 3
    {
        CapEnv env("3");
        NoteLog t;
        setup(t);
        for (int i = 0; i < 6; ++i)
        {
            t.on(0, 1 + i, 0, 60 + i);
            t.run(8);
            check(t.voices() <= 3, "cap 3: at most 3 voices after each note-on settles");
        }
        check(t.voices() == 3 && t.ended.size() == 3 && t.ended.count(1) && t.ended.count(2) &&
                  t.ended.count(3),
              "cap 3, six held notes: the three oldest ended, three voices sound");
        t.releaseAll();
        t.settle();
        t.verify("cap held");
    }
    // a held note is never cut while a releasing one exists, and the quietest releasing note goes
    // first, whatever its age
    {
        CapEnv env("3");
        NoteLog t;
        setup(t);
        t.on(0, 1, 0, 60, 0, 0.9);
        t.on(0, 2, 0, 62, 0, 0.2);
        t.on(0, 3, 0, 64, 0, 0.5);
        t.run(8);
        t.off(0, 1, 0, 60);
        t.off(0, 2, 0, 62);
        t.off(0, 3, 0, 64);
        t.run(8);
        t.on(0, 4, 0, 66, 0, 0.8); // the quietest releasing note (2) goes, not the oldest (1)
        t.run(8);
        check(t.ended.size() == 1 && t.ended.count(2), "the quietest releasing note goes first");
        t.on(0, 5, 0, 67, 0, 0.8); // then 3 (0.5), before 1 (0.9), while 4 is held
        t.run(8);
        check(t.ended.size() == 2 && t.ended.count(3) && !t.ended.count(1) && !t.ended.count(4),
              "then the next quietest releasing note, never a held one");
        t.on(0, 6, 0, 69, 0, 0.8);
        t.run(8);
        check(t.ended.size() == 3 && t.ended.count(1), "the last releasing note goes next");
        t.on(0, 7, 0, 71, 0, 0.8); // only held ones remain: the oldest (4)
        t.run(8);
        check(t.ended.size() == 4 && t.ended.count(4) && t.voices() == 3,
              "held notes go only when no releasing one remains, the oldest first");
        t.releaseAll();
        t.settle();
        t.verify("cap order");
    }
    // the host's limit: a higher one does not lift the cap, a lower one stands
    {
        CapEnv env("4");
        NoteLog t;
        setup(t);
        t.limit(8);
        for (int i = 0; i < 6; ++i)
            t.on(0, 1 + i, 0, 60 + i);
        t.run(8);
        check(t.voices() == 4, "a host limit of 8 over a cap of 4: 4 voices");
        t.limit(2);
        t.run(8);
        check(t.voices() == 2, "a host limit of 2 under a cap of 4 stands");
        t.limit(0);
        t.on(0, 20, 0, 80);
        t.on(0, 21, 0, 81);
        t.on(0, 22, 0, 82);
        t.run(8);
        check(t.voices() == 4, "the host lifts its limit: the cap stands");
        t.releaseAll();
        t.settle();
        t.verify("cap and host limit");
    }
    // invalid values are ignored: no cap at all
    for (const char *bad : {"0", "-3", "abc", "3x", "", "129", "99999999999999999999"})
    {
        CapEnv env(bad, "65");
        NoteLog t;
        setup(t);
        for (int i = 0; i < 12; ++i)
            t.on(0, 1 + i, 0, 40 + i);
        t.run(8);
        check(t.voices() == 12 && t.ended.empty(),
              std::string("a cap of \"") + bad + "\" is ignored");
    }
    // a per-sound cap bounds each sound and leaves the other alone; the instance cap, the total
    {
        CapEnv env(nullptr, "2");
        NoteLog t;
        setup(t);
        const auto second = t.addSound(1);
        (void)second;
        for (int i = 0; i < 5; ++i)
            t.on(0, 1 + i, 0, 60 + i);
        t.on(0, 10, 1, 70);
        t.on(0, 11, 1, 71);
        t.on(0, 12, 1, 72);
        t.run(8);
        check(t.voices() == 4 && t.ended.size() == 4 && t.ended.count(1) && t.ended.count(2) &&
                  t.ended.count(3) && t.ended.count(10),
              "per-sound cap 2: each sound keeps its two newest notes");
        t.releaseAll();
        t.settle();
        t.verify("per-sound cap");
    }
    {
        CapEnv env("5", "3");
        NoteLog t;
        setup(t);
        t.addSound(1);
        for (int i = 0; i < 4; ++i)
            t.on(0, 1 + i, 0, 60 + i);
        t.run(8);
        check(t.voices() == 3, "the per-sound cap alone bounds a sound at 3");
        for (int i = 0; i < 4; ++i)
            t.on(0, 10 + i, 1, 70 + i);
        t.run(8);
        check(t.voices() == 5, "3 and 3 voices under an instance cap of 5: 5 sound");
        // a new note's own sound gives way first (the order's third rule): 1 went before, then the
        // second sound's own oldest, 10 and 11
        check(t.ended.size() == 3 && t.ended.count(1) && t.ended.count(10) && t.ended.count(11),
              "the new note's own sound gives up its older notes first");
        t.releaseAll();
        t.settle();
        t.verify("per-sound and instance caps");
    }
    // a long randomised run: notes of two sounds, struck and released at random, some held to
    // the end; the voices never exceed the cap when the fades have run, NOTE_ENDs balance
    {
        CapEnv env("5", "3");
        NoteLog t;
        setup(t, {{"a_env1_release", -1}});
        t.addSound(1);
        std::mt19937 rng(7);
        int id = 100;
        std::vector<int> down;
        std::map<int, std::pair<int, int>> keyOf;
        bool within = true;
        uint32_t peak = 0;
        for (int step = 0; step < 400; ++step)
        {
            const int r = int(rng() % 8);
            if (r < 5 || down.empty())
            {
                const int ch = int(rng() % 2), key = 36 + int(rng() % 60);
                t.on(uint32_t(rng() % 100), ++id, ch, key, 0, 0.3 + 0.7 * double(rng() % 100) / 100);
                keyOf[id] = {ch, key};
                down.push_back(id);
            }
            else
            {
                const size_t i = rng() % down.size();
                if (!t.ended.count(down[i]))
                    t.off(uint32_t(rng() % 100), down[i], keyOf[down[i]].first, keyOf[down[i]].second);
                down.erase(down.begin() + long(i));
            }
            t.run(1 + int(rng() % 20)); // at most 2560 frames: a fade is 144
            t.run(2);
            peak = std::max(peak, t.voices());
            within &= t.voices() <= 5;
        }
        std::printf("  device cap: randomised run peak %u voices\n", peak);
        check(within, "randomised run: voices never above the cap once the fades ended");
        t.releaseAll();
        t.settle();
        t.verify("randomised cap");
    }
    // an arpeggio: sixteenths at 84 BPM over three keys, gate 0.9, a release of about 1.2 s
    {
        const double step = 48000 * 60 / 84.0 / 4;
        auto arp = [&](const char *cap) {
            CapEnv env(cap);
            NoteLog t;
            setup(t, {{"a_env1_release", 0.26}, {"a_env1_sustain", 1}});
            const int keys[3] = {60, 64, 67};
            std::vector<int> checkAt;
            uint32_t peak = 0, atStep = 0;
            bool sounds = true;
            t.run();
            const uint64_t start = t.a.position;
            // blocks of 128 frames; each note-on or note-off goes into the block holding its time
            for (uint64_t pos = start; pos < start + uint64_t(36 * step); pos += NoteLog::block)
            {
                for (int n = 0; n < 36; ++n)
                {
                    const uint64_t on = start + uint64_t(n * step),
                                   off = on + uint64_t(0.9 * step);
                    if (on >= pos && on < pos + NoteLog::block)
                        t.on(uint32_t(on - pos), 500 + n, 0, keys[n % 3]);
                    if (off >= pos && off < pos + NoteLog::block)
                        t.off(uint32_t(off - pos), 500 + n, 0, keys[n % 3]);
                    // 100 ms after a note-on, once the line is established: it still sounds
                    if (n >= 12 && on + 4800 >= pos && on + 4800 < pos + NoteLog::block)
                        checkAt.push_back(500 + n);
                }
                t.run();
                peak = std::max(peak, t.voices());
                for (auto id : checkAt)
                {
                    const auto notes = Budget(t.a).notes();
                    sounds &= notes.count(id) && notes.at(id).loudness > 0.05f;
                }
                checkAt.clear();
                // just before the next note: the count is what the cap leaves
                const double rel = std::fmod(double(pos + NoteLog::block - start), step);
                if (pos > start + uint64_t(12 * step) && rel > step - NoteLog::block)
                    atStep = std::max(atStep, t.voices());
            }
            t.releaseAll();
            t.settle();
            t.verify("arpeggio");
            return std::tuple<uint32_t, uint32_t, bool>{peak, atStep, sounds};
        };
        const auto [openPeak, openSteady, openSounds] = arp(nullptr);
        const auto [capPeak, capSteady, capSounds] = arp("5");
        std::printf("  arpeggio: uncapped peak %u voices (steady %u); cap 5: peak %u (steady %u)\n",
                    openPeak, openSteady, capPeak, capSteady);
        check(openPeak >= 7 && openPeak <= 8, "the arpeggio overlaps 7 to 8 voices uncapped");
        // the one voice a new note takes the place of fades for 3 ms beside it: 6 at most, then 5
        check(capPeak <= 6 && capSteady == 5, "with a cap of 5 the count stays at 5");
        check(openSounds && capSounds, "every new note sounds, capped or not");
    }
}

static void testNoteLifetime()
{
    // a release that finishes, while another note is still held
    {
        NoteLog t;
        t.on(0, 1, 0, 60);
        t.on(10, 2, 3, 64);
        t.run(8);
        t.off(0, 2, 3, 64);
        t.run(48000 * 4 / NoteLog::block);
        check(t.ended.count(2) == 1 && !t.ended.count(1),
              "a released note ends while another is held");
        t.off(0, 1, 0, 60);
        t.settle();
        t.verify("release");
    }
    // Surge's own polyphony limit, more notes than it keeps
    {
        NoteLog t;
        t.param("polylimit", 3);
        t.run();
        for (int i = 0; i < 12; ++i)
            t.on(i, 100 + i, 0, 40 + i);
        t.run(8);
        t.releaseAll();
        t.settle();
        t.verify("polyphony limit");
        check(t.ended.size() == 12, "every one of 12 notes ended");
    }
    // far more notes held than the voices Surge keeps
    {
        NoteLog t;
        for (int i = 0; i < 300; ++i)
            t.on(i % 128, 1000 + i, i % 4, 24 + i % 80);
        t.run(60);
        t.releaseAll();
        t.settle();
        t.verify("300 notes held");
    }
    // the host's budget: its limit, reserves and priorities on two sounds, tightened and lifted
    for (int variant = 0; variant < 3; ++variant)
    {
        NoteLog t;
        const auto second = t.addSound(1);
        if (variant == 0)
            t.limit(3);
        if (variant == 1)
            t.limit(4, {{0, 0, 3, 0, 0}, {second, 0, 0, 9, 0}});
        if (variant == 2)
            t.limit(0, {{OSCAIRN_VOICE_BUDGET_SELECTED, 2, 0, 1, 0}});
        for (int i = 0; i < 8; ++i)
            t.on(i, 300 + i, i % 2, 40 + i);
        t.run(4);
        t.limit(2);
        t.run(4);
        for (int i = 0; i < 4; ++i)
            t.on(0, 320 + i, i % 2, 60 + i);
        t.run(4);
        t.limit(0);
        t.releaseAll();
        t.settle();
        t.verify("host budget " + std::to_string(variant));
    }
    // every play mode: keys over each other, a held key released first, a key struck again
    for (int mode = 0; mode < n_play_modes; ++mode)
    {
        NoteLog t;
        t.param("a_polymode", mode);
        t.run();
        t.on(0, 500, 0, 60);
        t.run(4);
        t.on(0, 501, 0, 64);
        t.run(4);
        t.on(0, 502, 0, 50);
        t.run(4);
        t.off(0, 501, 0, 64);
        t.run(4);
        t.on(0, 503, 0, 64); // struck again
        t.on(0, 504, 0, 64); // and once more under it
        t.off(0, 500, 0, 60);
        t.run(4);
        t.off(0, 503, 0, 64);
        t.run(4);
        t.releaseAll();
        if (mode == pm_latch)
        {
            // Latch keeps the last notes sounding after their keys are up: they end with all
            // sound off, which then ends each once
            t.run(48000 / NoteLog::block);
            check(t.voices() > 0, "latch keeps its notes sounding after the keys are released");
            t.midi(0, 0xB0, 120, 0);
        }
        t.settle();
        t.verify("play mode " + std::to_string(mode));
    }
    // unison: several voices to a note, one NOTE_END when the last ends, also with voices
    // given up one by one
    for (int variant = 0; variant < 3; ++variant)
    {
        NoteLog t;
        t.param("a_unison_voices", 7);
        if (variant == 1)
            t.param("polylimit", 9);
        t.run();
        if (variant == 2)
            t.limit(5);
        for (int i = 0; i < 4; ++i)
            t.on(i, 600 + i, 0, 50 + 3 * i);
        t.run(8);
        t.off(0, 600, 0, 50);
        t.run(4);
        if (variant == 2)
            t.limit(3);
        t.run(8);
        t.releaseAll();
        t.settle();
        t.verify("unison " + std::to_string(variant));
    }
    // what active_voices counts: Surge's voices, a unison note's each
    {
        NoteLog t;
        t.param("a_unison_voices", 7);
        t.run();
        t.on(0, 650, 0, 60);
        t.run(8);
        check(t.voices() == 7, "active_voices counts each voice of a unison note");
        t.off(0, 650, 0, 60);
        t.run(1);
        check(t.voices() == 7, "and counts them while they release");
        t.settle();
        t.verify("unison voices counted");
    }
    // the same key again, with the first still held (and not)
    {
        NoteLog t;
        t.on(0, 700, 0, 60);
        t.run(4);
        t.on(0, 701, 0, 60);
        t.run(4);
        t.off(0, 701, 0, 60);
        t.run(4);
        t.off(0, 700, 0, 60);
        t.settle();
        t.verify("same key again");
    }
    // note-ons that get no voice: another port, a key or channel out of range
    {
        NoteLog t;
        t.on(0, 800, 0, 60, 1);
        t.on(0, 801, 0, 128);
        t.on(0, 802, 0, -1);
        t.on(0, 803, 16, 60);
        t.on(0, 804, -1, 60);
        t.run(4);
        t.on(0, 805, 0, 60);
        t.run(2);
        t.off(0, 805, 0, 60);
        t.settle();
        t.verify("note-on without a voice");
    }
    // a key-on delay still pending at the note-off, and at a state load
    {
        NoteLog t;
        t.param("a_keyon_delay", -2);
        t.param("a_keyon_delay.off", 0);
        t.run();
        t.on(0, 900, 0, 60);
        t.run(4);
        check(t.voices() == 0, "active_voices does not count a note waiting out its key-on delay");
        t.off(0, 900, 0, 60);
        t.run(4);
        t.on(0, 901, 0, 64);
        t.on(0, 902, 0, 67);
        t.run(8);
        t.off(0, 902, 0, 67);
        t.settle();
        t.off(0, 901, 0, 64);
        t.settle();
        t.verify("key-on delay");
    }
    // CC 120 and CC 123, on one channel and then the other
    for (int cc : {120, 123})
    {
        NoteLog t;
        t.on(0, 1000, 0, 60);
        t.on(0, 1001, 0, 64);
        t.on(0, 1002, 1, 67);
        t.run(8);
        t.midi(5, 0xB0, (uint8_t)cc, 0);
        t.run(4);
        t.midi(5, 0xB1, (uint8_t)cc, 0);
        t.run(4);
        t.settle();
        t.verify("CC " + std::to_string(cc));
    }
    // choke and note-off by ID, by key and channel, with wildcards, and with an ID it does not know
    {
        NoteLog t;
        t.on(0, 1100, 0, 60);
        t.on(0, 1101, 0, 61);
        t.on(0, 1102, 2, 62);
        t.on(0, 1103, 2, 63);
        t.on(0, 1104, 4, 64);
        t.on(0, 1105, 5, 65);
        t.run(8);
        t.note(0, CLAP_EVENT_NOTE_CHOKE, -1, 0, 60);
        t.note(0, CLAP_EVENT_NOTE_CHOKE, 1101, -1, -1);
        t.note(0, CLAP_EVENT_NOTE_OFF, -1, 2, 62);
        t.note(0, CLAP_EVENT_NOTE_OFF, -1, -1, 63);
        t.note(0, CLAP_EVENT_NOTE_CHOKE, -1, 4, -1);
        t.run(8);
        t.off(0, 7777, 5, 65); // an ID the plugin does not know: nothing to release
        t.run(2);
        check(t.ended.count(1105) == 0, "a note-off with another ID releases nothing");
        t.off(0, 1105, 5, 65);
        t.settle();
        t.verify("choke and note-off addressing");
    }
    // a note-off before its note-on, and twice
    {
        NoteLog t;
        t.off(0, 1200, 0, 60);
        t.on(1, 1200, 0, 60);
        t.off(2, 1200, 0, 60);
        t.off(3, 1200, 0, 60);
        t.settle();
        t.verify("note-off twice");
    }
    // a sound switched under held notes, and retiring sounds choked
    {
        NoteLog t;
        const auto second = t.addSound(1);
        t.on(0, 1300, 0, 60);
        t.on(0, 1301, 1, 64);
        t.run(8);
        check(t.contexts->select(t.a.plugin, second, 0), "select the second sound over held notes");
        t.run(4);
        t.on(0, 1302, 0, 70);
        t.run(2);
        t.off(0, 1300, 0, 60);
        t.run(2);
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            t.a.plugin->get_extension(t.a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        check(snaps && snaps->choke_retiring(t.a.plugin), "choke_retiring");
        check(t.contexts->select(t.a.plugin, 0, -1), "select the default everywhere");
        t.run(8);
        t.releaseAll();
        t.settle();
        t.verify("sound switch and choke_retiring");
    }
    // states loaded while notes are held, ringing and waiting
    {
        NoteLog t;
        const auto plain = t.a.save();
        t.param("a_polymode", pm_mono);
        t.param("a_unison_voices", 5);
        t.run();
        const auto other = t.a.save();
        check(t.a.load(plain), "load the plain state");
        t.on(0, 1400, 0, 60);
        t.on(0, 1401, 0, 64);
        t.run(8);
        t.off(0, 1401, 0, 64);
        t.run(1);
        check(t.a.load(other), "load another state while notes are held and ringing");
        t.run(8);
        t.on(0, 1402, 0, 67);
        t.on(1, 1403, 0, 70);
        t.run(8);
        check(t.a.load(plain), "load the first again");
        t.run(8);
        t.on(0, 1404, 0, 67);
        t.run(2);
        t.releaseAll();
        t.settle();
        t.verify("state load");
    }
    // reset: no NOTE_END for the notes it ended, then the instance goes on as new
    {
        NoteLog t;
        t.on(0, 1600, 0, 60);
        t.on(0, 1601, 0, 64);
        t.run(8);
        t.off(0, 1601, 0, 64);
        t.run(1);
        t.reset();
        t.run(40);
        t.on(0, 1602, 0, 60);
        t.run(8);
        t.off(0, 1602, 0, 60);
        t.settle();
        t.verify("reset");
        check(!t.ended.count(1600) && !t.ended.count(1601) && t.ended.count(1602) == 1,
              "no NOTE_END after a reset, one for the new note");
    }
    // deactivate and activate with notes held: none after
    {
        NoteLog t;
        t.on(0, 1700, 0, 60);
        t.run(8);
        t.a.plugin->stop_processing(t.a.plugin);
        t.a.plugin->deactivate(t.a.plugin);
        t.forgiven.insert(1700);
        t.a.activate(48000);
        t.run(40);
        t.verify("deactivate");
    }
    // what active_voices counts
    {
        NoteLog t;
        check(t.voices() == 0, "no voices at the start");
        t.on(0, 1800, 0, 60);
        t.on(0, 1801, 0, 64);
        t.run(16);
        check(t.voices() == 2, "active_voices counts held notes");
        t.off(0, 1800, 0, 60);
        t.run(1);
        check(t.voices() == 2, "and a releasing one");
        t.releaseAll();
        t.settle();
        check(t.voices() == 0, "no voices when every note has ended");
        t.verify("active_voices");
    }

    // The long run: thousands of notes across keys and channels, far past the voices Surge
    // keeps, with play modes, unison, key-on delay, Surge's polyphony limit and the host's budget
    // changing, the pedal, CC 120 and 123, chokes, wildcards, a second sound and a few resets.
    {
        NoteLog t;
        const auto second = t.addSound(1);
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        auto rnd = [&](uint32_t n) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            return (uint32_t)((rng >> 11) % n);
        };
        std::vector<int32_t> held;
        int32_t next = 10000;
        int notes = 0, resets = 0;
        for (int step = 0; step < 5000; ++step)
        {
            const uint32_t events = 1 + rnd(5);
            for (uint32_t k = 0; k < events; ++k)
            {
                const uint32_t kind = rnd(100), time = rnd(NoteLog::block);
                if (kind < 45)
                {
                    t.on(time, next, (int16_t)rnd(4), (int16_t)(24 + rnd(72)));
                    held.push_back(next++);
                    ++notes;
                }
                else if (kind < 80 && !held.empty())
                {
                    const size_t i = rnd((uint32_t)held.size());
                    const int32_t id = held[i];
                    held.erase(held.begin() + (long)i);
                    const auto s = t.started[id];
                    t.off(time, rnd(8) == 0 ? -1 : id, s.channel, s.key);
                }
                else if (kind < 84 && !held.empty())
                {
                    const int32_t id = held[rnd((uint32_t)held.size())];
                    const auto s = t.started[id];
                    t.note(time, CLAP_EVENT_NOTE_CHOKE, rnd(2) ? id : -1, s.channel,
                           rnd(2) ? s.key : -1);
                }
                else if (kind < 86)
                    t.midi(time, (uint8_t)(0xB0 | rnd(4)), rnd(2) ? 123 : 120, 0);
                else if (kind < 88)
                    t.midi(time, (uint8_t)(0xB0 | rnd(4)), 64, rnd(2) ? 127 : 0);
                else if (kind < 92)
                {
                    switch (rnd(4))
                    {
                    case 0:
                        t.param("a_polymode", rnd(n_play_modes));
                        break;
                    case 1:
                        t.param("a_unison_voices", 1 + rnd(7));
                        break;
                    case 2:
                        t.param("polylimit", 2 + rnd(7));
                        break;
                    default:
                        t.param("a_keyon_delay.off", rnd(2));
                        t.param("a_keyon_delay", -4 + (double)rnd(3));
                    }
                }
                else if (kind < 96)
                {
                    const uint32_t n = rnd(3);
                    std::vector<oscairn_voice_budget_context_t> c;
                    if (n > 0)
                        c.push_back({0, rnd(4), rnd(3), (uint8_t)rnd(11), 0});
                    if (n > 1)
                        c.push_back({second, rnd(4), rnd(3), (uint8_t)rnd(11), 0});
                    t.limit(rnd(3) == 0 ? 0 : 1 + rnd(12), c);
                }
            }
            t.run();
            if (step % 1250 == 1249)
            {
                t.reset();
                for (auto id : held)
                    t.offed.insert(id); // the player lets go after the reset; those keys are not sent again
                held.clear();
                ++resets;
            }
        }
        t.limit(0);
        t.param("a_polymode", pm_poly);
        t.param("a_keyon_delay.off", 1);
        t.releaseAll();
        for (int c = 0; c < 4; ++c)
            t.midi(0, (uint8_t)(0xB0 | c), 64, 0);
        t.run(2);
        for (int c = 0; c < 4; ++c)
            t.midi(0, (uint8_t)(0xB0 | c), 123, 0);
        t.run(2);
        t.releaseAll();
        t.settle(30);
        t.verify("randomised run");
        size_t counted = 0;
        for (auto &[id, n] : t.ended)
            counted += (size_t)n;
        const size_t expected = t.started.size() - t.forgiven.size();
        std::printf("randomised run: %d note-ons, %d resets, %zu NOTE_END, %zu expected\n", notes,
                    resets, counted, expected);
        check(counted == expected,
              "randomised run: NOTE_END count equals the note-ons not ended by a reset");
    }
}

// Voices given up past the host's limits: a lowered limit fades the excess out at the next
// block, without a click; a new note takes a voice in voice-budget.h's order across two sounds;
// the voices and notes reported, fading ones included; NOTE_END after a note's last voice;
// Surge's own Polyphony Limit unchanged without a limit.
static void testVoiceBudget()
{
    // a sine, so a cut would show as a step; velocity sets the amp's gain (-24 dB at 0)
    const ParamList sine{{"a_osc1_type", 1},
                         {"a_lowcut.off", 1},
                         {"a_vca_velsense", -24},
                         {"a_env1_release", 1}};
    {
        Instance a;
        Budget b(a);
        check(b.x->honors_volume(a.plugin), "every voice follows the volume expression");
        setParams(a, sine);
        auto before = a.render(4800, 64, {noteOn(0, 1, 0, 60, 1)});
        auto after = a.render(4800, 64, {expressionEvent(a.position, CLAP_NOTE_EXPRESSION_VOLUME, 1, 0)});
        float p0 = 0, p1 = 0;
        for (size_t i = before.size() / 2; i < before.size(); ++i)
            p0 = std::max(p0, std::fabs(before[i]));
        for (size_t i = after.size() / 2; i < after.size(); ++i)
            p1 = std::max(p1, std::fabs(after[i]));
        check(p0 > 1e-2f && p1 < 1e-4f * p0, "a note at volume expression 0 is silent");
    }
    // A limit lowered from none to 2 with four notes held: the two quietest fade out from the
    // next block over 3 ms, against the same render without the limit.
    {
        const uint32_t T = 24000; // a block boundary
        std::vector<Event> on;
        for (int k = 0; k < 4; ++k)
            on.push_back(noteOn(1000 * k, 1 + k, 0, (int16_t)(60 + 4 * k), 0.9 - 0.1 * k));
        Instance a, b;
        setParams(a, sine);
        setParams(b, sine);
        Budget ba(a);
        a.render(T, 64, on);
        b.render(T, 64, on);
        check(ba.active() == 4, "four voices");
        ba.set(2);
        std::vector<float> removed; // left channel, without the limit minus with it
        uint32_t endAt = 0;
        for (uint32_t t = T; t < T + 4800; t += 16)
        {
            auto x = a.render(16, 16), y = b.render(16, 16);
            for (size_t i = 0; i < x.size(); i += 2)
                removed.push_back(y[i] - x[i]);
            if (!endAt && hasNoteEnd(a, 3) && hasNoteEnd(a, 4))
                endAt = t + 16 - T;
            if (t + 16 == T + 64)
                check(ba.active() == 4 && ba.notes().size() == 4,
                      "fading voices are counted and reported until they end");
        }
        size_t t0 = 0;
        while (t0 < removed.size() && removed[t0] == 0)
            ++t0;
        // A click is a corner: the second difference, which a sine of these keys keeps under
        // 0.5 % of its level and a 3 ms ramp's corners add 0.7 % to, and a cut makes the
        // signal's value then.
        auto at = [&](size_t i) { return i < removed.size() ? removed[i] : 0.f; };
        float corner = 0, level = 0;
        for (size_t i = 0; i < BLOCK_SIZE + 240 + 2; ++i)
            corner = std::max(corner, std::fabs(at(i) - 2 * (i ? at(i - 1) : 0.f) +
                                                (i > 1 ? at(i - 2) : 0.f)));
        for (size_t i = 300; i < 2700; ++i)
            level = std::max(level, std::fabs(removed[i]));
        std::printf("  voice budget: fade from sample %zu, NOTE_END by %u, largest second "
                    "difference %.5f of %.4f\n",
                    t0, endAt, corner, level);
        check(t0 < BLOCK_SIZE, "a lowered limit takes effect within a block");
        check(endAt >= 96 && endAt <= BLOCK_SIZE + 240 + 16,
              "the voices given up fade over 2 to 5 ms, then end");
        check(level > 1e-2f && corner <= 0.02f * level,
              "no click: no second difference over 2 % of the faded notes' level");
        auto n = ba.notes();
        check(ba.active() == 2 && n.size() == 2 && n.count(1) && n.count(2) && !hasNoteEnd(a, 1) &&
                  !hasNoteEnd(a, 2),
              "the two loudest notes keep sounding");
    }
    // Two sounds: the default on channel 0, a prepared one on channel 1.
    {
        Instance a;
        setParams(a, sine);
        Sounds s(a);
        Budget b(a);
        std::vector<oscairn_sound_parameter_t> values;
        for (auto &[name, v] : sine)
            values.push_back({fnv1a(name), v});
        char message[256] = "";
        auto id = s.x->prepare(a.plugin, "second", values.data(), (uint32_t)values.size(), nullptr,
                               nullptr, message, sizeof(message));
        check(id != CLAP_INVALID_ID && s.x->select(a.plugin, id, 1),
              std::string("a second sound on channel 1: ") + message);
        // 1: default, quiet, held; 2: default, loud, released; 3, 4: second, loud and quieter, held
        a.render(9600, 64,
                 {noteOn(0, 1, 0, 60, 0.2), noteOn(0, 2, 0, 62, 1), noteOn(0, 3, 1, 64, 1),
                  noteOn(0, 4, 1, 65, 0.5), noteEvent(4800, CLAP_EVENT_NOTE_OFF, 2, 62, 0)});
        b.set(4);
        a.render(4800, 64);
        auto n = b.notes();
        check(b.active() == 4 && a.out.noteEnds.empty(), "at the limit, nothing is given up");
        check(n.size() == 4 && n[2].loudness > n[4].loudness && n[4].loudness > n[1].loudness,
              "the released note is louder than note 4, which is louder than note 1");
        // one process() call: the room is made at the note, not at the next call's start
        a.render(4800, 4800, {noteOn(a.position, 5, 1, 67, 0.9)});
        check(hasNoteEnd(a, 2) && !hasNoteEnd(a, 1) && !hasNoteEnd(a, 3) && !hasNoteEnd(a, 4) &&
                  !hasNoteEnd(a, 5) && b.active() == 4,
              "a new note takes a released voice first, of another sound and louder");
        a.render(4800, 4800, {noteOn(a.position, 6, 1, 69, 0.9)});
        check(hasNoteEnd(a, 4) && !hasNoteEnd(a, 1) && !hasNoteEnd(a, 3) && !hasNoteEnd(a, 5) &&
                  !hasNoteEnd(a, 6) && b.active() == 4,
              "then the quietest held voice of the new note's own sound, before a quieter one "
              "of another sound");
    }
    // Unison 3: voices and notes reported; NOTE_END only after a note's last voice.
    {
        Instance a;
        auto ps = sine;
        ps.push_back({"a_unison_voices", 3});
        setParams(a, ps);
        Budget b(a);
        a.render(4800, 64, {noteOn(0, 7, 0, 60, 1), noteOn(0, 8, 0, 67, 0.5)});
        auto n = b.notes();
        const double ratio = std::pow(10.0, -24 * (1 - 64 / 127.0) / 20);
        check(b.active() == 6 && n.size() == 2 && n[7].voices == 3 && n[8].voices == 3 &&
                  n[7].key == 60 && n[7].channel == 0 && n[7].port_index == 0,
              "six voices, two notes of three voices, by the host's address");
        check(n[7].loudness > 0.1f && n[7].loudness <= 1.0001f &&
                  std::fabs(n[8].loudness / n[7].loudness / ratio - 1) < 0.02,
              "a note's loudness is its amp's gain");
        b.set(4);
        a.render(64, 64);
        check(b.active() == 6 && b.notes()[8].voices == 3,
              "fading voices are counted until they end");
        a.render(1024, 64);
        n = b.notes();
        check(b.active() == 4 && n[7].voices == 3 && n[8].voices == 1 && !hasNoteEnd(a, 8),
              "two of a note's three voices given up: the note sounds on, no NOTE_END");
        b.set(3);
        a.render(64, 64);
        check(!hasNoteEnd(a, 8), "no NOTE_END while a note's last voice fades");
        a.render(1024, 64);
        check(hasNoteEnd(a, 8) && !hasNoteEnd(a, 7) && b.active() == 3 && b.notes().size() == 1,
              "NOTE_END once a note's last voice has ended");
    }
    // Surge's Polyphony Limit (4) full: without a limit from the host it releases the oldest
    // note, exactly as stock Surge renders; under a budget, the quietest.
    {
        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto stock = std::make_unique<SurgeSynthesizer>(&layer, config);
        stock->storage.getPatch().polylimit.val.i = 4;
        stock->storage.getPatch().scene[0].vca_velsense.val.f = -24;
        void *data = nullptr;
        auto size = stock->saveRaw(&data);
        std::vector<uint8_t> patch((uint8_t *)data, (uint8_t *)data + size);
        const double vel[5] = {0.9, 0.3, 0.9, 0.9, 0.9};
        std::vector<Event> ev;
        for (int k = 0; k < 5; ++k)
            ev.push_back(noteOn(3200 * k, 1 + k, 0, (int16_t)(60 + 2 * k), vel[k]));
        Instance a, b, c;
        check(a.load(patch) && b.load(patch) && c.load(patch), "the patch loads");
        auto plugin = a.render(24000, 64, ev);

        auto &sp = stock->storage.getPatch();
        stock->setSamplerate(48000);
        stock->seedRandom(0x05CA1A4E); // the plugin's render seed
        sp.scenemode.val.i = sm_single;
        sp.scene_active.val.i = 0;
        sp.fx_disable.val.i = (1 << n_fx_slots) - 1;
        sp.copy_globaldata(sp.globaldata);
        sp.copy_scenedata(sp.scenedata[0], sp.scenedataOrig[0], 0);
        stock->audio_processing_active = true;
        std::vector<float> ref;
        for (uint32_t t = 0; t < 24000; t += BLOCK_SIZE)
        {
            for (int k = 0; k < 5; ++k)
                if (t == 3200u * k)
                    stock->playNote(0, 60 + 2 * k, (char)std::lround(vel[k] * 127), 0, k + 1);
            stock->process();
            stock->time_data.ppqPos += (double)BLOCK_SIZE * stock->time_data.tempo / (60. * 48000);
            for (int i = 0; i < BLOCK_SIZE; ++i)
            {
                ref.push_back(stock->output[0][i]);
                ref.push_back(stock->output[1][i]);
            }
        }
        check(plugin == ref, "with no limit, the Polyphony Limit steals exactly as stock Surge");
        check(hasNoteEnd(a, 1) && !hasNoteEnd(a, 2), "stock: the oldest note goes");
        Budget bb(b), bc(c);
        bb.set(0); // a budget, no limit
        b.render(24000, 64, ev);
        check(hasNoteEnd(b, 2) && !hasNoteEnd(b, 1),
              "under a budget, the Polyphony Limit releases the quietest note");
        bc.set(2);
        bc.set(0, false);
        check(c.render(24000, 64, ev) == plugin, "with the budget gone, stock again");
    }
}

// CLAP reset, as the host sends it to an instance it takes over (audio thread, before giving it a
// state): it takes effect at once, so a note in the next block sounds at full level; the
// instance then plays exactly as a fresh one with the same state, whatever it played before
// (notes held, the pedals, bend, pressure, controllers, MPE turned on by RPN, MIDI's all sound
// off); every prepared sound with it; and nothing is allocated (reset runs under the allocation
// guard).
static void testReset(const std::vector<std::string> &patches)
{
    auto at = [](std::vector<Event> ev, uint64_t from) {
        for (auto &e : ev)
            e.time += (uint32_t)from;
        return ev;
    };
    auto reset = [](Instance &a) {
        forbidAllocation = true;
        a.plugin->reset(a.plugin);
        forbidAllocation = false;
    };
    // what the outgoing Performance played: held notes, one released under the pedal, both
    // pedals, bend, pressure, controllers, MPE turned on by RPN and, just before the reset,
    // MIDI's all sound off
    auto busy = [](int16_t ch) {
        const uint8_t cc = 0xB0 | ch;
        return std::vector<Event>{
            noteOn(0, 1, ch, 48, 0.8),       noteOn(0, 2, ch, 60, 0.8),
            noteOn(0, 3, ch, 64, 0.8),       noteOn(0, 4, ch, 67, 0.8),
            noteOn(50, 5, ch, 72, 0.7), // an odd count, so the alternating sources are flipped
            midiEvent(100, cc, 64, 127),     midiEvent(150, cc, 66, 127),
            midiEvent(200, 0xE0 | ch, 0x7F, 0x7F), midiEvent(300, cc, 1, 100),
            midiEvent(310, cc, 2, 90),       midiEvent(320, cc, 11, 40),
            midiEvent(400, 0xD0 | ch, 90, 0), midiEvent(500, 0xA0 | ch, 60, 70),
            midiEvent(600, cc, 101, 0),      midiEvent(601, cc, 100, 6),
            midiEvent(602, cc, 6, 1),        noteEvent(10000, CLAP_EVENT_NOTE_OFF, 1, 48, 0),
            midiEvent(23990, cc, 120, 0)}; // all sound off: Surge's fade, cut short by the reset
    };
    // what the incoming one plays: a data entry (an RPN the outgoing one left selected would
    // take it), a bend and two notes
    auto incoming = [](int16_t ch, int32_t id) {
        return std::vector<Event>{midiEvent(0, 0xB0 | ch, 6, 1), midiEvent(0, 0xE0 | ch, 0, 0x50),
                                  noteOn(0, id, ch, 60, 0.8), noteOn(0, id + 1, ch, 64, 0.6),
                                  noteEvent(24000, CLAP_EVENT_NOTE_OFF, id, 60, 0),
                                  noteEvent(24000, CLAP_EVENT_NOTE_OFF, id + 1, 64, 0)};
    };
    auto rms = [](const std::vector<float> &lr, size_t from, size_t to) {
        double e = 0;
        for (size_t i = 2 * from; i < 2 * to; ++i)
            e += (double)lr[i] * lr[i];
        return std::sqrt(e / (2 * (to - from)));
    };
    const uint32_t frames = 48000;

    // At once: notes struck in the first block after a reset sound at full level.
    {
        Instance a, b;
        a.render(24000, 64, busy(0), false);
        reset(a);
        const size_t ended = a.out.noteEnds.size();
        auto x = a.render(frames, 64, at(incoming(0, 11), a.position));
        auto y = b.render(frames, 64, incoming(0, 11));
        check(rms(y, 4800, 24000) > 1e-3 && rms(x, 4800, 24000) >= 0.99 * rms(y, 4800, 24000),
              "notes struck in the block after a reset sound at full level: " +
                  std::to_string(rms(x, 4800, 24000)) + " against " +
                  std::to_string(rms(y, 4800, 24000)));
        // reset has no output events; the notes it ended are not reported later either
        bool stale = false;
        for (size_t k = ended; k < a.out.noteEnds.size(); ++k)
            stale |= a.out.noteEnds[k].note_id >= 1 && a.out.noteEnds[k].note_id <= 4;
        check(!stale && hasNoteEnd(a, 11) && hasNoteEnd(a, 12),
              "no NOTE_END after a reset for the notes it ended; the next notes end");
    }

    // As a fresh instance: reset and the same state loaded again, or another Performance's state
    // after this one played; kept loaded and reset with no load (its state unchanged); the host's
    // takeover (reset, deactivate, load, activate) against a fresh instance loaded before
    // activation. Every 8th factory patch, and one where what the reset clears would show:
    // oscillator 1's pitch follows every controller, key and alternating source it clears and a
    // free-running, tempo-synced voice LFO, played on channel 1 (where MPE left on would change
    // how the bend applies).
    std::vector<uint8_t> routed;
    {
        HeadlessLayer layer;
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto s = std::make_unique<SurgeSynthesizer>(&layer, config);
        auto &sc = s->storage.getPatch().scene[0];
        sc.lfo[0].trigmode.val.i = lm_freerun;
        sc.lfo[0].rate.temposync = true;
        float depth = 0.02f;
        for (auto m : {ms_polyaftertouch, ms_aftertouch, ms_breath, ms_expression, ms_sustain,
                       ms_lowest_key, ms_highest_key, ms_latest_key, ms_alternate_bipolar,
                       ms_lfo1})
            s->setModDepth01(sc.osc[0].pitch.id, m, 0, 0, depth += 0.01f);
        void *data = nullptr;
        auto size = s->saveRaw(&data);
        routed.assign((uint8_t *)data, (uint8_t *)data + size);
    }
    struct Case
    {
        std::string name;
        std::vector<uint8_t> patch, other;
        int16_t channel;
    };
    std::vector<Case> cases;
    for (size_t i = 0; i < patches.size(); i += 8)
        cases.push_back({patches[i], patchFromFxp(patches[i]),
                         patchFromFxp(patches[(i * 7 + 13) % patches.size()]), 0});
    cases.push_back({"routed", routed, patchFromFxp(patches[0]), 1});
    int compared = 0;
    for (auto &c : cases)
    {
        const auto &x = c.patch;
        const auto ch = c.channel;
        Instance f;
        if (!f.load(x))
            continue;
        auto fresh = f.render(frames, 64, incoming(ch, 11), false);
        Instance g;
        g.plugin->stop_processing(g.plugin);
        g.plugin->deactivate(g.plugin);
        g.load(x);
        g.activate(48000);
        auto freshLoadedFirst = g.render(frames, 64, incoming(ch, 11), false);
        for (int way = 0; way < 4; ++way)
        {
            Instance a;
            if (!a.load(way == 1 ? c.other : x))
                a.load(x);
            a.render(25000, 64, busy(ch), false); // not a whole beat at 120 BPM
            const auto before = a.save();
            reset(a);
            if (way == 3)
            {
                a.plugin->stop_processing(a.plugin);
                a.plugin->deactivate(a.plugin);
            }
            if (way != 2)
                a.load(x);
            if (way == 3)
                a.activate(48000);
            // with no load it keeps its state (which holds the mod wheel's position)
            std::vector<float> kept;
            if (way == 2)
            {
                check(a.save() == before, "a reset leaves the state as it was: " + c.name);
                Instance k;
                k.load(before);
                kept = k.render(frames, 64, incoming(ch, 11), false);
            }
            auto y = a.render(frames, 64, at(incoming(ch, 11), a.position), false);
            static const char *ways[] = {"the same state loaded again", "after another state",
                                         "no load", "deactivated, loaded and activated"};
            check(y == (way == 3 ? freshLoadedFirst : way == 2 ? kept : fresh),
                  std::string("after a reset, ") + ways[way] + ", it renders as a fresh "
                  "instance: " + c.name);
        }
        ++compared;
    }
    check(compared > 50, "patches compared after a reset: " + std::to_string(compared));

    // Every sound: two prepared presets on channels 1 and 2, the first then taken off channel 1
    // while its notes ring on; after a reset the instance plays as one that never played.
    {
        std::string p1 = patches[patches.size() / 3], p2 = patches[2 * patches.size() / 3];
        auto prepare = [&](Instance &a, Sounds &s) {
            auto id1 = s.preset(p1), id2 = s.preset(p2);
            check(id1 != CLAP_INVALID_ID && id2 != CLAP_INVALID_ID &&
                      s.x->select(a.plugin, id1, 1) && s.x->select(a.plugin, id2, 2),
                  "two presets prepared and selected");
            return id2;
        };
        Instance a, b;
        Sounds sa(a), sb(b);
        auto a2 = prepare(a, sa);
        std::vector<Event> ev;
        for (int16_t ch : {0, 1, 2})
            for (auto e : busy(ch))
            {
                // the mod wheel's position is the sound's state, which a reset keeps (above)
                const auto &d = e.midi.data;
                if (e.header.type == CLAP_EVENT_MIDI && (d[0] & 0xF0) == 0xB0 && d[1] == 1)
                    continue;
                if (e.header.type != CLAP_EVENT_MIDI)
                    e.note.note_id += ch * 100;
                ev.push_back(e);
            }
        a.render(12000, 64, ev, false);
        sa.x->select(a.plugin, a2, 1);
        a.render(12000, 64, {}, false);
        reset(a);
        sb.x->select(b.plugin, prepare(b, sb), 1);
        std::vector<Event> after;
        for (int16_t ch : {0, 1, 2})
            for (auto e : incoming(ch, 11 + 10 * ch))
                after.push_back(e);
        auto x = a.render(frames, 64, at(after, a.position));
        auto y = b.render(frames, 64, after);
        check(rms(y, 4800, 24000) > 1e-3 && x == y,
              "after a reset every sound plays as in an instance that never played");
    }
    // A sound silent and selected by no channel at the reset (which the main thread may be
    // using, so it is cleared when next selected) plays as one that never played.
    {
        auto idle = [](Instance &a, Sounds &s) {
            char message[256] = "";
            auto id = s.x->prepare(a.plugin, "idle", nullptr, 0, nullptr, nullptr, message,
                                   sizeof(message));
            check(id != CLAP_INVALID_ID && s.x->select(a.plugin, id, 3),
                  std::string("a sound prepared and selected: ") + message);
            return id;
        };
        Instance a, b;
        Sounds sa(a), sb(b);
        auto id = idle(a, sa);
        auto off = noteEvent(4800, CLAP_EVENT_NOTE_OFF, 1, 48, 0);
        off.note.channel = 3;
        a.render(9600, 64,
                 {noteOn(0, 1, 3, 48, 0.8), midiEvent(100, 0xE3, 0x7F, 0x7F),
                  midiEvent(200, 0xD3, 90, 0), off},
                 false);
        sa.x->select(a.plugin, 0, 3);
        a.render(2 * 48000, 64, {}, false); // it goes quiet and is unpinned
        reset(a);
        sa.x->select(a.plugin, id, 3);
        idle(b, sb);
        auto x = a.render(frames, 64, at(incoming(3, 11), a.position));
        auto y = b.render(frames, 64, incoming(3, 11));
        check(rms(y, 4800, 24000) > 1e-3 && x == y,
              "after a reset a sound idle at the reset plays as in an instance that never played");
    }
}

// Per-part voice budget entries, as Oscairn sends them for parts sharing an instance (RFC 0021):
// an entry per part's sound with its reserve and priority and no limit of its own, and a limit for
// the instance.
static void testPartBudgets()
{
    auto on = [](uint32_t t, int32_t id, int16_t ch, int16_t key) {
        auto e = noteEvent(t, CLAP_EVENT_NOTE_ON, id, key, 0.8);
        e.note.channel = ch;
        return e;
    };
    // Part 2 (channel 1) plays two notes, then part 1 (channel 0) two, all
    // held; then the limits come, and one voice must go.
    auto list = [](const std::vector<int32_t> &v) {
        std::string t;
        for (auto i : v)
            t += " " + std::to_string(i);
        return t;
    };
    auto budgetRun = [&](uint32_t reserve1, uint8_t priority1, uint8_t priority2) {
        Instance a;
        Sounds s(a);
        char message[128];
        auto second = s.x->prepare(a.plugin, "second", nullptr, 0, nullptr, nullptr, message,
                                   sizeof(message));
        s.x->select(a.plugin, second, 1);
        auto x = static_cast<const oscairn_plugin_voice_budget_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_VOICE_BUDGET));
        const oscairn_voice_budget_context_t entries[] = {{0, 0, reserve1, priority1, 0},
                                                          {second, 0, 0, priority2, 0}};
        a.render(9600, 512,
                 {on(0, 21, 1, 48), on(512, 22, 1, 52), on(1024, 1, 0, 60), on(1536, 2, 0, 64)},
                 false);
        forbidAllocation = true;
        x->set_limits(a.plugin, 3, entries, 2);
        forbidAllocation = false;
        a.render(9600, 512, {}, false);
        std::vector<int32_t> ended;
        for (auto &n : a.out.noteEnds)
            ended.push_back(n.note_id);
        return ended;
    };
    // within its reserve part 1 keeps its notes; part 2, over its reserve (none), gives up its
    // older note although its priority is the higher
    auto ended = budgetRun(2, 0, 10);
    check(ended == std::vector<int32_t>{21},
          "a part within its reserve keeps its notes; the other gives up its oldest:" +
              list(ended));
    // no reserves: the lower priority part's note goes, not the oldest
    ended = budgetRun(0, 0, 10);
    check(ended == std::vector<int32_t>{1},
          "with no reserves the lower priority part's note goes, not the oldest:" + list(ended));
}

// Oscairn parts sharing one instance (RFC 0021): each sound plays on the output port its Output
// Bus names, set on its channel; two parts on one instance render what two instances render; the
// buses are saved; a bus change crossfades; only the main output rings out.
static void testSharing(const std::vector<std::string> &patches)
{
    std::string path;
    for (auto &p : patches)
        if (p.find("Polysynth") != std::string::npos)
            path = p;
    const clap_id busId = fnv1a("oscairn_output_bus");
    auto busEvent = [&](uint32_t t, int16_t ch, double v) {
        auto e = paramEvent(t, busId, v);
        e.value.channel = ch;
        return e;
    };
    auto on = [](uint32_t t, int32_t id, int16_t ch, int16_t key) {
        auto e = noteEvent(t, CLAP_EVENT_NOTE_ON, id, key, 0.8);
        e.note.channel = ch;
        return e;
    };
    auto off = [](uint32_t t, int32_t id, int16_t ch, int16_t key) {
        auto e = noteEvent(t, CLAP_EVENT_NOTE_OFF, id, key, 0);
        e.note.channel = ch;
        return e;
    };
    auto silent = [](const std::vector<float> &v) {
        return std::all_of(v.begin(), v.end(), [](float x) { return x == 0; });
    };
    const uint32_t frames = 48000;
    // part 1: the default sound (a factory patch) on channel 0; part 2: a preset on channel 1,
    // with a pitch bend on its channel only
    const std::vector<Event> part1{on(0, 1, 0, 60), on(4800, 2, 0, 67), off(24000, 1, 0, 60),
                                   off(26000, 2, 0, 67)};
    const std::vector<Event> part2{on(100, 11, 1, 64), midiEvent(9600, 0xE1, 0, 0x60),
                                   off(25000, 11, 1, 64)};
    auto both = [&](uint32_t bus1, uint32_t bus2) {
        auto ev = part1;
        ev.insert(ev.end(), part2.begin(), part2.end());
        ev.push_back(busEvent(0, 0, bus1));
        ev.push_back(busEvent(0, 1, bus2));
        return ev;
    };
    std::vector<float> alone1, alone2;
    {
        Instance b, c;
        b.load(patchFromFxp(patches[0]));
        c.load(patchFromFxp(path));
        alone1 = b.render(frames, 512, part1, false);
        alone2 = c.render(frames, 512, part2, false);
    }
    check(!silent(alone1) && !silent(alone2), "both parts sound alone");
    std::vector<uint8_t> snap;
    {
        Instance a;
        a.outputs = 9;
        Sounds s(a);
        a.load(patchFromFxp(patches[0]));
        s.x->select(a.plugin, s.preset(path), 1);
        auto x = a.render(frames, 512, both(1, 2), false);
        bool rest = silent(a.port[0]);
        for (int k = 3; k < 9; ++k)
            rest = rest && silent(a.port[k]);
        check(a.port[1] == alone1 && a.port[2] == alone2 && rest,
              "two parts on one instance, each its sound, channel and bus, render as two "
              "instances; Main and the other buses are silent");
        double v = -1;
        a.params->get_value(a.plugin, busId, &v);
        check(v == 1, "get_value shows channel 1's sound's bus");
        snap = s.snapshot();
        // the state carries the default sound's bus
        auto state = a.save();
        Instance d;
        d.outputs = 9;
        check(d.load(state), "the state with its bus loads");
        d.params->get_value(d.plugin, busId, &v);
        auto y = d.render(frames, 512, part1, false);
        check(v == 1 && d.port[1] == alone1 && silent(d.port[0]),
              "the state's sound plays on its saved bus");
    }
    {
        Instance a;
        a.outputs = 9;
        Sounds s(a);
        check(s.restore(snap), "the snapshot with buses loads");
        auto ev = part1;
        ev.insert(ev.end(), part2.begin(), part2.end());
        a.render(frames, 512, ev, false);
        check(a.port[1] == alone1 && a.port[2] == alone2 && silent(a.port[0]) &&
                  s.snapshot() == snap,
              "restored, each sound plays on its bus, and saves the same snapshot");
        Instance b;
        b.outputs = 9;
        Sounds t(b);
        uint32_t ids[16];
        check(t.prepareSnapshot(snap, ids), "the snapshot prepared beside");
        for (int16_t ch = 0; ch < 16; ++ch)
            t.x->select(b.plugin, ids[ch], ch);
        b.render(frames, 512, ev, false);
        check(b.port[1] == alone1 && b.port[2] == alone2 && silent(b.port[0]),
              "prepared beside, its sounds play on their buses");
    }
    // a sound saved on its own plays on the main output
    {
        Instance a;
        Sounds s(a);
        auto id = s.preset(path);
        s.x->select(a.plugin, id, 1);
        a.render(512, 512, {busEvent(0, 1, 4)}, false);
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        oscairn_sound_parameter_t values[1024];
        s.x->select(a.plugin, 0, 1);
        a.render(512, 512, {}, false);
        const auto n = snaps->sound_values(a.plugin, id, values, 1024);
        check(n > 0 && values[n - 1].id == busId && values[n - 1].value == 4,
              "a sound's values include its bus");
        std::vector<uint8_t> alone;
        clap_ostream_t os{&alone, [](const clap_ostream_t *st, const void *b, uint64_t k) {
                              auto v = static_cast<std::vector<uint8_t> *>(st->ctx);
                              v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + k);
                              return (int64_t)k;
                          }};
        snaps->save_sound(a.plugin, id, &os);
        Instance b;
        Sounds t(b);
        t.restore(alone);
        check(b.render(frames, 512, part2, false) == alone2,
              "a sound on bus 4 saved on its own plays on the main output");
    }
    // A bus change while a note is held: the sound crossfades to the new bus over 3 ms, no step
    // larger than the plain render's, and the two buses add up to it.
    {
        auto run = [&](bool change) {
            Instance a;
            a.outputs = 9;
            a.load(patchFromFxp(patches[0]));
            std::vector<Event> ev{on(0, 1, 0, 60), busEvent(0, 0, 1)};
            if (change)
                ev.push_back(busEvent(12000, 0, 2));
            a.render(24000, 512, ev, false);
            return std::pair{a.port[1], a.port[2]};
        };
        auto plain = run(false), moved = run(true);
        auto step = [](const std::vector<float> &v) {
            float m = 0;
            for (size_t i = 2; i < v.size(); ++i)
                m = std::max(m, std::fabs(v[i] - v[i - 2]));
            return m;
        };
        float sum = 0;
        for (size_t i = 0; i < plain.first.size(); ++i)
            sum = std::max(sum, std::fabs(moved.first[i] + moved.second[i] - plain.first[i]));
        const bool before = std::equal(moved.first.begin(), moved.first.begin() + 2 * 12000,
                                       plain.first.begin()) &&
                            silent({moved.second.begin(), moved.second.begin() + 2 * 12000});
        const bool after = silent({moved.first.begin() + 2 * (12032 + 160), moved.first.end()});
        check(before && after && sum < 1e-6 && step(moved.first) <= step(plain.first) &&
                  step(moved.second) <= step(plain.first),
              "a bus change crossfades without a click: sum within " + std::to_string(sum) +
                  ", steps " + std::to_string(step(moved.first)) + " and " +
                  std::to_string(step(moved.second)) + " against " +
                  std::to_string(step(plain.first)));
    }
    // Only the main output rings out: a retiring sound on bus 3 plays on there.
    {
        Instance a;
        a.outputs = 9;
        Sounds s(a);
        auto snaps = static_cast<const oscairn_plugin_sound_snapshots_t *>(
            a.plugin->get_extension(a.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        std::vector<float> rb[4];
        float *ch[4];
        for (int k = 0; k < 4; ++k)
        {
            rb[k].assign(512, 0.f);
            ch[k] = rb[k].data();
        }
        clap_audio_buffer_t b{};
        b.data32 = ch;
        b.channel_count = 4;
        float ring = 0;
        a.beforeProcess = [&](uint32_t n) {
            for (auto &v : rb)
            {
                for (uint32_t k = 0; k < n; ++k)
                    ring = std::max(ring, std::fabs(v[k]));
                std::fill(v.begin(), v.end(), 0.f);
            }
            snaps->ring_out(a.plugin, &b);
        };
        a.render(9600, 480, {on(0, 1, 0, 60), busEvent(0, 0, 3)}, false);
        uint32_t ids[16];
        s.prepareSnapshot(snap, ids);
        for (int16_t c = 0; c < 16; ++c)
            s.x->select(a.plugin, ids[c], c);
        a.render(4800, 480, {}, false);
        check(ring == 0 && !silent(a.port[3]), "a retiring sound on bus 3 plays on there");
    }
}

// OSCAIRN_TEST_ONLY and OSCAIRN_TEST_SKIP split the run across CI jobs: each is a list of test
// names separated by '|', and a test runs when ONLY, if set, names it and SKIP does not. A name
// that matches no test fails the run, so a renamed test cannot drop out of a split.
struct TestFilter
{
    std::vector<std::pair<std::string, int>> only, skip; // name, tests matched

    static std::vector<std::pair<std::string, int>> parts(const char *env)
    {
        std::vector<std::pair<std::string, int>> r;
        std::string s = env ? env : "";
        for (size_t b = 0, e; b < s.size(); b = e + 1)
        {
            e = std::min(s.find('|', b), s.size());
            if (e > b)
                r.emplace_back(s.substr(b, e - b), 0);
        }
        return r;
    }
    static bool match(std::vector<std::pair<std::string, int>> &ps, const std::string &name)
    {
        bool any = false;
        for (auto &[part, n] : ps)
            if (name == part)
                any = true, ++n;
        return any;
    }
    bool runs(const std::string &name)
    {
        bool in = match(only, name) || only.empty();
        return match(skip, name) ? false : in;
    }
    void checkAllMatched()
    {
        for (auto *ps : {&only, &skip})
            for (auto &[part, n] : *ps)
                check(n > 0, "test filter name \"" + part + "\" matches a test");
    }
};

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : SURGE_FACTORY_PATCHES;
    static TestFilter filter{TestFilter::parts(std::getenv("OSCAIRN_TEST_ONLY")),
                             TestFilter::parts(std::getenv("OSCAIRN_TEST_SKIP"))};
    try
    {
        auto patches = factoryPatches(dir);
        check(!patches.empty(), "factory patches found in " + dir);
        auto run = [](const char *name, auto &&test) {
            if (!filter.runs(name))
                return;
            std::printf("%s\n", name);
            std::fflush(stdout);
            test();
        };
        run("ports and parameters", [&] { testPortsAndParams(patches); });
        run("rich views", [] { testRichViews(); });
        run("stepped value texts", [] { testSteppedValueTexts(); });
        run("state round trips", [&] { testStateRoundTrip(patches); });
        run("block sizes", [&] { testBlockSizes(patches); });
        run("notes", [] { testNotes(); });
        run("mono key struck again", [] { testMonoKeyStruckAgain(); });
        run("note-on work", [] { testNoteOnWork(); });
        run("formula refused", [] { testFormulaRefused(); });
        run("concurrent state", [] { testConcurrentState(); });
        run("plain patches match stock Surge", [&] { testPlainPatchesMatchStock(patches); });
        run("presets", [&] { testPresets(dir); });
        run("sound contexts", [&] { testSoundContexts(patches); });
        run("note lifetime", [] { testNoteLifetime(); });
        run("voice budget", [] { testVoiceBudget(); });
        run("device voice cap", [] { testDeviceCap(); });
        run("per-part voice budgets", [] { testPartBudgets(); });
        run("sharing", [&] { testSharing(patches); });
        run("reset", [&] { testReset(patches); });
        run("modern oscillator kernels", [] { testModernFastKernels(); });
        run("modern oscillator fast path", [] { testModernFastAgainstSlow(); });
        run("modern oscillator fast path falls back", [] { testModernFastFallsBack(); });
        run("modern oscillator fast path mid-note", [] { testModernFastMidNote(); });
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
        // The same checks at other rates (the reference was measured at 44.1 kHz; the cutoff
        // rate match is fitted at 48, 96 and 192 kHz and interpolated at 88.2 and 176.4),
        // in the standard build only: they check numbers, not memory or threads, and would
        // add most of an hour to the TSan run. The cutoff tables hold there through the
        // filter cutoff rate match.
        if (!sanitizerBuild)
        {
            for (double r : {48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
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
    filter.checkAllMatched();
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

