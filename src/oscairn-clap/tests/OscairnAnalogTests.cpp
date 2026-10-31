/*
 * Tests for the Oscairn Analog CLAP instrument (doc/oscairn-plan.md, milestone 0).
 * GPL-3.0-or-later, see ../OscairnAnalog.cpp.
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
 *   - state loads and saves on one thread while another renders (run under TSan).
 *
 * Usage: oscairn-analog-tests [factory patch directory]
 */

#include "OscairnAnalog.h"
#include "SurgeSynthesizer.h"

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
    int loaded = 0;
    for (auto &path : patches)
    {
        auto patch = patchFromFxp(path);
        check(!patch.empty(), "factory patch has a sub3 chunk: " + path);
        if (patch.empty())
            continue;
        bool ok = a.load(patch);
        reference.loadRaw(patch.data(), (int)patch.size(), false);
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
}

static void testStateRoundTrip(const std::vector<std::string> &patches)
{
    Instance a;
    auto first = a.save();
    check(a.load(first), "load own state");
    check(a.save() == first, "default state round trip is byte-identical");
    Instance b;
    check(b.save() == first, "two fresh instances save the same bytes");

    // Loading an older patch migrates it, and the first save after that can differ from the
    // next one (Surge's migrations aren't idempotent for some revision-9/10 patches). The
    // plugin's own state must be a fixed point: save -> load -> save gives the same bytes.
    int tested = 0, migrated = 0;
    for (auto &path : patches)
    {
        auto patch = patchFromFxp(path);
        if (!a.load(patch))
            continue;
        auto s1 = a.save();
        check(b.load(s1), "load saved state: " + path);
        auto s2 = b.save();
        Instance c;
        check(c.load(s2), "load saved state again: " + path);
        check(c.save() == s2, "state round trip is byte-identical: " + path);
        migrated += s1 != s2;
        ++tested;
    }
    std::printf("state round trips: %d (first save after migration differed for %d)\n", tested,
                migrated);
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

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : SURGE_FACTORY_PATCHES;
    try
    {
        auto patches = factoryPatches(dir);
        check(!patches.empty(), "factory patches found in " + dir);
        testPortsAndParams(patches);
        testStateRoundTrip(patches);
        testBlockSizes(patches);
        testNotes();
        testFormulaRefused();
        testConcurrentState();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
