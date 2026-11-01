/*
 * Oscairn Analog: Surge XT's engine as a CLAP instrument for the Oscairn host.
 *
 * Copyright 2026, Oscairn contributors, and the Surge XT authors as described in the
 * GitHub transaction log of https://github.com/surge-synthesizer/surge.
 *
 * Released under the GNU General Public Licence v3 or later (GPL-3.0-or-later), like
 * Surge XT. See the LICENSE file in the root of this repository.
 *
 * What differs from the Surge XT plugin (doc/oscairn-plan.md, section 4):
 * - Written against CLAP directly, no JUCE: one stereo output, no input, one note port.
 * - One scene (scene A), no effects. Only scene A and a few global parameters are
 *   exposed; the effect slots are disabled.
 * - Parameters carry Surge's own values and ranges, real defaults and stepped flags.
 *   Parameters whose range or meaning follows another parameter (oscillator controls,
 *   filter 2 cutoff, filter subtypes) are exposed as 0..1, so metadata never depends on
 *   the loaded state.
 * - Rendering is reproducible: the RNG is reseeded on activation, events take effect at
 *   the first internal block boundary at or after their time (counted from activation, so
 *   output doesn't depend on the host's block size).
 * - A CLAP note-on with velocity 0 stays a note-on.
 * - State is loaded on the main thread. While it loads, process() outputs silence instead
 *   of touching the engine, and never waits.
 * - No Formula modulators: patches using them are refused.
 */

#include "OscairnAnalog.h"

#include "SurgeSynthesizer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "org.oscairn.analog",
    "Oscairn Analog",
    "Oscairn",
    "https://github.com/oscairn/surge",
    "",
    "",
    "0.1.0",
    "Analog-modelling instrument based on Surge XT",
    features,
};

constexpr uint32_t renderSeed = 0x05CA1A4E;
constexpr float longestEnvelopeTime = 8; // log2 seconds; Surge's own envelopes stop at 5

struct HeadlessLayer : SurgeSynthesizer::PluginLayer
{
    void surgeParameterUpdated(const SurgeSynthesizer::ID &, float) override {}
    void surgeMacroUpdated(long, float) override {}
};

struct ParamInfo
{
    Parameter *p{nullptr};
    clap_id id{0};
    bool normalized{false}; // exposed as Surge's 0..1 value instead of its own units
    bool deactivation{false}; // the parameter's off switch (1 = off), not its value
    double min{0}, max{1}, def{0};
    uint32_t flags{0};
    std::string name, module;
};

uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

std::string moduleFor(const Parameter &p)
{
    auto n = std::to_string(p.ctrlgroup_entry + 1);
    switch (p.ctrlgroup)
    {
    case cg_OSC:
        return "Oscillator " + n;
    case cg_MIX:
        return "Mixer";
    case cg_FILTER:
        return p.ctrlgroup_entry < n_filterunits_per_scene ? "Filter " + n : "Filter";
    case cg_ENV:
        return p.ctrlgroup_entry == 0 ? "Amp Envelope" : "Filter Envelope";
    case cg_LFO:
    {
        // an LFO's control group entry is its modulation source
        int l = p.ctrlgroup_entry - ms_lfo1;
        return l < n_lfos_voice ? "Voice LFO " + std::to_string(l + 1)
                                : "Scene LFO " + std::to_string(l - n_lfos_voice + 1);
    }
    default:
        return p.scene == 0 ? "Global" : "Scene";
    }
}

struct Plugin
{
    clap_plugin_t plugin{};
    const clap_host_t *host{nullptr};
    HeadlessLayer layer;
    std::unique_ptr<SurgeSynthesizer> synth;

    std::vector<ParamInfo> params;
    std::unordered_map<clap_id, uint32_t> indexById;
    std::unique_ptr<std::atomic<double>[]> values; // what get_value reports, for the main thread

    // Main thread sets loading, then waits until the audio thread is out of process().
    // process() marks itself inside, then backs out if a load is pending. Both use seq_cst,
    // so at least one of them sees the other.
    // A call that backs out sets backedOut: process() calls never overlap, so every later call
    // backs out too and the main thread needn't catch inProcess false between back-to-back calls.
    std::atomic<bool> loading{false}, inProcess{false}, backedOut{false};
    // Odd while the audio thread applies parameter changes; save checks it didn't change.
    std::atomic<uint32_t> paramEpoch{0};

