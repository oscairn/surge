/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors. This file is MIT-licensed; programs using it link
 * Surge XT and are GPL-3.0-or-later as a whole.
 */
#include "RenderNote.h"

#include "OscairnAnalog.h"
#include "SurgeSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

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

const clap_host_t host = {
    CLAP_VERSION_INIT,
    nullptr,
    "oscairn-analog-render-note",
    "Oscairn",
    "",
    "0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

void setParam(SurgeSynthesizer &synth, const std::string &name, double v)
{
    // name.off=1 deactivates a parameter that can be switched off (a_lowcut.off=1)
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".off") == 0)
    {
        findParam(synth, name.substr(0, name.size() - 4))->deactivated = v > 0.5;
        return;
    }
    // name.deform=n sets a parameter's deform type (an oscillator control's variant)
    if (name.size() > 7 && name.compare(name.size() - 7, 7, ".deform") == 0)
    {
        findParam(synth, name.substr(0, name.size() - 7))->deform_type = (int)std::lround(v);
        return;
    }
    auto *p = findParam(synth, name);
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
    float f01 = p->valtype == vt_int
                    ? Parameter::intScaledToFloat(d.i, p->val_max.i, p->val_min.i)
                    : tmp.get_value_f01();
    synth.setParameter01(synth.idForParameter(p), f01, true);
    synth.process(); // applies queued oscillator type changes
    if (p->valtype == vt_float)
        p->val.f = (float)v; // exact value, not via 0..1
}
} // namespace

namespace oscairn_analog
{
std::vector<std::string> listParameters()
{
    Layer layer;
    auto synth = makeSynth(layer);
    std::vector<std::string> out;
    for (auto *p : synth->storage.getPatch().param_ptr)
        if (p->scene <= 1 && p->ctrltype != ct_none)
            out.push_back(std::string(p->get_storage_name()) + "\t" + p->get_full_name() + "\t" +
                          p->get_display());
    return out;
}

std::vector<float> renderNote(const RenderSpec &spec)
{
    Layer layer;
    auto synth = makeSynth(layer);
    const int note = spec.note, vel = spec.velocity;
    const double hold = spec.hold, tail = spec.tail, rate = spec.rate;
    for (auto &[name, v] : spec.params)
        setParam(*synth, name, v);
    for (auto &m : spec.mods)
        synth->setModDepth01(findParam(*synth, m.target)->id, (modsources)m.source, 0, 0,
                             (float)m.depth);
    synth->storage.analogEnvelopeAttackClamp = spec.referenceMatching;
    synth->storage.vcaVelocityAmplitudeLaw = spec.referenceMatching;
    synth->storage.saturatorAlgebraicCurve = spec.referenceMatching;
    synth->storage.lfoLongRelease = spec.referenceMatching;
    synth->storage.filterCutoffRateMatch = spec.referenceMatching;
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

    const uint32_t total = (uint32_t)std::lround((hold + tail) * rate);
    auto at = [&](double t) { return (uint32_t)std::lround(t * rate); };
    auto noteEv = [&](uint32_t time, uint16_t type, int32_t id, int key) {
        clap_event_note_t e{};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
        e.note_id = id;
        e.channel = 0;
        e.key = (int16_t)key;
        e.velocity = vel / 127.0;
        return e;
    };
    std::vector<clap_event_note_t> notes; // in time order
    notes.push_back(noteEv(0, CLAP_EVENT_NOTE_ON, 1, note));
    if (spec.legatoNote >= 0 && spec.legatoAt + 0.1 < hold)
    {
        notes.push_back(noteEv(at(spec.legatoAt), CLAP_EVENT_NOTE_ON, 2, spec.legatoNote));
        notes.push_back(noteEv(at(spec.legatoAt + 0.1), CLAP_EVENT_NOTE_OFF, 1, note));
        notes.push_back(noteEv(at(hold), CLAP_EVENT_NOTE_OFF, 2, spec.legatoNote));
    }
    else
        notes.push_back(noteEv(at(hold), CLAP_EVENT_NOTE_OFF, 1, note));
    std::vector<float> lr;
    lr.reserve(total * 2);
    float left[512], right[512];
    for (uint32_t pos = 0; pos < total; pos += 512)
    {
        uint32_t n = std::min<uint32_t>(512, total - pos);
        std::vector<const clap_event_header_t *> evs;
        for (auto &e : notes)
            if (e.header.time >= pos && e.header.time < pos + n)
            {
                e.header.time -= pos; // each event is in one block only
                evs.push_back(&e.header);
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
    return lr;
}
} // namespace oscairn_analog
