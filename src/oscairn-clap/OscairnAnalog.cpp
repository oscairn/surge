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
 * - Written against CLAP directly, no JUCE: a stereo main output and eight stereo sound buses,
 *   no input, one note port.
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
 * - Presets: Surge's factory and user .fxp patches, listed through CLAP preset discovery and
 *   loaded with preset-load, or prepared as sound contexts.
 * - Oscairn's native sound contexts (oscairn.sound-contexts/draft-5, with the Oscairn SDK's
 *   helper): each prepared sound is a Surge synthesizer of its own; MIDI channels select a
 *   sound for new notes, and a held note stays on the sound it started on. The CLAP state is
 *   the default sound's patch; the sound-context snapshot holds every sound, and a snapshot's
 *   sounds can be prepared beside the playing ones (oscairn.sound-snapshots/draft-4), the
 *   outgoing ones ringing out into the host's ring-out buffer; a sound can be saved on its own
 *   and its values read. Each sound plays on the output its Output Bus parameter names, so
 *   several Oscairn parts can share one instance.
 * - Oscairn's voice budget (oscairn.voice-budget/draft-1): past the host's limits, Surge
 *   voices of every sound are given up in the SDK's order and fade out over 3 ms. With no
 *   limit set, Surge steals voices as it always has.
 */

#include "OscairnAnalog.h"

#include "SurgeSynthesizer.h"
#include "PatchFileHeaderStructs.h"
#include "sst/basic-blocks/mechanics/endian-ops.h"

#include <oscairn-clap/memory.h>
#include <oscairn-clap/sound-contexts-plugin.hpp>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
    "0.3.4",
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
    Parameter *p{nullptr}; // the default sound's; other sounds' by ptr
    uint32_t ptr{0};       // index in the patch's param_ptr, the same in every synthesizer
    clap_id id{0};
    bool normalized{false}; // exposed as Surge's 0..1 value instead of its own units
    bool deactivation{false}; // the parameter's off switch (1 = off), not its value
    bool bus{false};          // the Output Bus: the sound's output port, not a Surge parameter
    double min{0}, max{1}, def{0};
    uint32_t flags{0};
    uint32_t role{OSCAIRN_ROLE_NONE}; // oscairn.ui
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

// --- presets ----------------------------------------------------------------------------
// Surge's .fxp patch files, listed through CLAP preset discovery and loaded with preset-load.

// Reads a Surge .fxp file: its chunk header ('CcnK', 'FPCh', 'cjs3') and, with data, the patch
// data after it. False when the file isn't one.
bool readFxp(const char *path, std::vector<uint8_t> *data)
{
    namespace mech = sst::basic_blocks::mechanics;
    std::error_code ec;
    auto fileSize = fs::file_size(string_to_path(path), ec);
    std::ifstream f(string_to_path(path), std::ios::binary);
    sst::io::fxChunkSetCustom h{};
    if (ec || !f.read(reinterpret_cast<char *>(&h), sizeof(h)) ||
        mech::endian_read_int32BE((uint32_t)h.chunkMagic) != 'CcnK' ||
        mech::endian_read_int32BE((uint32_t)h.fxMagic) != 'FPCh' ||
        mech::endian_read_int32BE((uint32_t)h.fxID) != 'cjs3')
        return false;
    if (!data)
        return true;
    uint32_t size = mech::endian_read_int32BE((uint32_t)h.chunkSize);
    if (size == 0 || size > fileSize - sizeof(h))
        return false;
    data->resize(size);
    return (bool)f.read(reinterpret_cast<char *>(data->data()), size);
}

// --- sound contexts -----------------------------------------------------------------------

// Every prepared sound is a whole Surge synthesizer, but they share one store of Surge's voices
// (18.2 MiB, 128 voices), which made up most of each (32 MiB before). What stays a sound's own is
// mostly its patch (3.3 MiB), its oscillators' wavetables and the synthesizer object without its
// voices: 13-14 MiB (see synthBytes). 16 sounds with the default fit where 8 did (288 MiB).
constexpr uint32_t contextCapacity = 16;
constexpr uint32_t soundBuses = 8; // output ports beside Main, as Oscairn's Basic Synth has
constexpr size_t voiceCapacity = 256; // notes held or sounding, every sound together

// A note in a sound: it goes to that sound's synthesizer under an ID of its own, and ends when
// the synthesizer reports that ID ended.
struct Voice : oscairn::contexts::VoiceBase
{
    int32_t surgeId{-1};
    bool released{false}, choked{false}, ended{false};
};

// A sound's patch as Surge's patch data ("sub3" onwards), as a preset or a snapshot carries it.
// The sound plays from its synthesizer (Slot); this is filled from it for a snapshot.
struct Patch
{
    std::vector<uint8_t> state;
    uint32_t bus{0}; // its Output Bus
    // A snapshot's sound, built while reading it (main thread, beside whatever plays), installed
    // when the sound is published or the snapshot loaded.
    std::unique_ptr<SurgeSynthesizer> built;
    Patch() = default;
    Patch(Patch &&) = default;
    Patch &operator=(Patch &&) = default;
    Patch(const Patch &o) : state(o.state), bus(o.bus) {} // a copy (draft 4's saveSound)
};

// The synthesizer of a sound context slot. The audio thread touches it only while the slot is
// pinned (its context's users), the main thread only while it isn't, or with processing
// excluded.
struct Slot
{
    SurgeSynthesizer *synth{nullptr};
    std::unique_ptr<SurgeSynthesizer> owned; // slots 1 up; slot 0 plays Plugin::synth
    bool ringing{false}; // audio: pinned while it may still sound
    bool silent{false};  // audio: quiet after its last block (see renderBlock)
    uint32_t quietBlocks{0}; // audio: blocks in a row without voices, under -120 dB
    bool fresh{false};   // audio: rendered the current block, which pins it
    bool choking{false}; // audio: choked by chokeRetiring: fading out, then silent until unpinned
    uint32_t fadeLeft{0}; // audio: frames of that fade left
    // The Output Bus as last set (parameter events on the audio thread; the main thread while
    // the sound isn't published, or with processing excluded), and where its output goes: a
    // change crossfades from the old port to the new one over the choke time.
    std::atomic<uint32_t> bus{0};
    uint32_t outBus{0}, fromBus{0}, busFade{0}; // audio
    bool sounded{false}; // audio: its last block's output wasn't all zero
};

void clear(SurgeSynthesizer &s);

// Surge's voices, shared by every sound's synthesizer: a base, so it outlives every synthesizer,
// those in the helper's contexts too.
struct SharedVoices
{
    std::unique_ptr<SurgeSynthesizer::VoiceStore> voiceStore{
        std::make_unique<SurgeSynthesizer::VoiceStore>()};
};