    int blockPos{BLOCK_SIZE}; // read position in synth->output; BLOCK_SIZE = render next
    double sampleRate{48000};

    Plugin(const clap_host_t *h) : host(h)
    {
        plugin.desc = &descriptor;
        plugin.plugin_data = this;
    }

    // --- parameters ---------------------------------------------------------------

    static bool exposed(const Parameter &p)
    {
        if (p.ctrlgroup == cg_FX || p.ctrltype == ct_none)
            return false;
        auto sn = std::string(p.get_storage_name());
        if (p.scene == 0)
            return sn == "volume" || sn == "polylimit" || sn == "character";
        if (p.scene != 1)
            return false;
        // Effect sends belong to the effect slots this build doesn't have.
        return sn.find("send_fx_") == std::string::npos;
    }

    bool stateDependent(const Parameter &p) const
    {
        auto &sc = synth->storage.getPatch().scene[0];
        if (p.ctrlgroup == cg_OSC)
            for (auto &o : sc.osc)
                for (auto &op : o.p)
                    if (&op == &p)
                        return true;
        for (auto &f : sc.filterunit)
            if (&f.subtype == &p)
                return true;
        return &p == &sc.filterunit[1].cutoff;
    }

    void buildParams()
    {
        for (auto *p : synth->storage.getPatch().param_ptr)
        {
            if (!exposed(*p))
                continue;
            ParamInfo pi;
            pi.p = p;
            pi.id = fnv1a(p->get_storage_name());
            pi.normalized = stateDependent(*p);
            pi.module = moduleFor(*p);
            pi.name = p->get_name();
            pi.flags = CLAP_PARAM_IS_AUTOMATABLE;
            if (pi.normalized)
            {
                pi.min = 0;
                pi.max = 1;
                pi.def = p->get_default_value_f01();
                if (p->ctrlgroup == cg_OSC)
                    pi.name = "Control " + std::to_string((p - &synth->storage.getPatch()
                                                                     .scene[0]
                                                                     .osc[p->ctrlgroup_entry]
                                                                     .p[0]) +
                                                          1);
            }
            else if (p->valtype == vt_float)
            {
                pi.min = p->val_min.f;
                pi.max = p->val_max.f;
                pi.def = p->val_default.f;
            }
            else if (p->valtype == vt_int)
            {
                pi.min = p->val_min.i;
                pi.max = p->val_max.i;
                pi.def = p->val_default.i;
                pi.flags |= CLAP_PARAM_IS_STEPPED;
            }
            else
            {
                pi.min = 0;
                pi.max = 1;
                pi.def = p->val_default.b ? 1 : 0;
                pi.flags |= CLAP_PARAM_IS_STEPPED;
            }
            if (!pi.normalized && p->valtype == vt_float && p->can_be_nondestructively_modulated())
            {
                pi.flags |= CLAP_PARAM_IS_MODULATABLE;
                if (p->per_voice_processing)
                    pi.flags |= CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID |
                                CLAP_PARAM_IS_MODULATABLE_PER_KEY |
                                CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL;
            }
            pi.def = std::clamp(pi.def, pi.min, pi.max);
            indexById[pi.id] = (uint32_t)params.size();
            params.push_back(pi);
            if (p->can_deactivate())
            {
                // Its off switch (the saturators, the filter types, the scene highpass ...),
                // with the ID of "<storage name>.off" and the init patch's state as default
                ParamInfo off;
                off.p = p;
                off.deactivation = true;
                off.id = fnv1a((std::string(p->get_storage_name()) + ".off").c_str());
                off.module = pi.module;
                off.name = pi.name + " Off";
                off.def = p->deactivated ? 1 : 0;
                off.flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
                indexById[off.id] = (uint32_t)params.size();
                params.push_back(std::move(off));
            }
        }
        values.reset(new std::atomic<double>[params.size()]);
        refreshValues();
    }

    static double currentValue(const ParamInfo &pi)
    {
        auto *p = pi.p;
        if (pi.deactivation)
            return p->deactivated ? 1 : 0;
        double v = pi.normalized           ? p->get_value_f01()
                   : p->valtype == vt_float ? p->val.f
                   : p->valtype == vt_int   ? p->val.i
                                            : (p->val.b ? 1 : 0);
        // Unused oscillator controls can hold values outside the current type's range.
        return std::clamp(v, pi.min, pi.max);
    }

