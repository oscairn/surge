// SPDX-License-Identifier: MIT
// Drives a CLAP plugin as Oscairn opens a Performance with it, for privacy-check.sh, which runs
// it with a temporary HOME and fails if anything touches the user's protected folders:
// instance creation, init and activate, preset discovery (each declared location listed, as
// the host's scanner does, and metadata read), a preset loaded, a note, the state saved and
// loaded into a fresh instance (and a given state file), and a note there.
//
//   privacy-host <plugin.clap> <plugin id> [--library <id>=<folder>]... [--state <file>]
//
// --library answers oscairn.assets library_root for that library ID, as an installed asset
// library. Exits non-zero when a step fails.
#include <clap/clap.h>
#include <oscairn-clap/assets.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::map<std::string, std::string> libraries;
static int failures = 0;
static void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
    failures += !ok;
}

static bool libraryRoot(const void *, const char *id, const char *, char *path, uint32_t cap)
{
    auto i = libraries.find(id ? id : "");
    if (i == libraries.end() || i->second.size() + 1 > cap)
        return false;
    std::snprintf(path, cap, "%s", i->second.c_str());
    return true;
}
static bool resolve(const clap_host_t *, oscairn_asset_ref_t *ref, char *path, uint32_t cap,
                    char *msg, uint32_t msgCap)
{
    auto i = libraries.find(ref->library);
    if (i == libraries.end() || i->second.size() + std::strlen(ref->path) + 2 > cap)
    {
        std::snprintf(msg, msgCap, "not installed");
        return false;
    }
    std::snprintf(path, cap, "%s/%s", i->second.c_str(), ref->path);
    return true;
}
static bool createAsset(const clap_host_t *, const char *, oscairn_asset_ref_t *, char *msg,
                        uint32_t msgCap)
{
    std::snprintf(msg, msgCap, "not offered by this host");
    return false;
}
static const oscairn_host_assets_t assets{resolve, libraryRoot, createAsset};
static const oscairn_host_assets_t indexerAssets{nullptr, libraryRoot, nullptr};

static const clap_host_log_t hostLog{[](const clap_host_t *, clap_log_severity s, const char *m) {
    std::printf("plugin log %d: %s\n", (int)s, m);
}};
static const clap_host_thread_check_t threadCheck{[](const clap_host_t *) { return true; },
                                                  [](const clap_host_t *) { return false; }};
static const clap_host_params_t hostParams{
    [](const clap_host_t *, clap_param_rescan_flags) {},
    [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t *) {}};
static std::string presetError;
static const clap_host_preset_load_t hostPresetLoad{
    [](const clap_host_t *, uint32_t, const char *, const char *, int32_t, const char *m) {
        presetError = m ? m : "error";
    },
    [](const clap_host_t *, uint32_t, const char *, const char *) {}};

static const clap_host_t host{
    CLAP_VERSION_INIT, nullptr, "privacy-host", "Oscairn", "", "1",
    [](const clap_host_t *, const char *id) -> const void * {
        if (!std::strcmp(id, OSCAIRN_EXT_ASSETS) || !std::strcmp(id, OSCAIRN_EXT_ASSETS_DRAFT_2) ||
            !std::strcmp(id, OSCAIRN_EXT_ASSETS_DRAFT_1))
            return &assets;
        if (!std::strcmp(id, CLAP_EXT_LOG))
            return &hostLog;
        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK))
            return &threadCheck;
        if (!std::strcmp(id, CLAP_EXT_PARAMS))
            return &hostParams;
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
            return &hostPresetLoad;
        return nullptr;
    },
    [](const clap_host_t *) {}, [](const clap_host_t *) {}, [](const clap_host_t *) {}};

struct Events
{
    std::vector<clap_event_note_t> notes;
};
static uint32_t evSize(const clap_input_events_t *l)
{
    return (uint32_t) static_cast<Events *>(l->ctx)->notes.size();
}
static const clap_event_header_t *evGet(const clap_input_events_t *l, uint32_t i)
{
    return &static_cast<Events *>(l->ctx)->notes[i].header;
}
static bool outPush(const clap_output_events_t *, const clap_event_header_t *) { return true; }