struct Plugin : SharedVoices,
                oscairn::contexts::Engine<Plugin, Voice, Patch, contextCapacity, voiceCapacity>,
                SurgeSynthesizer::VoiceChooser
{
    using Base = oscairn::contexts::Engine<Plugin, Voice, Patch, contextCapacity, voiceCapacity>;
    static constexpr const char *stateVersion = "oscairn-analog-sounds-2";
    static constexpr size_t maxStateBytes = size_t(64) << 20;

    const clap_host_t *host{nullptr};
    HeadlessLayer layer;
    std::unique_ptr<SurgeSynthesizer> synth; // the default sound's (context 0)
    std::array<Slot, contextCapacity> slots;

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


    Plugin(const clap_host_t *h) : host(h)
    {
        plugin.desc = &descriptor;
        plugin.plugin_data = this;
    }

    static Parameter *paramIn(SurgeSynthesizer &s, const ParamInfo &pi)
    {
        return s.storage.getPatch().param_ptr[pi.ptr];
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

    // oscairn.ui roles: the ones Oscairn guessed from the parameters' names before any was
    // declared (declaring one turns its guessing off), but the voice LFOs' Hold, which it took
    // for the amp envelope's.
    uint32_t roleOf(const Parameter &p) const
    {
        auto &sc = synth->storage.getPatch().scene[0];
        const std::pair<const Parameter *, uint32_t> roles[] = {
            {&sc.volume, OSCAIRN_ROLE_LEVEL},
            {&sc.pan, OSCAIRN_ROLE_PAN},
            {&sc.filterunit[0].cutoff, OSCAIRN_ROLE_CUTOFF},
            {&sc.filterunit[0].resonance, OSCAIRN_ROLE_RESONANCE},
            {&sc.adsr[0].a, OSCAIRN_ROLE_ATTACK},
            {&sc.adsr[0].d, OSCAIRN_ROLE_DECAY},
            {&sc.adsr[0].s, OSCAIRN_ROLE_SUSTAIN},
            {&sc.adsr[0].r, OSCAIRN_ROLE_RELEASE},
            {&sc.adsr[1].a, OSCAIRN_ROLE_FILTER_ATTACK},
            {&sc.adsr[1].d, OSCAIRN_ROLE_FILTER_DECAY},
            {&sc.adsr[1].s, OSCAIRN_ROLE_FILTER_SUSTAIN},
            {&sc.adsr[1].r, OSCAIRN_ROLE_FILTER_RELEASE}};
        for (auto &[q, role] : roles)
            if (q == &p)
                return role;
        return OSCAIRN_ROLE_NONE;
    }

    void buildParams()
    {
        auto &all = synth->storage.getPatch().param_ptr;
        for (uint32_t index = 0; index < all.size(); ++index)
        {
            auto *p = all[index];
            if (!exposed(*p))
                continue;
            ParamInfo pi;
            pi.p = p;
            pi.ptr = index;
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
            pi.role = roleOf(*p);
            indexById[pi.id] = (uint32_t)params.size();
            params.push_back(pi);
            if (p->can_deactivate())
            {
                // Its off switch (the saturators, the filter types, the scene highpass ...),
                // with the ID of "<storage name>.off" and the init patch's state as default
                ParamInfo off;
                off.p = p;
                off.ptr = index;
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
        ParamInfo bus;
        bus.bus = true;
        bus.id = fnv1a("oscairn_output_bus");
        bus.name = "Output Bus";
        bus.module = "Global";
        bus.max = soundBuses;
        bus.flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
        bus.role = OSCAIRN_ROLE_OUTPUT_BUS;
        indexById[bus.id] = (uint32_t)params.size();
        params.push_back(std::move(bus));
        values.reset(new std::atomic<double>[params.size()]);
        refreshValues();
    }

    static double currentValue(const ParamInfo &pi) { return currentValue(pi, pi.p); }
    static double currentValue(const ParamInfo &pi, const Parameter *p)
    {
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

    // A parameter's value in a sound's slot.
    double valueIn(uint32_t slot, const ParamInfo &pi) const
    {
        return pi.bus ? slots[slot].bus.load(std::memory_order_relaxed)
                      : currentValue(pi, paramIn(*slots[slot].synth, pi));
    }
    void refreshValues()
    {
        for (size_t i = 0; i < params.size(); ++i)
            values[i].store(valueIn(0, params[i]), std::memory_order_relaxed);
    }

    const ParamInfo *find(clap_id id, void *cookie) const
    {
        if (cookie)
            return static_cast<const ParamInfo *>(cookie);
        auto it = indexById.find(id);
        return it == indexById.end() ? nullptr : &params[it->second];
    }

    static void setParamIn(SurgeSynthesizer &s, const ParamInfo &pi, double v)
    {
        auto *p = paramIn(s, pi);
        if (pi.deactivation)
            p->deactivated = v > 0.5; // read by the engine at the next block
        else
            s.setParameter01(s.idForParameter(p), to01(pi, v), true);
    }

    // --- events -------------------------------------------------------------------
    // The SDK helper (Base::process) delivers events at their sample times and renders between
    // them; render() runs every pinned synthesizer a block at a time from a block phase counted
    // from activation, so an event takes effect at the first block boundary at or after it.

    static bool validNote(int16_t channel, int16_t key)
    {
        return channel >= 0 && channel < 16 && key >= 0 && key < 128;
    }

    static char vel127(double v) { return (char)std::clamp((int)std::lround(v * 127), 0, 127); }

    int blockPos{BLOCK_SIZE}; // read position in the synthesizers' outputs; BLOCK_SIZE = next
    double sampleRate{48000};
    bool active{false};                         // main thread
    double tempo{0};                            // 0: the host gave none
    int playing{-1};                            // -1: the host gave no transport
    int32_t nextSurgeId{0};                     // audio thread
    char releaseVelocity{0};                    // the note-off being applied
    bool quiet{false};                          // finishing voices without the synthesizers
    // Audio thread: sounds a reset found unpinned, which the main thread may be using; each is
    // cleared when it is next pinned, at the start of a process() call, before any event.
    uint32_t stale{0};
    std::array<std::array<uint8_t, 128>, 16> keySlot{}; // audio: the sound a key last started in

    // Audio thread: the slots it may touch, from what it knows itself (the channels' selections,
    // its voices, its tails and the block being rendered), never from the pin counts, which the
    // main thread also changes.
    uint32_t live(bool withBlock = true) const
    {
        uint32_t m = 0;
        for (auto slot : selected)
            m |= 1u << slot;
        for (auto &v : voices)
            if (v.active)
                m |= 1u << v.context;
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (slots[i].ringing || (withBlock && slots[i].fresh))
                m |= 1u << i;
        return m;
    }
    bool pinned(uint32_t slot) const { return live() >> slot & 1; }
    // Pinned slots a channel's controls reach: the sound it selects and those holding its notes.
    uint32_t targets(int16_t channel) const
    {
        uint32_t m = 0;
        for (int c = 0; c < 16; ++c)
            if (channel < 0 || channel == c)
                m |= 1u << selected[c];
        for (auto &v : voices)
            if (v.active && (channel < 0 || v.channel == channel))
                m |= 1u << v.context;
        return m;
    }
    template <typename F> void forTargets(int16_t channel, F &&f)
    {
        auto m = targets(channel) & live();
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (m >> i & 1)
                f(*slots[i].synth, i);
    }
    void wake(uint32_t slot)
    {
        auto &sl = slots[slot];
        sl.silent = false;
        sl.quietBlocks = 0;
        if (sl.ringing)
            return;
        sl.ringing = true;
        contexts[slot].users.fetch_add(1, std::memory_order_relaxed);
    }

    // --- oscairn.voice-budget -------------------------------------------------------
    // A voice is one Surge voice (a unison copy is one) of any sound the audio thread may
    // touch. Past the host's limits, voices are given up in voice-budget.h's order (the SDK's
    // victim()) and fade out over 3 ms; with no limit set, nothing here runs and Surge steals as
    // it always has. Audio thread only.
    oscairn::voices::Limits budget; // as set_limits last gave it
    static constexpr size_t surgeVoices = n_scenes * MAX_VOICES; // the shared store's
    std::array<oscairn::voices::Candidate, surgeVoices> candidates{};
    std::array<SurgeVoice *, surgeVoices> candidateVoice{}; // null: no voice there

    bool budgeted() const { return budget.limit || budget.count; }
    size_t storeIndex(const SurgeVoice &v) const
    {
        return size_t(&v - voiceStore->voices[0].data());
    }
    static bool monoSound(SurgeSynthesizer &s)
    {
        return s.storage.getPatch().scene[0].polymode.val.i != pm_poly;
    }
    // Linear, 1 at full level: the amp's gain (level, velocity, envelope, fade) and the note's
    // volume expression.
    static double voiceGain(const SurgeVoice &v)
    {
        return v.ampGain() * v.noteExpressions[SurgeVoice::VOLUME];
    }
    // f(note, voice) for each Surge voice playing one of the helper's notes, in pinned sounds.
    template <typename F> void forNoteVoices(F &&f)
    {
        const auto m = live();
        for (auto &n : voices)
            if (n.active && (m >> n.context & 1))
                for (auto *sv : slots[n.context].synth->voices[0])
                    if (sv->host_note_id == n.surgeId)
                        f(n, *sv);
    }
    // Describes every Surge voice as a Candidate. Held: its note's key is down (a pedal
    // doesn't count); in a mono or legato sound, its gate, so a sustained mono note is kept.
    void describeVoices()
    {
        candidateVoice.fill(nullptr);
        const auto m = live();
        for (uint32_t slot = 0; slot < contextCapacity; ++slot)
        {
            if (!(m >> slot & 1))
                continue;
            auto &sy = *slots[slot].synth;
            const auto &c = contexts[slot];
            const bool mono = monoSound(sy), selected = c.selected.load(std::memory_order_relaxed);
            for (auto *sv : sy.voices[0])
            {
                const size_t i = storeIndex(*sv);
                candidates[i] = {c.id,
                                 selected,
                                 mono && sv->state.gate,
                                 sv->fadeBlocks || sv->state.uberrelease,
                                 mono,
                                 voiceGain(*sv),
                                 uint64_t(INT32_MAX - sv->age)}; // age counts blocks
                candidateVoice[i] = sv;
            }
        }
        forNoteVoices([&](Voice &n, SurgeVoice &sv) {
            candidates[storeIndex(sv)].held |= n.held;
        });
    }
    // Voices sounding and not on their way out, of context `id` or of all.
    uint32_t keeping(uint32_t id = oscairn::voices::none) const
    {
        uint32_t n = 0;
        for (size_t i = 0; i < surgeVoices; ++i)
            n += candidateVoice[i] && !candidates[i].ending &&
                 (id == oscairn::voices::none || candidates[i].context == id);
        return n;
    }
    int victim(uint32_t incoming, uint32_t only = oscairn::voices::none)
    {
        return oscairn::voices::victim(
            surgeVoices, [&](size_t i) { return candidateVoice[i] ? &candidates[i] : nullptr; },
            budget, incoming, only);
    }
    bool giveUp(int i)
    {
        if (i < 0)
            return false;
        candidateVoice[i]->fadeOut(std::max(1, (int)std::ceil(chokeFrames / (double)BLOCK_SIZE)));
        candidates[i].ending = true;
        return true;
    }
    // Before a note starts in `slot`: room for the voices it will add within the limits.
    void makeRoom(uint32_t slot, int16_t key)
    {
        if (!budgeted())
            return;
        describeVoices();
        auto &sy = *slots[slot].synth;
        const auto &c = contexts[slot];
        uint32_t adding = (uint32_t)sy.unisonVoiceCount(0);
        if (monoSound(sy))
            adding = keeping(c.id) ? 0 : adding; // the new note takes the sounding one's place
        else if (sy.storage.getPatch().scene[0].polyVoiceRepeatedKeyMode != NEW_VOICE_EVERY_NOTEON)
            for (auto *sv : sy.voices[0])
                if (sv->state.key == key)
                    adding = 0; // reuses that key's voice
        const auto own = budget.of(c.id, c.selected.load(std::memory_order_relaxed));
        while (own.limit && keeping(c.id) + adding > own.limit && giveUp(victim(c.id, c.id)))
            ;
        while (budget.limit && keeping() + adding > budget.limit && giveUp(victim(c.id)))
            ;
    }
    // At a block's start (from the helper's process, after the selections): a sound a reset left
    // stale is cleared, and voices past a limit set_limits lowered fade out.
    void keepWithinLimits()
    {
        if (const auto m = stale & live())
        {
            for (uint32_t i = 0; i < contextCapacity; ++i)
                if (m >> i & 1)
                    clear(*slots[i].synth);
            stale &= ~m;
        }
        if (!budgeted())
            return;
        describeVoices();
        const auto m = live();
        for (uint32_t slot = 0; slot < contextCapacity; ++slot)
        {
            if (!(m >> slot & 1))
                continue;
            const auto &c = contexts[slot];
            const auto own = budget.of(c.id, c.selected.load(std::memory_order_relaxed));
            while (own.limit && keeping(c.id) > own.limit &&
                   giveUp(victim(oscairn::voices::none, c.id)))
                ;
        }
        while (budget.limit && keeping() > budget.limit && giveUp(victim(oscairn::voices::none)))
            ;
    }
    // Surge's Polyphony Limit, full: under a budget, the voice it releases is chosen in the
    // same order, within that sound.
    SurgeVoice *voiceToSoftkill(SurgeSynthesizer &s, int) override
    {
        if (!budgeted())
            return nullptr;
        describeVoices();
        for (uint32_t slot = 0; slot < contextCapacity; ++slot)
            if (slots[slot].synth == &s)
            {
                const int i = victim(contexts[slot].id, contexts[slot].id);
                return i < 0 ? nullptr : candidateVoice[i];
            }
        return nullptr;
    }
    uint32_t countVoices()
    {
        uint32_t n = 0;
        const auto m = live();
        for (uint32_t slot = 0; slot < contextCapacity; ++slot)
            if (m >> slot & 1)
                n += (uint32_t)slots[slot].synth->voices[0].size();
        return n;
    }
    uint32_t writeNotes(oscairn_voice_budget_note_t *out, uint32_t capacity)
    {
        oscairn::voices::Report<voiceCapacity> report;
        forNoteVoices([&](Voice &n, SurgeVoice &sv) {
            report.add(n.noteId, 0, n.channel, n.key, voiceGain(sv));
        });
        return report.write(out, capacity);
    }

    // Helper hooks.
    bool accepts(const Patch &, const clap_event_note_t &) { return true; }
    void start(Voice &v, Context &c)
    {
        const auto slot = uint32_t(&c - contexts.data());
        nextSurgeId = (nextSurgeId + 1) & 0x3fffffff;
        v.surgeId = nextSurgeId;
        wake(slot);
        makeRoom(slot, v.key);
        slots[slot].synth->playNote(v.channel, v.key, vel127(v.velocity), 0, v.surgeId);
    }
    void release(Voice &v)
    {
        if (v.released)
            return;
        v.released = true;
        slots[v.context].synth->releaseNote(v.channel, v.key, releaseVelocity, v.surgeId);
    }
    // Every way a voice ends: unless its synthesizer ended it, the synthesizer chokes it.
    void voiceFinished(Voice &v)
    {
        if (!quiet && !v.ended && !v.choked)
            slots[v.context].synth->chokeNote(v.channel, v.key, 0, v.surgeId);
    }
    void noteOn(const clap_event_note_t &n)
    {
        if (validNote(n.channel, n.key) && n.port_index == 0)
            keySlot[n.channel][n.key] = (uint8_t)selected[n.channel];
        Base::noteOn(n);
    }
    void noteOff(const clap_event_note_t &n, bool choke = false, bool midi = false)
    {
        if (n.port_index > 0)
            return;
        releaseVelocity = vel127(n.velocity);
        bool onKey = false;
        for (auto &v : voices)
            onKey |= v.active && v.channel == n.channel && v.key == n.key;
        Base::noteOff(n, choke, midi);
        if (choke)
            return;
        if (validNote(n.channel, n.key))
        {
            // A key whose note its synthesizer already ended (a mono mode moved the voice to
            // another key) is still released there, or the mono modes would go back to it.
            auto slot = keySlot[n.channel][n.key];
            if (!onKey && pinned(slot))
                slots[slot].synth->releaseNote(n.channel, n.key, releaseVelocity, -1);
            return;
        }
        // A note named by its ID: the helper released its voice, nothing else goes.
        if (n.note_id >= 0)
            return;
        // Wildcard key or channel: every matching key still down in every pinned sound, as
        // Surge's own (releasing every key was 2048 calls a sound, 5 ms for eight).
        const auto m = live();
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (m >> i & 1)
                for (int c = 0; c < 16; ++c)
                    for (int k = 0; k < 128; ++k)
                        if ((n.channel < 0 || n.channel == c) && (n.key < 0 || n.key == k) &&
                            slots[i].synth->channelState[c].keyState[k].keyIsDown)
                            slots[i].synth->releaseNote(c, k, releaseVelocity, -1);
    }
    void applyExpression(const clap_event_note_expression_t &x)
    {
        if (x.port_index > 0)
            return;
        auto net = SurgeVoice::UNKNOWN;
        switch (x.expression_id)
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
            return;
        }
        for (auto &v : voices)
            if (matches(v, x.note_id, x.key, x.channel))
                slots[v.context].synth->setNoteExpression(net, v.surgeId, v.key, v.channel,
                                                          x.value);
    }
    void applyMidi(const clap_event_midi_t &e)
    {
        if (e.port_index)
            return;
        const uint8_t *d = e.data;
        const int16_t ch = d[0] & 0x0F;
        const int kind = d[0] & 0xF0;
        if (kind == 0x90 || kind == 0x80)
        {
            clap_event_note_t n{};
            n.note_id = -1;
            n.channel = ch;
            n.key = d[1] & 0x7F;
            n.velocity = (d[2] & 0x7F) / 127.0;
            // MIDI's note-on with velocity 0 is a note-off
            if (kind == 0x90 && (d[2] & 0x7F))
                noteOn(n);
            else
                noteOff(n, false, true);
            return;
        }
        forTargets(ch, [&](SurgeSynthesizer &s, uint32_t) {
            switch (kind)
            {
            case 0xA0:
                s.polyAftertouch(ch, d[1] & 0x7F, d[2] & 0x7F);
                break;
            case 0xB0:
                s.channelController(ch, d[1] & 0x7F, d[2] & 0x7F);
                break;
            case 0xD0:
                s.channelAftertouch(ch, d[1] & 0x7F);
                break;
            case 0xE0:
                s.pitchBend(ch, ((d[2] & 0x7F) << 7 | (d[1] & 0x7F)) - 8192);
                break;
            default:
                break;
            }
        });
    }
    void applyParam(const clap_event_header_t *e)
    {
        if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        if (e->type == CLAP_EVENT_PARAM_VALUE)
        {
            auto v = reinterpret_cast<const clap_event_param_value_t *>(e);
            auto pi = find(v->param_id, v->cookie);
            if (!pi || !std::isfinite(v->value))
                return;
            paramEpoch.fetch_add(1);
            if (pi->bus)
            {
                // the sounds the event's channel selects (the host sets it on a part's channel)
                const auto b = (uint32_t)std::lround(std::clamp(v->value, pi->min, pi->max));
                for (int ch = 0; ch < 16; ++ch)
                    if (v->channel < 0 || v->channel >= 16 || v->channel == ch)
                        slots[selected[ch]].bus.store(b, std::memory_order_relaxed);
                values[pi - params.data()].store(valueIn(selected[0], *pi),
                                                 std::memory_order_relaxed);
            }
            else
                forTargets(v->channel < 16 ? v->channel : -1, [&](SurgeSynthesizer &s, uint32_t i) {
                    setParamIn(s, *pi, v->value);
                    if (i == selected[0])
                        values[pi - params.data()].store(currentValue(*pi, paramIn(s, *pi)),
                                                         std::memory_order_relaxed);
                });
            paramEpoch.fetch_add(1);
        }
        else if (e->type == CLAP_EVENT_PARAM_MOD)
        {
            auto m = reinterpret_cast<const clap_event_param_mod_t *>(e);
            auto pi = find(m->param_id, m->cookie);
            if (!pi || !(pi->flags & CLAP_PARAM_IS_MODULATABLE) || !std::isfinite(m->amount))
                return;
            auto depth = (float)(m->amount / (pi->max - pi->min)); // in Surge's 0..1 units
            if ((m->note_id < 0 && m->key < 0 && m->channel < 0) ||
                !(pi->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID))
            {
                forTargets(m->channel < 16 ? m->channel : -1, [&](SurgeSynthesizer &s, uint32_t) {
                    s.applyParameterMonophonicModulation(paramIn(s, *pi), depth);
                });
                return;
            }
            for (auto &v : voices)
                if (matches(v, m->note_id, m->key, m->channel))
                {
                    auto &s = *slots[v.context].synth;
                    s.applyParameterPolyphonicModulation(paramIn(s, *pi), v.surgeId, v.key,
                                                         v.channel, depth);
                }
        }
    }
    // After a selection for every channel: the parameters show the selected sound's values.
    void selectedGlobally(Context &next, const clap_output_events_t *out)
    {
        const auto slot = uint32_t(&next - contexts.data());
        for (size_t i = 0; i < params.size(); ++i)
        {
            auto v = valueIn(slot, params[i]);
            if (v == values[i].load(std::memory_order_relaxed))
                continue;
            values[i].store(v, std::memory_order_relaxed);
            if (!out)
                continue;
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = params[i].id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = v;
            out->try_push(out, &ev.header);
        }
    }

    // One block of every pinned synthesizer, at frame s of the call.
    void renderBlock(uint32_t s)
    {
        eventTime = s;
        for (uint32_t i = 0; i < contextCapacity; ++i)
        {
            auto &sl = slots[i];
            // A sound found quiet a block ago with no note since gives up its tail's pin here,
            // before its output stops being read.
            if (sl.ringing && sl.silent &&
                std::none_of(voices.begin(), voices.end(),
                             [&](const Voice &v) { return v.active && v.context == i; }))
            {
                sl.ringing = false;
                sl.choking = false;
                contexts[i].users.fetch_sub(1, std::memory_order_release);
            }
            // An Output Bus change: a sound whose last block sounded crossfades to the new port
            if (const auto b = sl.bus.load(std::memory_order_relaxed); b != sl.outBus)
            {
                sl.fromBus = sl.outBus;
                sl.outBus = b;
                sl.busFade = sl.fresh && sl.sounded ? chokeFrames : 0;
            }
            // A block pins what it renders until the next boundary: a selection can unpin a
            // sound mid-block, while this block still reads its output.
            const bool use = live(false) >> i & 1;
            if (use != sl.fresh)
            {
                if (use)
                    contexts[i].users.fetch_add(1, std::memory_order_relaxed);
                else
                    contexts[i].users.fetch_sub(1, std::memory_order_release);
                sl.fresh = use;
            }
            sl.sounded = false;
            if (!sl.fresh)
                continue;
            auto &sy = *sl.synth;
            if (tempo > 0)
                sy.time_data.tempo = tempo;
            if (playing >= 0)
                sy.time_data.isPlaying = playing;
            sy.process();
            sy.time_data.ppqPos += (double)BLOCK_SIZE * sy.time_data.tempo / (60. * sampleRate);
            for (int k = 0; k < sy.hostNoteEndedDuringBlockCount; ++k)
                for (auto &v : voices)
                    if (v.active && v.context == i && v.surgeId == sy.endedHostNoteIds[k])
                    {
                        v.ended = true;
                        finishVoice(v);
                        break;
                    }
            const bool voiceless =
                sy.voices[0].empty() && sy.voices[1].empty() && !sy.delayedNoteCount;
            // Quiet: no voice and no waiting note, and the output (the scene's filters ring on
            // after the last voice) exactly zero for a block, or under -120 dB for a second.
            float peak = 0;
            for (int k = 0; k < BLOCK_SIZE; ++k)
                peak = std::max({peak, std::fabs(sy.output[0][k]), std::fabs(sy.output[1][k])});
            sl.sounded = peak > 0;
            sl.quietBlocks = voiceless && peak < 1e-6f ? sl.quietBlocks + 1 : 0;
            sl.silent = voiceless && (peak == 0 || sl.quietBlocks * BLOCK_SIZE >= sampleRate);
            if (sl.choking && !sl.fadeLeft)
            {
                // faded out: its voices end and its tail (the scene's filters) is cut, at once
                // (all sound off would wait for its next blocks, then cut the notes struck there)
                sy.stopSound();
                sl.silent = true;
            }
            // Released notes whose voices are all gone end, if the synthesizer didn't say so.
            if (voiceless)
                for (auto &v : voices)
                    if (v.active && v.context == i && !v.held)
                    {
                        v.ended = true;
                        finishVoice(v);
                    }
        }
    }
    void render(const clap_process_t *pr, uint32_t from, uint32_t to)
    {
        // A choked note is choked in its synthesizer; the helper ends it after its fade.
        for (auto &v : voices)
            if (v.active && v.chokeLength && !v.choked)
            {
                v.choked = true;
                slots[v.context].synth->chokeNote(v.channel, v.key, 0, v.surgeId);
            }
        const uint32_t ports = std::min(pr->audio_outputs_count, soundBuses + 1);
        for (uint32_t s = from; s < to; ++s)
        {
            if (blockPos == BLOCK_SIZE)
            {
                renderBlock(s);
                blockPos = 0;
            }
            // A retiring sound (selected by no channel, still sounding) on the main output goes
            // to the ring-out channels retiring() gives when the host gives a buffer
            // (oscairn.sound-snapshots); one chokeRetiring choked fades out there over the choke
            // time, tail and all, and then stays silent. Only the main output rings out: a sound
            // on a sound bus plays on there.
            float out[soundBuses + 1][2] = {};
            for (uint32_t i = 0; i < contextCapacity; ++i)
            {
                auto &sl = slots[i];
                if (!sl.fresh)
                    continue;
                if (sl.choking && contexts[i].selected.load(std::memory_order_relaxed))
                    sl.choking = false; // selected again
                auto *ro = sl.outBus || sl.busFade ? nullptr : retiring(i);
                if (ro && ro == ringOut + 2 && !sl.choking)
                {
                    sl.choking = true;
                    sl.fadeLeft = chokeFrames;
                }
                float g = 1;
                if (sl.choking)
                {
                    // the helper's fading flag ends with the last voice; the tail stays on the
                    // fading channels until it is cut
                    ro = ringOut ? ringOut + 2 : nullptr;
                    g = (float)sl.fadeLeft / (float)chokeFrames;
                    if (sl.fadeLeft)
                        --sl.fadeLeft;
                }
                const float x = g * sl.synth->output[0][blockPos],
                            y = g * sl.synth->output[1][blockPos];
                if (ro)
                {
                    ro[0][s] += x;
                    ro[1][s] += y;
                    continue;
                }
                if (sl.busFade)
                {
                    const float h = (float)sl.busFade-- / (float)chokeFrames;
                    out[sl.fromBus][0] += h * x;
                    out[sl.fromBus][1] += h * y;
                    out[sl.outBus][0] += (1 - h) * x;
                    out[sl.outBus][1] += (1 - h) * y;
                    continue;
                }
                out[sl.outBus][0] += x;
                out[sl.outBus][1] += y;
            }
            for (uint32_t b = 0; b < ports; ++b)
            {
                auto &o = pr->audio_outputs[b];
                if (o.channel_count >= 2 && o.data32)
                {
                    o.data32[0][s] = out[b][0];
                    o.data32[1][s] = out[b][1];
                }
            }
            ++blockPos;
        }
    }

    clap_process_status process(const clap_process_t *pr)
    {
        if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count != 2)
            return CLAP_PROCESS_ERROR;

        inProcess.store(true);
        if (loading.load())
        {
            backedOut.store(true);
            inProcess.store(false);
            for (uint32_t b = 0; b < pr->audio_outputs_count; ++b)
                for (uint32_t k = 0; k < pr->audio_outputs[b].channel_count; ++k)
                    if (pr->audio_outputs[b].data32)
                        std::fill(pr->audio_outputs[b].data32[k],
                                  pr->audio_outputs[b].data32[k] + pr->frames_count, 0.f);
            return CLAP_PROCESS_CONTINUE;
        }
        if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) &&
            pr->transport->tempo > 0)
            tempo = pr->transport->tempo;
        if (pr->transport)
            playing = (pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING) ? 1 : 0;
        auto status = Base::process(&plugin, pr);
        inProcess.store(false);
        return status;
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
    static bool normalizePatch(SurgeSynthesizer &s)
    {
        auto &patch = s.storage.getPatch();
        // No ceiling on note shift or pitch bend range (Jacob's decision; the instrument this
        // plugin matches stops at +-48 semitones of note shift per Part and per Performance,
        // and bend ranges of -48 to +24): the scene pitch reaches +-127 semitones, the bend up
        // range 127 semitones and the bend down range -127 to +127 (negative inverts it). The
        // engine has no limit on either; Surge's patch loader reads a bend up range outside
        // 0-255 as a legacy value, so it stays positive. Ranges in cents (extended) keep Surge's.
        // Set after every load, which resets the bend ranges.
        auto &scene0 = patch.scene[0];
        scene0.pitch.val_min.f = -127.f;
        scene0.pitch.val_max.f = 127.f;
        if (!scene0.pbrange_up.extend_range)
            scene0.pbrange_up.val_max.i = 127;
        if (!scene0.pbrange_dn.extend_range)
        {
            scene0.pbrange_dn.val_min.i = -127;
            scene0.pbrange_dn.val_max.i = 127;
        }
        for (auto &lfo : patch.scene[0].lfo)
            if (lfo.shape.val.i == lt_formula)
                return false;
        patch.scenemode.val.i = sm_single;
        patch.scene_active.val.i = 0;
        patch.fx_disable.val.i = (1 << n_fx_slots) - 1;
        // The engine options that match the reference instrument (no attack overshoot,
        // amplitude velocity law, algebraic saturators) come with the patch: off unless it
        // turns them on, as a converted Part does.
        return true;
    }

    // A voice takes its parameters from the scene data, which the engine refreshes from the
    // patch at the start of each of its blocks; a note at the start of the first block after a
    // load would otherwise start with the previous patch's values (LFO start phases, which are
    // read only when a voice starts, stay wrong for the whole note). Audio excluded.
    static void refreshSceneData(SurgeSynthesizer &s)
    {
        auto &patch = s.storage.getPatch();
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
            ok = normalizePatch(*synth);
            if (!ok && !lastGood.empty())
            {
                synth->loadRaw(lastGood.data(), (int)lastGood.size(), false);
                normalizePatch(*synth);
            }
            refreshSceneData(*synth);
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

    const clap_host_preset_load_t *hostPresetLoad{nullptr};
    const clap_host_params_t *hostParams{nullptr};

    // A Surge .fxp file as the whole patch (CLAP preset-load; Oscairn loads it into a spare
    // instance). Refused, the patch stays and the host is told why. Main thread.
    bool loadPreset(uint32_t kind, const char *location, const char *loadKey)
    {
        std::vector<uint8_t> data;
        const char *why = nullptr;
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location || loadKey)
            why = "Oscairn Analog loads presets from Surge patch files only";
        else if (!readFxp(location, &data) || data.size() < 8 ||
                 std::memcmp(data.data(), "sub3", 4) != 0)
            why = "Not a Surge patch file";
        else if (!loadStateBytes(data.data(), data.size()))
            why = "Patch refused: it uses a Formula modulator";
        if (why)
        {
            if (hostPresetLoad)
                hostPresetLoad->on_error(host, kind, location, loadKey, 0, why);
            return false;
        }
        if (hostParams)
            hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        if (hostPresetLoad)
            hostPresetLoad->loaded(host, kind, location, nullptr);
        return true;
    }

    // --- sound contexts -------------------------------------------------------------

    // A synthesizer with the plugin's ranges and rules and the default patch, at the current
    // rate, seeded as at activation. Main thread.
    std::unique_ptr<SurgeSynthesizer> makeSynth()
    {
        SurgeStorage::SurgeStorageConfig config;
        config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        auto s = std::make_unique<SurgeSynthesizer>(&layer, config, voiceStore.get());
        s->voiceChooser = this;
        s->storage.saveFixedTempo = true;
        auto &scene = s->storage.getPatch().scene[0];
        // Envelope segments up to 2^8 s: the instrument this plugin matches reaches 44 s of
        // attack (10-90 %), past Surge's 2^5 s. Only the range grows; the envelope code has no
        // limit, and modulation depths are in seconds-exponent units, so existing patches
        // sound the same.
        for (auto &eg : scene.adsr)
            for (auto *t : {&eg.a, &eg.d, &eg.r})
                t->val_max.f = longestEnvelopeTime;
        // The same for the voice LFOs, which serve as the pitch, oscillator and modifier
        // envelopes. A release past 5 only plays in a patch with lfoLongRelease (a converted
        // Part): otherwise 5 and above still never release, as in Surge.
        for (int l = 0; l < n_lfos_voice; l++)
            for (auto *t : {&scene.lfo[l].attack, &scene.lfo[l].decay, &scene.lfo[l].release})
                t->val_max.f = longestEnvelopeTime;
        // Portamento up to just under the engine's limit of 2^4 s (Surge's range stops at
        // 2^2): the instrument this plugin matches glides an octave in 8.9 s (10-90 %) at time
        // 120.
        scene.portamento.val_max.f = 3.99f;
        // Amp velocity sensitivity under the amplitude law down to k = 128 (-12 k dB): the
        // instrument this plugin matches reaches k = 32 at +252, and at +255 only velocity
        // 127 sounds.
        scene.vca_velsense.val_min.f = -1536.f;
        // (note shift and pitch bend ranges: in normalizePatch, as a load resets the bend
        // ranges)
        s->setSamplerate((float)sampleRate);
        normalizePatch(*s);
        refreshSceneData(*s);
        s->storage.perform_queued_wtloads();
        s->seedRandom(renderSeed);
        s->audio_processing_active = active;
        return s;
    }
    // Loads patch data into a synthesizer as a state load does; false if it is refused.
    static bool loadPatchInto(SurgeSynthesizer &s, const std::vector<uint8_t> &state)
    {
        if (state.size() < 8 || std::memcmp(state.data(), "sub3", 4) != 0)
            return false;
        s.loadRaw(state.data(), (int)state.size(), false);
        if (!normalizePatch(s))
            return false;
        refreshSceneData(s);
        s.storage.perform_queued_wtloads();
        return true;
    }
    static std::vector<uint8_t> patchOf(SurgeSynthesizer &s)
    {
        void *data = nullptr;
        auto size = s.saveRaw(&data);
        return {(uint8_t *)data, (uint8_t *)data + size};
    }
    void install(Context &c, std::unique_ptr<SurgeSynthesizer> s)
    {
        auto &sl = slots[&c - contexts.data()];
        // (its audio flags are the audio thread's; unpinned, it leaves them unset)
        sl.owned = std::move(s);
        sl.synth = sl.owned.get();
        sl.bus.store(c.patch.bus, std::memory_order_relaxed);
    }
    void discarded(Context &c)
    {
        auto &sl = slots[&c - contexts.data()];
        sl.synth = nullptr;
        sl.owned.reset();
        sl.bus.store(0, std::memory_order_relaxed);
        c.patch = {};
    }

    // prepare: the default patch with the supplied parameter values (by parameter ID, in the
    // parameters' own units); no patch text.
    static uint32_t prepare(const clap_plugin_t *p, const char *name,
                            const oscairn_sound_parameter_t *supplied, uint32_t count,
                            const char *patch, const oscairn_asset_ref_t *base, char *message,
                            uint32_t capacity)
    {
        auto fail = [&](const char *why) {
            if (message && capacity)
                std::snprintf(message, capacity, "%s", why);
            return CLAP_INVALID_ID;
        };
        if (patch || base)
            return fail("Oscairn Analog takes parameter values, not patch text");
        if (!name || std::strlen(name) > 63)
            return fail("a sound name of at most 63 bytes is needed");
        if (count && !supplied)
            return fail("no parameter values");
        auto &s = self(p);
        auto *c = s.freeSlot();
        if (!c)
            return fail("no free sound context");
        std::vector<std::pair<const ParamInfo *, double>> set;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto pi = s.find(supplied[i].id, nullptr);
            auto v = supplied[i].value;
            bool repeated = std::any_of(set.begin(), set.end(),
                                        [&](auto &e) { return e.first == pi; });
            if (!pi || repeated || !std::isfinite(v) || v < pi->min || v > pi->max)
                return fail("unknown, repeated or out-of-range parameter value");
            set.push_back({pi, v});
        }
        auto synth = s.makeSynth();
        c->patch.bus = 0;
        for (auto &[pi, v] : set)
            if (pi->bus)
                c->patch.bus = (uint32_t)std::lround(v);
            else
                setParamIn(*synth, *pi, v);
        refreshSceneData(*synth);
        s.install(*c, std::move(synth));
        return s.publish(*c, name);
    }
    // prepare_preset: a Surge .fxp file, as preset-load takes it.
    bool readPreset(uint32_t kind, const char *location, const char *loadKey, Patch &patch,
                    std::string &error)
    {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || loadKey)
        {
            error = "Oscairn Analog loads presets from Surge patch files only";
            return false;
        }
        if (!readFxp(location, &patch.state) || patch.state.size() < 8 ||
            std::memcmp(patch.state.data(), "sub3", 4) != 0)
        {
            error = "Not a Surge patch file";
            return false;
        }
        return true;
    }
    // A preset's sound, or a snapshot's (oscairn.sound-snapshots), already built by readPatch.
    // Each synthesizer has its own tables; only the store of voices is shared.
    bool buildPreset(Context &c, std::string &error)
    {
        if (c.patch.built)
        {
            install(c, std::move(c.patch.built));
            return true;
        }
        auto synth = makeSynth();
        if (!loadPatchInto(*synth, c.patch.state))
        {
            error = "Patch refused: it uses a Formula modulator";
            return false;
        }
        install(c, std::move(synth));
        return true;
    }

    // The snapshot: every sound's patch data, in hex on its line.
    static bool saveSnapshot(const clap_plugin_t *p, const clap_ostream_t *stream)
    {
        auto &s = self(p);
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (s.contexts[i].prepared)
            {
                s.contexts[i].patch.state = patchOf(*s.slots[i].synth);
                s.contexts[i].patch.bus = s.slots[i].bus.load(std::memory_order_relaxed);
            }
        bool ok = Base::saveSnapshot(p, stream);
        for (auto &c : s.contexts)
            c.patch.state = {};
        return ok;
    }
    // oscairn.sound-snapshots draft 4: one sound saved as a snapshot of its own (the helper's
    // saveSound, rendering excluded), from its synthesizer's patch.
    static bool saveSound(const clap_plugin_t *p, uint32_t id, const clap_ostream_t *stream)
    {
        auto &s = self(p);
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (s.contexts[i].prepared && s.contexts[i].id == id)
            {
                s.contexts[i].patch.state = patchOf(*s.slots[i].synth);
                s.contexts[i].patch.bus = s.slots[i].bus.load(std::memory_order_relaxed);
            }
        bool ok = Base::saveSound(p, id, stream);
        for (auto &c : s.contexts)
            c.patch.state = {};
        return ok;
    }
    void alone(Patch &patch) { patch.bus = 0; } // on the main output
    // Draft 4: a sound's values, read from its synthesizer, which no channel selects, so the
    // audio thread doesn't touch it. Main thread.
    uint32_t patchValues(const Patch &patch, oscairn_sound_parameter_t *out, uint32_t capacity)
    {
        for (uint32_t i = 0; i < contextCapacity; ++i)
            if (&contexts[i].patch == &patch)
            {
                auto &sy = *slots[i].synth;
                for (uint32_t k = 0; k < params.size() && k < capacity; ++k)
                    out[k] = {params[k].id, valueIn(i, params[k])};
                return (uint32_t)params.size();
            }
        return 0;
    }
    bool writePatch(std::ostream &out, const Patch &patch)
    {
        static const char hex[] = "0123456789abcdef";
        std::string line(2 * patch.state.size() + 1, ' ');
        for (size_t i = 0; i < patch.state.size(); ++i)
        {
            line[1 + 2 * i] = hex[patch.state[i] >> 4];
            line[2 + 2 * i] = hex[patch.state[i] & 15];
        }
        out << ' ' << patch.bus << line; // its Output Bus, then its patch data
        return !patch.state.empty();
    }
    // Reading a sound builds its synthesizer, so a patch that would be refused refuses the
    // whole snapshot before anything changes; restored() or buildPreset installs it. It touches
    // only the new synthesizer, so it can run while the plugin renders (prepareSnapshot).
    void snapshotLoading(bool starting) { quiet = starting; }
    bool readPatch(std::istream &in, Patch &patch)
    {
        std::string text;
        if (!(in >> patch.bus) || patch.bus > soundBuses || !(in >> text) || text.size() % 2)
            return false;
        auto nibble = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        patch.state.resize(text.size() / 2);
        for (size_t i = 0; i < patch.state.size(); ++i)
        {
            int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            patch.state[i] = (uint8_t)(hi << 4 | lo);
        }
        auto synth = makeSynth();
        if (!loadPatchInto(*synth, patch.state))
            return false;
        patch.built = std::move(synth);
        return true;
    }
    void restored()
    {
        for (uint32_t i = 0; i < contextCapacity; ++i)
        {
            auto &c = contexts[i];
            auto &sl = slots[i];
            sl.ringing = sl.silent = sl.fresh = sl.choking = false;
            sl.bus.store(c.patch.bus, std::memory_order_relaxed);
            sl.outBus = c.patch.bus;
            sl.busFade = 0;
            if (!c.prepared)
            {
                sl.synth = nullptr;
                sl.owned.reset();
                continue;
            }
            if (i == 0)
            {
                // The default sound keeps its synthesizer, which the parameters point into.
                synth->allNotesOff();
                loadPatchInto(*synth, c.patch.state);
                lastGood = c.patch.state;
            }
            else
            {
                sl.owned = std::move(c.patch.built);
                sl.synth = sl.owned.get();
            }
            c.patch = {};
        }
        for (size_t i = 0; i < params.size(); ++i)
            values[i].store(valueIn(selected[0], params[i]), std::memory_order_relaxed);
    }

    // oscairn.memory: what one synthesizer holds without its voices, a sound each (15 MiB:
    // medians of three preparations measured 12.8-14.1 MiB on macOS; the tests keep it at or above
    // the median and within 25 % of it). The voices' store is the instance's.
    static uint64_t synthBytes() { return uint64_t(15) << 20; }
    uint32_t sounds() const
    {
        uint32_t n = 0;
        for (auto &c : contexts)
            n += c.prepared;
        return n;
    }
};