    // Surge's 0..1 value for a value in the parameter's exposed units.
    static float to01(const ParamInfo &pi, double v)
    {
        auto *p = pi.p;
        v = std::clamp(v, pi.min, pi.max);
        if (pi.normalized)
            return (float)v;
        switch (p->valtype)
        {
        case vt_float:
            return p->val_max.f > p->val_min.f
                       ? (float)((v - p->val_min.f) / (p->val_max.f - p->val_min.f))
                       : 0.f;
        case vt_int:
            return Parameter::intScaledToFloat((int)std::lround(v), p->val_max.i, p->val_min.i);
        default:
            return v > 0.5 ? 1.f : 0.f;
        }
    }

    void refreshValues()
    {
        for (size_t i = 0; i < params.size(); ++i)
            values[i].store(currentValue(params[i]), std::memory_order_relaxed);
    }

    const ParamInfo *find(clap_id id, void *cookie) const
    {
        if (cookie)
            return static_cast<const ParamInfo *>(cookie);
        auto it = indexById.find(id);
        return it == indexById.end() ? nullptr : &params[it->second];
    }

    void setParam(const ParamInfo &pi, double v)
    {
        if (pi.deactivation)
            pi.p->deactivated = v > 0.5; // read by the engine at the next block
        else
            synth->setParameter01(synth->idForParameter(pi.p), to01(pi, v), true);
        values[&pi - params.data()].store(currentValue(pi), std::memory_order_relaxed);
    }

    // --- events -------------------------------------------------------------------

    static bool validNote(int16_t channel, int16_t key)
    {
        return channel >= 0 && channel < 16 && key >= 0 && key < 128;
    }

    static char vel127(double v) { return (char)std::clamp((int)std::lround(v * 127), 0, 127); }

    void endNote(const clap_event_note_t *n, bool choke)
    {
        auto vel = vel127(n->velocity);
        if (validNote(n->channel, n->key))
        {
            if (choke)
                synth->chokeNote(n->channel, n->key, vel, n->note_id);
            else
                synth->releaseNote(n->channel, n->key, vel, n->note_id);
            return;
        }
        if (n->note_id >= 0 && !choke)
        {
            synth->releaseNoteByHostNoteID(n->note_id, vel);
            return;
        }
        // Wildcard key or channel: every matching note.
        for (int c = 0; c < 16; ++c)
            for (int k = 0; k < 128; ++k)
                if ((n->channel < 0 || n->channel == c) && (n->key < 0 || n->key == k))
                {
                    if (choke)
                        synth->chokeNote(c, k, vel, n->note_id);
                    else
                        synth->releaseNote(c, k, vel, n->note_id);
                }
    }

    void midi(const uint8_t *d)
    {
        const int ch = d[0] & 0x0F;
        switch (d[0] & 0xF0)
        {
        case 0x90:
            if (d[2])
            {
                synth->playNote(ch, d[1] & 0x7F, d[2] & 0x7F, 0, -1);
                break;
            }
            [[fallthrough]]; // MIDI's note-on with velocity 0 is a note-off
        case 0x80:
            synth->releaseNote(ch, d[1] & 0x7F, d[2] & 0x7F);
            break;
        case 0xA0:
            synth->polyAftertouch(ch, d[1] & 0x7F, d[2] & 0x7F);
            break;
        case 0xB0:
            synth->channelController(ch, d[1] & 0x7F, d[2] & 0x7F);
            break;
        case 0xD0:
            synth->channelAftertouch(ch, d[1] & 0x7F);
            break;
        case 0xE0:
            synth->pitchBend(ch, ((d[2] & 0x7F) << 7 | (d[1] & 0x7F)) - 8192);
            break;
        default:
            break;
        }
    }