// Renders a note: on in the first block, off halfway; true when every sample is finite.
static bool playNote(const clap_plugin_t *p)
{
    constexpr uint32_t frames = 256, blocks = 40;
    auto ports = static_cast<const clap_plugin_audio_ports_t *>(
        p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
    std::vector<std::vector<float>> data;
    std::vector<std::vector<float *>> ptrs[2];
    std::vector<clap_audio_buffer_t> bufs[2];
    for (int dir = 0; dir < 2; ++dir)
    {
        uint32_t n = ports ? ports->count(p, dir == 0) : (dir == 0 ? 0 : 1);
        ptrs[dir].resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            clap_audio_port_info_t info{};
            uint32_t ch = 2;
            if (ports && ports->get(p, i, dir == 0, &info))
                ch = info.channel_count;
            for (uint32_t c = 0; c < ch; ++c)
            {
                data.emplace_back(frames, 0.f);
                ptrs[dir][i].push_back(nullptr);
            }
        }
    }
    size_t k = 0;
    for (int dir = 0; dir < 2; ++dir)
        for (auto &port : ptrs[dir])
        {
            for (auto &c : port)
                c = data[k++].data();
            bufs[dir].push_back({port.data(), nullptr, (uint32_t)port.size(), 0, 0});
        }
    if (!p->start_processing(p))
        return false;
    bool finite = true;
    for (uint32_t b = 0; b < blocks; ++b)
    {
        Events ev;
        if (b == 0 || b == blocks / 2)
        {
            clap_event_note_t n{};
            n.header = {sizeof(n), 0, CLAP_CORE_EVENT_SPACE_ID,
                        (uint16_t)(b == 0 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            n.note_id = -1;
            n.port_index = 0;
            n.channel = 0;
            n.key = 60;
            n.velocity = 0.8;
            ev.notes.push_back(n);
        }
        clap_input_events_t in{&ev, evSize, evGet};
        clap_output_events_t out{nullptr, outPush};
        clap_process_t pr{};
        pr.steady_time = (int64_t)b * frames;
        pr.frames_count = frames;
        pr.audio_inputs = bufs[0].data();
        pr.audio_inputs_count = (uint32_t)bufs[0].size();
        pr.audio_outputs = bufs[1].data();
        pr.audio_outputs_count = (uint32_t)bufs[1].size();
        pr.in_events = &in;
        pr.out_events = &out;
        if (p->process(p, &pr) == CLAP_PROCESS_ERROR)
            return false;
        for (auto &port : ptrs[1])
            for (auto *c : port)
                for (uint32_t i = 0; i < frames; ++i)
                    finite = finite && std::isfinite(c[i]);
    }
    p->stop_processing(p);
    return finite;
}

static std::vector<uint8_t> saveState(const clap_plugin_t *p)
{
    std::vector<uint8_t> bytes;
    auto st = static_cast<const clap_plugin_state_t *>(p->get_extension(p, CLAP_EXT_STATE));
    clap_ostream_t os{&bytes, [](const clap_ostream_t *s, const void *b, uint64_t n) -> int64_t {
                          auto v = static_cast<std::vector<uint8_t> *>(s->ctx);
                          v->insert(v->end(), (const uint8_t *)b, (const uint8_t *)b + n);
                          return (int64_t)n;
                      }};
    if (!st || !st->save(p, &os))
        bytes.clear();
    return bytes;
}
static bool loadState(const clap_plugin_t *p, const std::vector<uint8_t> &bytes)
{
    struct In
    {
        const std::vector<uint8_t> *b;
        size_t pos;
    } in{&bytes, 0};
    clap_istream_t is{&in, [](const clap_istream_t *s, void *b, uint64_t n) -> int64_t {
                          auto x = static_cast<In *>(s->ctx);
                          n = std::min<uint64_t>(n, x->b->size() - x->pos);
                          std::memcpy(b, x->b->data() + x->pos, n);
                          x->pos += n;
                          return (int64_t)n;
                      }};
    auto st = static_cast<const clap_plugin_state_t *>(p->get_extension(p, CLAP_EXT_STATE));
    return st && st->load(p, &is);
}

struct Preset
{
    uint32_t kind;
    std::string location, key;
};
struct Indexer
{
    std::vector<std::string> extensions;
    std::vector<std::pair<uint32_t, std::string>> locations;
};
struct Receiver
{
    const char *pluginId;
    std::vector<std::string> keys;
    bool forPlugin{false};
};

// Lists every preset the plugin's providers declare, as the host's scanner does.
static std::vector<Preset> discoverPresets(const clap_plugin_entry_t *entry, const char *pluginId)
{
    std::vector<Preset> found;
    auto f = static_cast<const clap_preset_discovery_factory_t *>(
        entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!f)
        f = static_cast<const clap_preset_discovery_factory_t *>(
            entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    if (!f)
    {
        std::printf("no preset discovery\n");
        return found;
    }
    for (uint32_t i = 0; i < f->count(f); ++i)
    {
        Indexer ix;
        clap_preset_discovery_indexer_t indexer{
            CLAP_VERSION_INIT, "privacy-host", "Oscairn", "", "1", &ix,
            [](const clap_preset_discovery_indexer_t *i,
               const clap_preset_discovery_filetype_t *t) {
                static_cast<Indexer *>(i->indexer_data)
                    ->extensions.push_back(t->file_extension ? t->file_extension : "");
                return true;
            },
            [](const clap_preset_discovery_indexer_t *i,
               const clap_preset_discovery_location_t *l) {
                static_cast<Indexer *>(i->indexer_data)
                    ->locations.emplace_back(l->kind, l->location ? l->location : "");
                return true;
            },
            [](const clap_preset_discovery_indexer_t *,
               const clap_preset_discovery_soundpack_t *) { return true; },
            [](const clap_preset_discovery_indexer_t *, const char *id) -> const void * {
                return !std::strcmp(id, OSCAIRN_EXT_ASSETS) ||
                               !std::strcmp(id, OSCAIRN_EXT_ASSETS_DRAFT_2)
                           ? &indexerAssets
                           : nullptr;
            }};
        auto pr = f->create(f, &indexer, f->get_descriptor(f, i)->id);
        check(pr && pr->init(pr), std::string("preset provider ") + f->get_descriptor(f, i)->id +
                                      " initializes");
        if (!pr)
            continue;
        auto metadata = [&](uint32_t kind, const std::string &loc) {
            Receiver r{pluginId};
            clap_preset_discovery_metadata_receiver_t m{};
            m.receiver_data = &r;
            m.on_error = [](const clap_preset_discovery_metadata_receiver_t *, int32_t,
                            const char *) {};
            m.begin_preset = [](const clap_preset_discovery_metadata_receiver_t *m,
                                const char *, const char *key) {
                static_cast<Receiver *>(m->receiver_data)->keys.push_back(key ? key : "");
                return true;
            };
            m.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t *m,
                                 const clap_universal_plugin_id_t *id) {
                auto r = static_cast<Receiver *>(m->receiver_data);
                r->forPlugin = r->forPlugin || !std::strcmp(id->id, r->pluginId);
            };
            m.set_soundpack_id = [](auto *, auto *) {};
            m.set_flags = [](auto *, uint32_t) {};
            m.add_creator = [](auto *, auto *) {};
            m.set_description = [](auto *, auto *) {};
            m.set_timestamps = [](auto *, clap_timestamp, clap_timestamp) {};
            m.add_feature = [](auto *, auto *) {};
            m.add_extra_info = [](auto *, auto *, auto *) {};
            pr->get_metadata(pr, kind, kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? loc.c_str()
                                                                                    : nullptr,
                             &m);
            if (r.forPlugin)
                for (auto &k : r.keys)
                    found.push_back({kind, loc, k});
        };
        for (auto &[kind, loc] : ix.locations)
        {
            std::printf("location: %s\n", loc.empty() ? "(the plugin)" : loc.c_str());
            if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE)
            {
                metadata(kind, loc);
                continue;
            }
            std::error_code ec;
            if (!fs::is_directory(loc, ec))
            {
                metadata(kind, loc);
                continue;
            }
            int n = 0;
            for (auto it = fs::recursive_directory_iterator(loc, ec);
                 !ec && it != fs::recursive_directory_iterator() && n < 50; it.increment(ec))
            {
                auto ext = it->path().extension().string();
                bool wanted = ix.extensions.empty();
                for (auto &e : ix.extensions)
                    wanted = wanted || ext == "." + e;
                if (it->is_regular_file(ec) && wanted)
                {
                    ++n;
                    metadata(kind, it->path().string());
                }
            }
        }
        pr->destroy(pr);
    }
    std::printf("presets found: %zu\n", found.size());
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s plugin.clap plugin-id [--library id=folder]... "
                             "[--state file]\n",
                     argv[0]);
        return 2;
    }
    std::string bundle = argv[1], id = argv[2], stateFile;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--library" && v.find('=') != std::string::npos)
            libraries[v.substr(0, v.find('='))] = v.substr(v.find('=') + 1);
        else if (a == "--state")
            stateFile = v;
        else
            return 2;
    }
    auto binary = bundle;
    if (fs::is_directory(bundle)) // a macOS bundle
        for (auto &e : fs::directory_iterator(fs::path(bundle) / "Contents" / "MacOS"))
            binary = e.path().string();
    auto lib = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto entry = lib ? static_cast<const clap_plugin_entry_t *>(dlsym(lib, "clap_entry")) : nullptr;
    if (!entry || !entry->init(bundle.c_str()))
    {
        std::printf("FAILED: cannot load %s: %s\n", bundle.c_str(), lib ? "" : dlerror());
        return 1;
    }
    auto factory =
        static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    auto make = [&]() -> const clap_plugin_t * {
        auto p = factory ? factory->create_plugin(factory, &host, id.c_str()) : nullptr;
        if (p && !p->init(p))
        {
            p->destroy(p);
            p = nullptr;
        }
        return p;
    };

    auto a = make();
    check(a != nullptr, "instance created and initialized");
    if (!a)
        return 1;
    check(a->activate(a, 48000, 1, 4096), "activated");
    check(playNote(a), "plays a note");

    auto presets = discoverPresets(entry, id.c_str());
    auto load = static_cast<const clap_plugin_preset_load_t *>(
        a->get_extension(a, CLAP_EXT_PRESET_LOAD));
    if (!load)
        load = static_cast<const clap_plugin_preset_load_t *>(
            a->get_extension(a, CLAP_EXT_PRESET_LOAD_COMPAT));
    if (load && !presets.empty())
    {
        auto &pr = presets.front();
        check(load->from_location(a, pr.kind,
                                  pr.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE
                                      ? pr.location.c_str()
                                      : nullptr,
                                  pr.key.empty() ? nullptr : pr.key.c_str()) &&
                  presetError.empty(),
              "loads the preset " + pr.location + "#" + pr.key + presetError);
        check(playNote(a), "plays a note in the preset");
    }
    auto state = saveState(a);
    check(!state.empty(), "saves its state");
    a->deactivate(a);
    a->destroy(a);

    std::vector<std::vector<uint8_t>> states{state};
    if (!stateFile.empty())
    {
        std::ifstream f(stateFile, std::ios::binary);
        states.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    for (auto &s : states)
    {
        auto b = make();
        check(b && loadState(b, s), "a fresh instance loads the state");
        if (!b)
            continue;
        check(b->activate(b, 48000, 1, 4096), "activated");
        check(playNote(b), "plays a note");
        b->deactivate(b);
        b->destroy(b);
    }
    entry->deinit();
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