Plugin &self(const clap_plugin_t *p) { return *static_cast<Plugin *>(p->plugin_data); }

// --- extensions -------------------------------------------------------------------------

// Main, then a sound bus per Output Bus value (Oscairn gives each part sharing the instance one).
uint32_t portsCount(const clap_plugin_t *, bool isInput) { return isInput ? 0 : soundBuses + 1; }
bool portsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info)
{
    if (isInput || index > soundBuses)
        return false;
    info->id = index;
    if (index == 0)
        std::snprintf(info->name, sizeof(info->name), "Main");
    else
        std::snprintf(info->name, sizeof(info->name), "Sound bus %u", index);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
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
    const auto bus = (unsigned)std::lround(std::clamp(value, pi->min, pi->max));
    auto text = pi->bus           ? (bus ? "Sound bus " + std::to_string(bus) : std::string("Main"))
                : pi->deactivation ? std::string(value > 0.5 ? "Off" : "On")
                                   : pi->p->get_display(true, Plugin::to01(*pi, value));
    std::snprintf(out, size, "%s", text.c_str());
    return true;
}
bool paramsTextToValue(const clap_plugin_t *p, clap_id id, const char *text, double *value)
{
    auto &s = self(p);
    auto pi = s.find(id, nullptr);
    if (pi && pi->bus)
    {
        unsigned b = 0;
        std::string t(text);
        if (t == "Main")
            *value = 0;
        else if (std::sscanf(text, "Sound bus %u", &b) == 1 && b >= 1 && b <= soundBuses)
            *value = b;
        else
            return false;
        return true;
    }
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
                 const clap_output_events_t *out)
{
    auto &s = self(p);
    // Called on the main thread when not processing, or on the audio thread instead of process.
    s.inProcess.store(true);
    if (s.loading.load())
        s.backedOut.store(true);
    else
    {
        s.applySelections(out);
        for (uint32_t i = 0; i < in->size(in); ++i)
            s.applyParam(in->get(in, i));
    }
    s.inProcess.store(false);
}
const clap_plugin_params_t paramsExt{paramsCount,       paramsInfo,        paramsValue,
                                     paramsValueToText, paramsTextToValue, paramsFlush};