    void apply(const clap_event_header_t *e)
    {
        if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        switch (e->type)
        {
        case CLAP_EVENT_NOTE_ON:
        {
            auto n = reinterpret_cast<const clap_event_note_t *>(e);
            if (validNote(n->channel, n->key))
                synth->playNote(n->channel, n->key, vel127(n->velocity), 0, n->note_id);
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
            endNote(reinterpret_cast<const clap_event_note_t *>(e), false);
            break;
        case CLAP_EVENT_NOTE_CHOKE:
            endNote(reinterpret_cast<const clap_event_note_t *>(e), true);
            break;
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            auto x = reinterpret_cast<const clap_event_note_expression_t *>(e);
            auto net = SurgeVoice::UNKNOWN;
            switch (x->expression_id)
            {
            case CLAP_NOTE_EXPRESSION_VOLUME:
                net = SurgeVoice::VOLUME;
                break;
            case CLAP_NOTE_EXPRESSION_PAN:
                net = SurgeVoice::PAN;
                break;
            case CLAP_NOTE_EXPRESSION_TUNING:
                net = SurgeVoice::PITCH;
                break;
            case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
                net = SurgeVoice::TIMBRE;
                break;
            case CLAP_NOTE_EXPRESSION_PRESSURE:
                net = SurgeVoice::PRESSURE;
                break;
            default:
                break;
            }
            if (net != SurgeVoice::UNKNOWN)
                synth->setNoteExpression(net, x->note_id, x->key, x->channel, x->value);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE:
        {
            auto v = reinterpret_cast<const clap_event_param_value_t *>(e);
            if (auto pi = find(v->param_id, v->cookie); pi && std::isfinite(v->value))
            {
                paramEpoch.fetch_add(1);
                setParam(*pi, v->value);
                paramEpoch.fetch_add(1);
            }
            break;
        }
        case CLAP_EVENT_PARAM_MOD:
        {
            auto m = reinterpret_cast<const clap_event_param_mod_t *>(e);
            auto pi = find(m->param_id, m->cookie);
            if (!pi || !(pi->flags & CLAP_PARAM_IS_MODULATABLE) || !std::isfinite(m->amount))
                break;
            auto depth = (float)(m->amount / (pi->max - pi->min)); // in Surge's 0..1 units
            if ((m->note_id < 0 && m->key < 0 && m->channel < 0) ||
                !(pi->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID))
                synth->applyParameterMonophonicModulation(pi->p, depth);
            else
                synth->applyParameterPolyphonicModulation(pi->p, m->note_id, m->key, m->channel,
                                                          depth);
            break;
        }
        case CLAP_EVENT_MIDI:
            midi(reinterpret_cast<const clap_event_midi_t *>(e)->data);
            break;
        default:
            break;
        }
    }

    void emitNoteEnds(const clap_output_events_t *out, uint32_t time)
    {
        for (int i = 0; i < synth->hostNoteEndedDuringBlockCount; ++i)
        {
            clap_event_note_t n{};
            n.header = {sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0};
            n.note_id = synth->endedHostNoteIds[i];
            n.port_index = 0;
            n.channel = synth->endedHostNoteOriginalChannel[i];
            n.key = synth->endedHostNoteOriginalKey[i];
            n.velocity = 0;
            out->try_push(out, &n.header);
        }
    }

    clap_process_status process(const clap_process_t *pr)
    {
        if (pr->audio_outputs_count != 1 || pr->audio_outputs[0].channel_count != 2)
            return CLAP_PROCESS_ERROR;
        float *outL = pr->audio_outputs[0].data32[0], *outR = pr->audio_outputs[0].data32[1];

        inProcess.store(true);
        if (loading.load())
        {
            backedOut.store(true);
            inProcess.store(false);
            std::fill(outL, outL + pr->frames_count, 0.f);
            std::fill(outR, outR + pr->frames_count, 0.f);
            return CLAP_PROCESS_CONTINUE;
        }

        if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) &&
            pr->transport->tempo > 0)
            synth->time_data.tempo = pr->transport->tempo;
        if (pr->transport)
            synth->time_data.isPlaying = pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING;

        auto in = pr->in_events;
        const uint32_t count = in->size(in);
        uint32_t next = 0;
        for (uint32_t s = 0; s < pr->frames_count; ++s)
        {
            if (blockPos == BLOCK_SIZE)
            {
                for (; next < count; ++next)
                {
                    auto e = in->get(in, next);
                    if (e->time > s)
                        break;
                    apply(e);
                }
                synth->process();
                synth->time_data.ppqPos +=
                    (double)BLOCK_SIZE * synth->time_data.tempo / (60. * sampleRate);
                emitNoteEnds(pr->out_events, s);
                blockPos = 0;
            }
            outL[s] = synth->output[0][blockPos];
            outR[s] = synth->output[1][blockPos];
            ++blockPos;
        }
        // Events past the next block boundary: applying them now is the same as applying
        // them at the start of the next call, since nothing renders in between.
        for (; next < count; ++next)
            apply(in->get(in, next));

