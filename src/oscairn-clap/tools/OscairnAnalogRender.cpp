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
 * is in Surge's own units (the plugin's parameter units). mod:source:name=depth adds a
 * modulation routing from Surge modulation source number `source` (enum modsources) with
 * depth in Surge's 0..1 units. The patch is built with Surge's API, saved, and loaded into
 * the plugin as its state, so the render goes through the plugin's own path. Writes a
 * stereo 32-bit float WAV.
 */

#include "OscairnAnalog.h"
#include "SurgeSynthesizer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Layer : SurgeSynthesizer::PluginLayer
{
    void surgeParameterUpdated(const SurgeSynthesizer::ID &, float) override {}
    void surgeMacroUpdated(long, float) override {}
};

std::unique_ptr<SurgeSynthesizer> makeSynth(Layer &layer)
{
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    return std::make_unique<SurgeSynthesizer>(&layer, config);
}

Parameter *findParam(SurgeSynthesizer &s, const std::string &name)
{
    for (auto *p : s.storage.getPatch().param_ptr)
        if (name == p->get_storage_name())
            return p;
    throw std::runtime_error("no parameter named " + name);
}

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

const clap_host_t host = {
    CLAP_VERSION_INIT,
    nullptr,
    "oscairn-analog-render",
    "Oscairn",
    "",
    "0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};
} // namespace

int main(int argc, char **argv)
{
    try
    {
        Layer layer;
        auto synth = makeSynth(layer);
        if (argc == 2 && !std::strcmp(argv[1], "--list"))
        {
            for (auto *p : synth->storage.getPatch().param_ptr)
                if (p->scene <= 1 && p->ctrltype != ct_none)
                    std::printf("%s\t%s\t%s\n", p->get_storage_name(), p->get_full_name(),
                                p->get_display().c_str());
            return 0;
        }
        if (argc < 2)
        {
            std::fprintf(stderr, "usage: oscairn-analog-render out.wav [options] [name=value] "
                                 "[mod:source:name=depth] | --list\n");
            return 2;
        }

        std::string out = argv[1];
        int note = 60, vel = 100;
        double hold = 1, tail = 1, rate = 48000;
        for (int i = 2; i < argc; ++i)
        {
            std::string a = argv[i];
            auto next = [&]() -> double {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value for " + a);
                return std::atof(argv[++i]);
            };
            if (a == "--note")
                note = (int)next();
            else if (a == "--vel")
                vel = (int)next();
            else if (a == "--hold")
                hold = next();
            else if (a == "--tail")
                tail = next();
            else if (a == "--rate")
                rate = next();
            else if (auto eq = a.find('='); eq != std::string::npos)
            {
                auto lhs = a.substr(0, eq);
                double v = std::atof(a.c_str() + eq + 1);
                if (lhs.rfind("mod:", 0) == 0)
                {
                    auto c = lhs.find(':', 4);
                    int src = std::atoi(lhs.substr(4, c - 4).c_str());
                    auto *p = findParam(*synth, lhs.substr(c + 1));
                    synth->setModDepth01(p->id, (modsources)src, 0, 0, (float)v);
                }
                else
                {
                    auto *p = findParam(*synth, lhs);
                    pdata d = p->val;
                    if (p->valtype == vt_float)
                        d.f = (float)v;
                    else if (p->valtype == vt_int)
                        d.i = (int)std::lround(v);
                    else
                        d.b = v > 0.5;
                    // Through setParameter01, so type changes (oscillators) take effect.
                    Parameter tmp = *p;
                    tmp.val = d;
                    float f01 = p->valtype == vt_int ? Parameter::intScaledToFloat(
                                                           d.i, p->val_max.i, p->val_min.i)
                                                     : tmp.get_value_f01();
                    synth->setParameter01(synth->idForParameter(p), f01, true);
                    synth->process(); // applies queued oscillator type changes
                    if (p->valtype == vt_float)
                        p->val.f = (float)v; // exact value, not via 0..1
                }
            }
            else
                throw std::runtime_error("unknown argument " + a);
        }
        void *data = nullptr;
        auto size = synth->saveRaw(&data);
        std::vector<uint8_t> state((uint8_t *)data, (uint8_t *)data + size);

        auto factory = static_cast<const clap_plugin_factory_t *>(
            oscairn_analog_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        auto plugin = factory->create_plugin(factory, &host, "org.oscairn.analog");
        if (!plugin || !plugin->init(plugin))
            throw std::runtime_error("plugin creation failed");
        auto stateExt =
            static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        struct Reader
        {
            std::vector<uint8_t> *b;
            size_t at;
        } r{&state, 0};
        clap_istream_t is{&r, [](const clap_istream_t *s, void *b, uint64_t n) {
                              auto rd = static_cast<Reader *>(s->ctx);
                              n = std::min<uint64_t>(n, rd->b->size() - rd->at);
                              std::memcpy(b, rd->b->data() + rd->at, n);
                              rd->at += n;
                              return (int64_t)n;
                          }};
        if (!plugin->activate(plugin, rate, 1, 512) || !plugin->start_processing(plugin))
            throw std::runtime_error("activation failed");
        if (!stateExt->load(plugin, &is))
            throw std::runtime_error("the plugin refused the patch");

        const uint32_t total = (uint32_t)std::lround((hold + tail) * rate),
                       off = (uint32_t)std::lround(hold * rate);
        clap_event_note_t on{}, offEv{};
        on.header = {sizeof(on), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
        on.note_id = 1;
        on.channel = 0;
        on.key = (int16_t)note;
        on.velocity = vel / 127.0;
        offEv = on;
        offEv.header.type = CLAP_EVENT_NOTE_OFF;
        std::vector<float> lr;
        lr.reserve(total * 2);
        float left[512], right[512];
        for (uint32_t pos = 0; pos < total; pos += 512)
        {
            uint32_t n = std::min<uint32_t>(512, total - pos);
            std::vector<const clap_event_header_t *> evs;
            if (pos == 0)
                evs.push_back(&on.header);
            if (off >= pos && off < pos + n)
            {
                offEv.header.time = off - pos;
                evs.push_back(&offEv.header);
            }
            struct List
            {
                std::vector<const clap_event_header_t *> *e;
            } list{&evs};
            clap_input_events_t in{&list,
                                   [](const clap_input_events_t *l) {
                                       return (uint32_t) static_cast<List *>(l->ctx)->e->size();
                                   },
                                   [](const clap_input_events_t *l, uint32_t i) {
                                       return (*static_cast<List *>(l->ctx)->e)[i];
                                   }};
            clap_output_events_t outEv{nullptr, [](const clap_output_events_t *,
                                                   const clap_event_header_t *) { return true; }};
            float *ch[2] = {left, right};
            clap_audio_buffer_t buf{};
            buf.data32 = ch;
            buf.channel_count = 2;
            clap_process_t pr{};
            pr.frames_count = n;
            pr.audio_outputs = &buf;
            pr.audio_outputs_count = 1;
            pr.in_events = &in;
            pr.out_events = &outEv;
            plugin->process(plugin, &pr);
            for (uint32_t k = 0; k < n; ++k)
            {
                lr.push_back(left[k]);
                lr.push_back(right[k]);
            }
        }
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        writeWav(out, lr, (int)rate);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