// oscairn.ui: a role for the parameters that have one; the host decides the rest.
bool uiParamHint(const clap_plugin_t *p, clap_id id, oscairn_param_hint_t *hint)
{
    auto pi = self(p).find(id, nullptr);
    if (!pi || pi->role == OSCAIRN_ROLE_NONE)
        return false;
    *hint = {pi->bus ? (uint32_t)OSCAIRN_WIDGET_LIST : (uint32_t)OSCAIRN_WIDGET_AUTO, 0,
             CLAP_INVALID_ID, 0, pi->role};
    return true;
}
const oscairn_plugin_ui_t uiExt{uiParamHint};

// The CLAP state: a line naming the format and the default sound's Output Bus, then its patch.
constexpr const char *stateHeader = "oscairn-analog-state-1";
bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream)
{
    auto &s = self(p);
    auto head = std::string(stateHeader) + " " +
                std::to_string(s.slots[0].bus.load(std::memory_order_relaxed)) + "\n";
    auto bytes = s.saveStateBytes();
    bytes.insert(bytes.begin(), head.begin(), head.end());
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
    // The header line, then Surge's "sub3" patch data. Patch data alone (a converted Part's
    // state) plays on the main output.
    uint32_t bus = 0;
    size_t at = 0;
    const auto n = std::strlen(stateHeader);
    if (bytes.size() > n && !std::memcmp(bytes.data(), stateHeader, n))
    {
        auto end = std::find(bytes.begin(), bytes.end(), (uint8_t)'\n');
        unsigned b = 0;
        if (end == bytes.end() ||
            std::sscanf(std::string(bytes.begin() + n, end).c_str(), " %u", &b) != 1 ||
            b > soundBuses)
            return false;
        bus = b;
        at = size_t(end - bytes.begin()) + 1;
    }
    if (bytes.size() < at + 8 || std::memcmp(bytes.data() + at, "sub3", 4) != 0)
        return false;
    auto &s = self(p);
    if (!s.loadStateBytes(bytes.data() + at, bytes.size() - at))
        return false;
    s.slots[0].bus.store(bus, std::memory_order_relaxed);
    s.values[s.indexById[fnv1a("oscairn_output_bus")]].store(s.valueIn(0, s.params.back()),
                                                            std::memory_order_relaxed);
    return true;
}
const clap_plugin_state_t stateExt{stateSave, stateLoad};