        inProcess.store(false);
        return CLAP_PROCESS_CONTINUE;
    }

    // --- state --------------------------------------------------------------------

    // Runs f with the audio thread kept out of the engine. Main thread only.
    template <typename F> void excludingAudio(F &&f)
    {
        backedOut.store(false);
        loading.store(true);
        while (inProcess.load() && !backedOut.load())
            std::this_thread::yield();
        f();
        loading.store(false);
    }

    // Applies the one-scene, no-effects, no-formula rules to a loaded patch.
    bool normalizePatch()
    {
        auto &patch = synth->storage.getPatch();
        for (auto &lfo : patch.scene[0].lfo)
            if (lfo.shape.val.i == lt_formula)
                return false;
        patch.scenemode.val.i = sm_single;
        patch.scene_active.val.i = 0;
        patch.fx_disable.val.i = (1 << n_fx_slots) - 1;
        return true;
    }

    // A voice takes its parameters from the scene data, which the engine refreshes from the
    // patch at the start of each of its blocks; a note at the start of the first block after a
    // load would otherwise start with the previous patch's values (LFO start phases, which are
    // read only when a voice starts, stay wrong for the whole note). Audio excluded.
    void refreshSceneData()
    {
        auto &patch = synth->storage.getPatch();
        patch.copy_globaldata(patch.globaldata);
        patch.copy_scenedata(patch.scenedata[0], patch.scenedataOrig[0], 0);
    }

    std::vector<uint8_t> lastGood; // state to restore when a load is refused

    bool loadStateBytes(const void *data, size_t size)
    {
        bool ok = false;
        excludingAudio([&] {
            // Notes ended by the load are reported at the start of the next block.
            synth->hostNoteEndedDuringBlockCount = 0;
            synth->loadRaw(data, (int)size, false);
            ok = normalizePatch();
            if (!ok && !lastGood.empty())
            {
                synth->loadRaw(lastGood.data(), (int)lastGood.size(), false);
                normalizePatch();
            }
            refreshSceneData();
            synth->storage.perform_queued_wtloads(); // here, not in process()
            for (int i = 0; i < synth->hostNoteEndedDuringBlockCount; ++i)
            {
                int h = synth->hostNoteEndedToPushToNextBlock++;
                synth->nextBlockEndedHostNoteIds[h] = synth->endedHostNoteIds[i];
                synth->nextBlockEndedHostNoteOriginalKey[h] = synth->endedHostNoteOriginalKey[i];
                synth->nextBlockEndedHostNoteOriginalChannel[h] =
                    synth->endedHostNoteOriginalChannel[i];
            }
            synth->hostNoteEndedDuringBlockCount = 0;
            refreshValues();
        });
        if (ok)
            lastGood.assign((const uint8_t *)data, (const uint8_t *)data + size);
        return ok;
    }

    std::vector<uint8_t> saveStateBytes()
    {
        std::vector<uint8_t> out;
        auto save = [&] {
            void *data = nullptr;
            auto size = synth->saveRaw(&data);
            out.assign((uint8_t *)data, (uint8_t *)data + size);
        };
        // Parameter changes are the only thing the audio thread writes into the patch;
        // retry if one happened during the save, then fall back to excluding audio.
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            auto before = paramEpoch.load();
            if (before & 1)
                continue;
            save();
            if (paramEpoch.load() == before)
                return out;
        }
        excludingAudio(save);
        return out;
    }
};

Plugin &self(const clap_plugin_t *p) { return *static_cast<Plugin *>(p->plugin_data); }

// --- extensions -------------------------------------------------------------------------

uint32_t portsCount(const clap_plugin_t *, bool isInput) { return isInput ? 0 : 1; }
bool portsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info)
{
    if (isInput || index != 0)
        return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t audioPorts{portsCount, portsGet};

uint32_t notePortsCount(const clap_plugin_t *, bool isInput) { return isInput ? 1 : 0; }
bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_note_port_info_t *info)
{
    if (!isInput || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}
const clap_plugin_note_ports_t notePorts{notePortsCount, notePortsGet};

uint32_t paramsCount(const clap_plugin_t *p) { return (uint32_t)self(p).params.size(); }
bool paramsInfo(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info)
{
    auto &s = self(p);
    if (index >= s.params.size())
        return false;
    auto &pi = s.params[index];
    info->id = pi.id;
    info->flags = pi.flags;
    info->cookie = const_cast<ParamInfo *>(&pi);
    std::snprintf(info->name, sizeof(info->name), "%s", pi.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", pi.module.c_str());
    info->min_value = pi.min;
    info->max_value = pi.max;
    info->default_value = pi.def;
    return true;
}
bool paramsValue(const clap_plugin_t *p, clap_id id, double *value)
{
    auto &s = self(p);
    auto it = s.indexById.find(id);
    if (it == s.indexById.end())
        return false;
    *value = s.values[it->second].load(std::memory_order_relaxed);
    return true;
}
bool paramsValueToText(const clap_plugin_t *p, clap_id id, double value, char *out, uint32_t size)
{
    auto &s = self(p);
    auto pi = s.find(id, nullptr);
    if (!pi || size == 0)
        return false;
    // shortcut: reads the parameter's current control type, which the audio thread can
    // change for oscillator controls; give these a snapshot once sound contexts exist.
    auto text = pi->deactivation ? std::string(value > 0.5 ? "Off" : "On")
                                 : pi->p->get_display(true, Plugin::to01(*pi, value));
    std::snprintf(out, size, "%s", text.c_str());
    return true;
}
bool paramsTextToValue(const clap_plugin_t *p, clap_id id, const char *text, double *value)
{
    auto &s = self(p);
    auto pi = s.find(id, nullptr);
    if (pi && pi->deactivation)
    {
        std::string t(text);
        if (t != "Off" && t != "On")
            return false;
        *value = t == "Off" ? 1 : 0;
        return true;
    }
    if (!pi || !pi->p->can_setvalue_from_string())
        return false;
    pdata parsed{};
    std::string err;
    if (!pi->p->set_value_from_string_onto(text, parsed, err))
        return false;
    double v = pi->p->valtype == vt_float ? parsed.f
               : pi->p->valtype == vt_int ? parsed.i
                                          : (parsed.b ? 1 : 0);
    if (pi->normalized)
    {
        Parameter copy = *pi->p;
        copy.val = parsed;
        v = copy.get_value_f01();
    }
    *value = std::clamp(v, pi->min, pi->max);
    return true;
}
void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in,
                 const clap_output_events_t *)
{
    auto &s = self(p);
    // Called on the main thread when not processing, or on the audio thread instead of process.
    s.inProcess.store(true);
    if (s.loading.load())
        s.backedOut.store(true);
    else
    {
        for (uint32_t i = 0; i < in->size(in); ++i)
        {
            auto e = in->get(in, i);
            if (e->space_id == CLAP_CORE_EVENT_SPACE_ID &&
                (e->type == CLAP_EVENT_PARAM_VALUE || e->type == CLAP_EVENT_PARAM_MOD))
                s.apply(e);
        }
    }
    s.inProcess.store(false);
}
const clap_plugin_params_t paramsExt{paramsCount,       paramsInfo,        paramsValue,
                                     paramsValueToText, paramsTextToValue, paramsFlush};

bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream)
{
    auto bytes = self(p).saveStateBytes();
    size_t done = 0;
    while (done < bytes.size())
    {
        auto n = stream->write(stream, bytes.data() + done, bytes.size() - done);
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}
bool stateLoad(const clap_plugin_t *p, const clap_istream_t *stream)
{
    std::vector<uint8_t> bytes;
    uint8_t buf[16384];
    for (;;)
    {
        auto n = stream->read(stream, buf, sizeof(buf));
        if (n < 0)
            return false;
        if (n == 0)
            break;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    // A state starts with Surge's "sub3" patch header.
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "sub3", 4) != 0)
        return false;
    return self(p).loadStateBytes(bytes.data(), bytes.size());
}
const clap_plugin_state_t stateExt{stateSave, stateLoad};

// --- plugin -----------------------------------------------------------------------------