bool presetLoadFromLocation(const clap_plugin_t *p, uint32_t kind, const char *location,
                            const char *loadKey)
{
    return self(p).loadPreset(kind, location, loadKey);
}
const clap_plugin_preset_load_t presetLoadExt{presetLoadFromLocation};

const clap_plugin_state_t snapshotExt{Plugin::saveSnapshot, Plugin::loadSnapshot};
const oscairn_plugin_sound_snapshots_t soundSnapshotsExt{
    Plugin::prepareSnapshot, Plugin::setRingOut, Plugin::chokeRetiring, Plugin::saveSound,
    Plugin::soundValues};
const oscairn_plugin_sound_contexts_t soundContextsExt{
    Plugin::capacity, Plugin::get,     Plugin::prepare,      Plugin::select,
    Plugin::discard,  &snapshotExt,    Plugin::preparePreset};

// oscairn.memory: a synthesizer per prepared sound. The default sound is the context; the
// state's other sounds and the voices' store count as shared.
bool memoryEstimate(const clap_plugin_t *p, double, uint32_t, oscairn_memory_t *out)
{
    auto &s = self(p);
    out->shared_bytes = sizeof(Plugin) + sizeof(SurgeSynthesizer::VoiceStore) +
                        Plugin::synthBytes() * (s.sounds() - 1);
    out->context_bytes = Plugin::synthBytes();
    out->asset_bytes = 0;
    return true;
}
bool memoryCurrent(const clap_plugin_t *p, oscairn_memory_t *out)
{
    return memoryEstimate(p, 0, 0, out);
}
const oscairn_plugin_memory_t memoryExt{memoryEstimate, memoryCurrent};