bool pluginInit(const clap_plugin_t *p)
{
    auto &s = self(p);
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    s.synth = std::make_unique<SurgeSynthesizer>(&s.layer, config);
    s.synth->storage.saveFixedTempo = true;
    // The instrument this plugin matches doesn't overshoot at the end of an envelope's attack.
    s.synth->storage.analogEnvelopeAttackClamp = true;
    // Its amp velocity sensitivity is an amplitude law, not linear in dB.
    s.synth->storage.vcaVelocityAmplitudeLaw = true;
    // Its saturators follow u / sqrt(1 + u^2) with a square term, not tanh.
    s.synth->storage.saturatorAlgebraicCurve = true;
    // Envelope segments up to 2^8 s: the instrument this plugin matches reaches 44 s of attack
    // (10-90 %), past Surge's 2^5 s. Only the range grows; the envelope code has no limit, and
    // modulation depths are in seconds-exponent units, so existing patches sound the same.
    for (auto &eg : s.synth->storage.getPatch().scene[0].adsr)
        for (auto *t : {&eg.a, &eg.d, &eg.r})
            t->val_max.f = longestEnvelopeTime;
    // The same for the voice LFOs' attack and decay, which serve as the pitch, oscillator and
    // modifier envelopes. Not release: an LFO release at its maximum means it never releases.
    for (int l = 0; l < n_lfos_voice; l++)
        for (auto *t : {&s.synth->storage.getPatch().scene[0].lfo[l].attack,
                        &s.synth->storage.getPatch().scene[0].lfo[l].decay})
            t->val_max.f = longestEnvelopeTime;
    // Portamento up to just under the engine's limit of 2^4 s (Surge's range stops at 2^2):
    // the instrument this plugin matches glides an octave in 8.9 s (10-90 %) at time 120.
    s.synth->storage.getPatch().scene[0].portamento.val_max.f = 3.99f;
    s.synth->setSamplerate((float)s.sampleRate);
    s.normalizePatch();
    s.refreshSceneData();
    s.synth->storage.perform_queued_wtloads();
    s.synth->seedRandom(renderSeed);
    s.buildParams();
    s.lastGood = s.saveStateBytes();
    return true;
}
void pluginDestroy(const clap_plugin_t *p) { delete &self(p); }
bool pluginActivate(const clap_plugin_t *p, double rate, uint32_t, uint32_t)
{
    auto &s = self(p);
    s.sampleRate = rate;
    s.synth->setSamplerate((float)rate);
    s.synth->allNotesOff();
    s.synth->seedRandom(renderSeed);
    s.synth->time_data.ppqPos = 0;
    s.synth->audio_processing_active = true;
    s.blockPos = BLOCK_SIZE;
    return true;
}
void pluginDeactivate(const clap_plugin_t *p) { self(p).synth->audio_processing_active = false; }
bool pluginStartProcessing(const clap_plugin_t *) { return true; }
void pluginStopProcessing(const clap_plugin_t *) {}
void pluginReset(const clap_plugin_t *p)
{
    auto &s = self(p);
    s.synth->allSoundOff();
    s.blockPos = BLOCK_SIZE;
}
clap_process_status pluginProcess(const clap_plugin_t *p, const clap_process_t *pr)
{
    return self(p).process(pr);
}
const void *pluginGetExtension(const clap_plugin_t *, const char *id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &notePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &paramsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &stateExt;
    return nullptr;
}
void pluginOnMainThread(const clap_plugin_t *) {}

uint32_t factoryCount(const clap_plugin_factory_t *) { return 1; }
const clap_plugin_descriptor_t *factoryDescriptor(const clap_plugin_factory_t *, uint32_t i)
{
    return i == 0 ? &descriptor : nullptr;
}
const clap_plugin_t *factoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *id)
{
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, descriptor.id))
        return nullptr;
    auto s = new Plugin(host);
    s->plugin.init = pluginInit;
    s->plugin.destroy = pluginDestroy;
    s->plugin.activate = pluginActivate;
    s->plugin.deactivate = pluginDeactivate;
    s->plugin.start_processing = pluginStartProcessing;
    s->plugin.stop_processing = pluginStopProcessing;
    s->plugin.reset = pluginReset;
    s->plugin.process = pluginProcess;
    s->plugin.get_extension = pluginGetExtension;
    s->plugin.on_main_thread = pluginOnMainThread;
    return &s->plugin;
}
const clap_plugin_factory_t factory{factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char *) { return true; }
void entryDeinit() {}
const void *entryFactory(const char *id)
{
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory;
}
} // namespace

const clap_plugin_entry_t oscairn_analog_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit,
                                                  entryFactory};