// oscairn.voice-budget: Surge's voices across every sound (see Plugin's voice budget section).
// Every voice multiplies its amp by its note's volume expression.
bool budgetHonorsVolume(const clap_plugin_t *) { return true; }
void budgetSetLimits(const clap_plugin_t *p, uint32_t limit,
                     const oscairn_voice_budget_context_t *contexts, uint32_t count)
{
    self(p).budget.set(limit, contexts, count);
}
uint32_t budgetActiveVoices(const clap_plugin_t *p, double *cost)
{
    if (cost)
        *cost = 0; // no estimate: the host measures the instance
    return self(p).countVoices();
}
uint32_t budgetNotes(const clap_plugin_t *p, oscairn_voice_budget_note_t *notes, uint32_t capacity)
{
    return self(p).writeNotes(notes, capacity);
}
const oscairn_plugin_voice_budget_t voiceBudgetExt{budgetHonorsVolume, budgetSetLimits,
                                                   budgetActiveVoices, budgetNotes};

// Preset discovery: Surge's factory and user patch folders, found as Surge finds them, each
// .fxp file in them one preset named after the file.
const clap_preset_discovery_provider_descriptor_t providerDescriptor{
    CLAP_VERSION_INIT, "org.oscairn.analog.patches", "Oscairn Analog patches", "Oscairn"};

struct Provider
{
    clap_preset_discovery_provider_t provider{};
    const clap_preset_discovery_indexer_t *indexer{nullptr};
};

bool providerInit(const clap_preset_discovery_provider_t *p)
{
    auto indexer = static_cast<Provider *>(p->provider_data)->indexer;
    SurgeStorage::SurgeStorageConfig config;
    config.suppliedDataPath = SurgeStorage::skipPatchLoadDataPathSentinel;
    config.createUserDirectory = false;
    config.scanWavetableAndPatches = false;
    auto storage = std::make_unique<SurgeStorage>(config);
    clap_preset_discovery_filetype_t fxp{"Surge XT patch", nullptr, "fxp"};
    if (!indexer->declare_filetype(indexer, &fxp))
        return false;
    auto factory = path_to_string(storage->datapath / "patches_factory");
    auto user = path_to_string(storage->userPatchesPath);
    clap_preset_discovery_location_t locations[] = {
        {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory patches",
         CLAP_PRESET_DISCOVERY_LOCATION_FILE, factory.c_str()},
        {CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "User patches",
         CLAP_PRESET_DISCOVERY_LOCATION_FILE, user.c_str()}};
    for (auto &l : locations)
    {
        std::error_code ec;
        if (fs::is_directory(string_to_path(l.location), ec) &&
            !indexer->declare_location(indexer, &l))
            return false;
    }
    return true;
}
void providerDestroy(const clap_preset_discovery_provider_t *p)
{
    delete static_cast<Provider *>(p->provider_data);
}
bool providerGetMetadata(const clap_preset_discovery_provider_t *, uint32_t kind,
                         const char *location,
                         const clap_preset_discovery_metadata_receiver_t *receiver)
{
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location || !readFxp(location, nullptr))
    {
        receiver->on_error(receiver, 0, "Not a Surge patch file");
        return false;
    }
    auto name = path_to_string(string_to_path(location).stem());
    if (!receiver->begin_preset(receiver, name.c_str(), nullptr))
        return true;
    clap_universal_plugin_id_t id{"clap", descriptor.id};
    receiver->add_plugin_id(receiver, &id);
    return true;
}
const void *providerGetExtension(const clap_preset_discovery_provider_t *, const char *)
{
    return nullptr;
}

uint32_t presetFactoryCount(const clap_preset_discovery_factory_t *) { return 1; }
const clap_preset_discovery_provider_descriptor_t *
presetFactoryDescriptor(const clap_preset_discovery_factory_t *, uint32_t i)
{
    return i == 0 ? &providerDescriptor : nullptr;
}
const clap_preset_discovery_provider_t *
presetFactoryCreate(const clap_preset_discovery_factory_t *,
                    const clap_preset_discovery_indexer_t *indexer, const char *id)
{
    if (!indexer || std::strcmp(id, providerDescriptor.id))
        return nullptr;
    auto s = new Provider;
    s->indexer = indexer;
    s->provider = {&providerDescriptor, s, providerInit, providerDestroy, providerGetMetadata,
                   providerGetExtension};
    return &s->provider;
}
const clap_preset_discovery_factory_t presetFactory{presetFactoryCount, presetFactoryDescriptor,
                                                    presetFactoryCreate};

// --- plugin -----------------------------------------------------------------------------

bool pluginInit(const clap_plugin_t *p)
{
    auto &s = self(p);
    s.hostParams = static_cast<const clap_host_params_t *>(
        s.host->get_extension(s.host, CLAP_EXT_PARAMS));
    s.hostPresetLoad = static_cast<const clap_host_preset_load_t *>(
        s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD));
    if (!s.hostPresetLoad)
        s.hostPresetLoad = static_cast<const clap_host_preset_load_t *>(
            s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD_COMPAT));
    s.synth = s.makeSynth();
    s.slots[0].synth = s.synth.get();
    s.buildParams();
    s.lastGood = s.saveStateBytes();
    return true;
}
void pluginDestroy(const clap_plugin_t *p) { delete &self(p); }
// Ends every note and tail without the synthesizers reporting it (activation, reset).
void silence(Plugin &s)
{
    s.quiet = true;
    s.resetVoices();
    s.quiet = false;
    for (uint32_t i = 0; i < contextCapacity; ++i)
    {
        s.slots[i].choking = false;
        s.slots[i].outBus = s.slots[i].bus.load(std::memory_order_relaxed);
        s.slots[i].busFade = 0;
        for (bool *pin : {&s.slots[i].ringing, &s.slots[i].fresh})
            if (*pin)
            {
                *pin = false;
                s.contexts[i].users.fetch_sub(1, std::memory_order_release);
            }
    }
    s.blockPos = BLOCK_SIZE;
}
// A synthesizer as a fresh one is after activation, its patch kept (activation, reset): at once,
// where Surge's allSoundOff() fades over 8 blocks and then stops any note struck meanwhile.
// Audio thread: nothing here allocates or locks.
void clear(SurgeSynthesizer &s)
{
    for (auto &c : s.channelState)
        std::memset(&c, 0, sizeof(c)); // bend, pressure and RPN state too, as constructed
    s.stopSound(); // voices, waiting notes, keys, pedals, hold buffers, the scene's filters
    s.approachingAllSoundOff = false;
    s.mpeEnabled = false; // only ever turned on by RPN here
    auto &st = s.storage;
    auto &patch = st.getPatch();
    std::memset(st.poly_aftertouch, 0, sizeof(st.poly_aftertouch));
    for (int sc = 0; sc < n_scenes; ++sc)
    {
        s.midiKeyPressedForScene[sc].fill(0); // the lowest, highest and latest key
        st.last_key[sc] = 60; // where portamento glides from
        auto &ms = patch.scene[sc].modsources;
        for (auto m : {ms_pitchbend, ms_modwheel, ms_breath, ms_expression, ms_sustain,
                       ms_aftertouch, ms_lowest_key, ms_highest_key, ms_latest_key})
        {
            auto *c = static_cast<ControllerModulationSource *>(ms[m]);
            const float to = c->target[0];
            c->init(0.f);
            // The patch keeps the mod wheel's position (it is saved with it): it glides there
            // from 0, as after a load.
            if (m == ms_modwheel)
                c->set_target(to);
        }
        for (auto m : {ms_alternate_bipolar, ms_alternate_unipolar})
            static_cast<AlternateModulationSource *>(ms[m])->state = false;
        // Scene LFOs run every block, voices or not, and keep their phase into the next note:
        // set up again as the synthesizer's constructor does (reseeded below).
        for (int l = 0; l < n_lfos_scene; ++l)
            static_cast<LFOModulationSource *>(ms[ms_slfo1 + l])
                ->assign(&st, &patch.scene[sc].lfo[n_lfos_voice + l], patch.scenedata[sc], 0,
                         &patch.stepsequences[sc][n_lfos_voice + l],
                         &patch.msegs[sc][n_lfos_voice + l],
                         &patch.formulamods[sc][n_lfos_voice + l]);
        // which modulators run is worked out at each block; a voice started before the first
        // block reads it as constructed
        std::fill(std::begin(patch.scene[sc].modsource_doprocess),
                  std::end(patch.scene[sc].modsource_doprocess), false);
    }
    // The output gains glide in from 0 over a fresh synthesizer's first blocks.
    for (auto *g : {&s.amp, &s.amp_mute})
        g->set_target_instant(0.f);
    Plugin::refreshSceneData(s); // the patch's values, without the last block's modulation
    s.seedRandom(renderSeed);
    s.time_data.ppqPos = st.songpos = 0; // a free-running LFO starting before the first block
}
bool pluginActivate(const clap_plugin_t *p, double rate, uint32_t, uint32_t)
{
    auto &s = self(p);
    s.sampleRate = rate;
    s.setSampleRate(rate);
    silence(s);
    for (auto &sl : s.slots)
        if (sl.synth)
        {
            sl.synth->setSamplerate((float)rate);
            clear(*sl.synth);
            sl.synth->audio_processing_active = true;
        }
    s.stale = 0;
    s.active = true;
    return true;
}
void pluginDeactivate(const clap_plugin_t *p)
{
    auto &s = self(p);
    s.active = false;
    for (auto &sl : s.slots)
        if (sl.synth)
            sl.synth->audio_processing_active = false;
}
bool pluginStartProcessing(const clap_plugin_t *) { return true; }
void pluginStopProcessing(const clap_plugin_t *) {}
void pluginReset(const clap_plugin_t *p)
{
    auto &s = self(p);
    // the sounds this thread may touch, before silence() unpins the tails
    const auto m = s.live();
    for (uint32_t i = 0; i < contextCapacity; ++i)
        if (m >> i & 1)
            clear(*s.slots[i].synth);
    s.stale = ~m & ((1u << contextCapacity) - 1);
    silence(s);
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
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
        return &presetLoadExt;
    if (!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS))
        return &soundContextsExt;
    if (!std::strcmp(id, OSCAIRN_EXT_SOUND_SNAPSHOTS))
        return &soundSnapshotsExt;
    if (!std::strcmp(id, OSCAIRN_EXT_MEMORY))
        return &memoryExt;
    if (!std::strcmp(id, OSCAIRN_EXT_VOICE_BUDGET))
        return &voiceBudgetExt;
    if (!std::strcmp(id, OSCAIRN_EXT_UI))
        return &uiExt;
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
    if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID))
        return &factory;
    if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
        !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
        return &presetFactory;
    return nullptr;
}
} // namespace

const clap_plugin_entry_t oscairn_analog_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit,
                                                  entryFactory};
