/*
 * oscairn-analog-bench: times every block of the Oscairn Analog plugin playing a Surge patch
 * file, as Oscairn plays it (preset-load, 48 kHz, 128-frame blocks): the blocks before any
 * note, one held note, `notes` held notes, and the slowest blocks with their page faults.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026, Oscairn contributors. This file is MIT-licensed; the executable links
 * Surge XT and is GPL-3.0-or-later as a whole.
 *
 *   oscairn-analog-bench patch.fxp [notes 8] [seconds per phase 2] [block 128]
 *
 * notes -1: only the blocks before any note; 0: those and one held note (for profiling).
 * OSCAIRN_BENCH_HELD=1: the blocks before any note and the chord held alone.
 * OSCAIRN_BENCH_MLOCK=1: memory locked first (mlockall), as Oscairn's workers run.
 */

#include "OscairnAnalog.h"

#include <oscairn-clap/sound-contexts.h>

#include <clap/clap.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <vector>

namespace
{
const clap_host_t host = {
    CLAP_VERSION_INIT,
    nullptr,
    "oscairn-analog-bench",
    "Oscairn",
    "",
    "0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

long faults()
{
    rusage u{};
#ifdef RUSAGE_THREAD
    getrusage(RUSAGE_THREAD, &u);
#else
    getrusage(RUSAGE_SELF, &u);
#endif
    return u.ru_minflt + u.ru_majflt;
}

// This thread's CPU time: unlike the wall clock it leaves out time the thread was preempted.
double cpuUs()
{
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

struct Block
{
    double us, cpu;
    long faults;
    const char *phase;
    int index;
};
// A block carrying note events, apart so a profiler can collect these blocks alone
// (valgrind --tool=callgrind --toggle-collect=processEventBlock).
__attribute__((noinline)) void processEventBlock(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    plugin->process(plugin, pr);
}
// A block without events (--toggle-collect=processQuietBlock).
__attribute__((noinline)) void processQuietBlock(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    plugin->process(plugin, pr);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: oscairn-analog-bench patch.fxp [notes] [seconds] [block]\n");
        return 2;
    }
    const int chord = argc > 2 ? std::atoi(argv[2]) : 8;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 2;
    const uint32_t frames = argc > 4 ? (uint32_t)std::atoi(argv[4]) : 128;
    const double rate = 48000;
    // OSCAIRN_BENCH_FTZ=1: denormals flushed to zero, as stock Surge XT's processBlock sets them
    // (aarch64 only here), to see what they cost and whether the output changes
    const bool ftz = std::getenv("OSCAIRN_BENCH_FTZ") != nullptr;
    if (std::getenv("OSCAIRN_BENCH_MLOCK") && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::perror("mlockall");
#if defined(__aarch64__)
    if (ftz)
    {
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ULL << 24)));
    }
#endif
    uint64_t hash = 1469598103934665603ULL; // FNV-1a over the output's bits

    auto factory = static_cast<const clap_plugin_factory_t *>(
        oscairn_analog_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    auto plugin = factory->create_plugin(factory, &host, "org.oscairn.analog");
    if (!plugin || !plugin->init(plugin) || !plugin->activate(plugin, rate, 1, frames) ||
        !plugin->start_processing(plugin))
        return 1;
    // OSCAIRN_BENCH_CONTEXT=1: the patch as a prepared sound context selected on every channel
    // (Oscairn's Performance path), else preset-load into the default sound
    const auto loadStart = std::chrono::steady_clock::now();
    if (std::getenv("OSCAIRN_BENCH_CONTEXT"))
    {
        auto ctx = static_cast<const oscairn_plugin_sound_contexts_t *>(
            plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        char why[256] = {};
        const uint32_t id = ctx ? ctx->prepare_preset(plugin, "bench", CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                                      argv[1], nullptr, why, sizeof why)
                                : CLAP_INVALID_ID;
        if (id == CLAP_INVALID_ID || !ctx->select(plugin, id, -1))
        {
            std::fprintf(stderr, "the plugin refused %s: %s\n", argv[1], why);
            return 1;
        }
    }
    else
    {
        auto presets = static_cast<const clap_plugin_preset_load_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
        if (!presets ||
            !presets->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, argv[1], nullptr))
        {
            std::fprintf(stderr, "the plugin refused %s\n", argv[1]);
            return 1;
        }
    }

    const double loadUs = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - loadStart)
                              .count();

    std::vector<float> left(frames), right(frames);
    std::vector<clap_event_note_t> pending;
    std::vector<Block> blocks;
    blocks.reserve(size_t(8 * seconds * rate / frames) + 16); // no growth between blocks
    int32_t nextId = 1;
    auto run = [&](const char *phase, double secs) {
        const int n = (int)(secs * rate / frames);
        for (int b = 0; b < n; ++b)
        {
            std::vector<const clap_event_header_t *> evs;
            for (auto &e : pending)
                evs.push_back(&e.header);
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
            clap_output_events_t out{nullptr, [](const clap_output_events_t *,
                                                 const clap_event_header_t *) { return true; }};
            float *ch[2] = {left.data(), right.data()};
            clap_audio_buffer_t buf{};
            buf.data32 = ch;
            buf.channel_count = 2;
            clap_process_t pr{};
            pr.frames_count = frames;
            pr.audio_outputs = &buf;
            pr.audio_outputs_count = 1;
            pr.in_events = &in;
            pr.out_events = &out;
            const long f0 = faults();
            const double c0 = cpuUs();
            const auto t0 = std::chrono::steady_clock::now();
            if (evs.empty())
                processQuietBlock(plugin, &pr);
            else
                processEventBlock(plugin, &pr);
            const double us =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count();
            blocks.push_back({us, cpuUs() - c0, faults() - f0, phase, b});
            for (uint32_t k = 0; k < frames; ++k)
                for (float x : {left[k], right[k]})
                {
                    uint32_t bits;
                    std::memcpy(&bits, &x, 4);
                    hash = (hash ^ bits) * 1099511628211ULL;
                }
            pending.clear();
        }
    };
    auto notes = [&](uint16_t type, int count) {
        for (int k = 0; k < count; ++k)
        {
            clap_event_note_t e{};
            e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
            e.note_id = type == CLAP_EVENT_NOTE_ON ? nextId++ : -1;
            e.channel = 0;
            e.key = (int16_t)(48 + 3 * k);
            e.velocity = 0.8;
            pending.push_back(e);
        }
    };

    run("before any note", seconds);
    if (chord < 0) // the blocks before any note alone (profiling the cost at rest)
        return std::printf("output hash %016llx\n", (unsigned long long)hash), 0;
    if (chord == 0) // one held note alone (profiling a voice)
    {
        notes(CLAP_EVENT_NOTE_ON, 1);
        run("1 note", seconds);
        return std::printf("output hash %016llx\n", (unsigned long long)hash), 0;
    }
    if (std::getenv("OSCAIRN_BENCH_HELD")) // the chord held alone (profiling voices)
    {
        notes(CLAP_EVENT_NOTE_ON, chord);
        run("held", seconds);
    }
    else
    {
        notes(CLAP_EVENT_NOTE_ON, 1);
        run("1 note", seconds);
        notes(CLAP_EVENT_NOTE_OFF, 1);
        run("released", seconds);
        notes(CLAP_EVENT_NOTE_ON, 1);
        run("1 note again", seconds);
        notes(CLAP_EVENT_NOTE_OFF, 1);
        run("released again", seconds);
        notes(CLAP_EVENT_NOTE_ON, chord);
        run("chord", seconds);
        notes(CLAP_EVENT_NOTE_OFF, chord);
        run("chord released", seconds);
    }

    std::printf("output hash %016llx%s\n", (unsigned long long)hash, ftz ? " (flush to zero)" : "");
    // OSCAIRN_BENCH_BLOCKS=file: every block's time, one line each (phase, index, us, faults,
    // thread CPU us)
    if (const char *path = std::getenv("OSCAIRN_BENCH_BLOCKS"))
        if (FILE *f = std::fopen(path, "w"))
        {
            for (auto &b : blocks)
                std::fprintf(f, "%s\t%d\t%.2f\t%ld\t%.2f\n", b.phase, b.index, b.us, b.faults, b.cpu);
            std::fclose(f);
        }
    const double budget = frames / rate * 1e6;
    std::printf("%s: %u-frame blocks (%.0f us), %d-note chord; load (main thread) %.0f us\n",
                argv[1], frames, budget, chord, loadUs);
    if (!blocks.empty())
    {
        auto w = std::max_element(blocks.begin(), blocks.end(),
                                  [](auto &a, auto &b) { return a.us < b.us; });
        auto wc = std::max_element(blocks.begin(), blocks.end(),
                                   [](auto &a, auto &b) { return a.cpu < b.cpu; });
        std::printf("  longest block %.1f us (%s, block %d); most CPU %.1f us (%s, block %d)\n",
                    w->us, w->phase, w->index, wc->cpu, wc->phase, wc->index);
    }
    const char *last = nullptr;
    for (size_t i = 0; i <= blocks.size(); ++i)
    {
        if (i < blocks.size() && last && blocks[i].phase == last)
            continue;
        if (last)
        {
            std::vector<double> t;
            long f = 0;
            size_t first = i;
            while (first > 0 && blocks[first - 1].phase == last)
                --first;
            for (size_t k = first; k < i; ++k)
            {
                t.push_back(blocks[k].us);
                f += blocks[k].faults;
            }
            auto worst = std::max_element(blocks.begin() + first, blocks.begin() + i,
                                          [](auto &a, auto &b) { return a.us < b.us; });
            auto worstCpu = std::max_element(blocks.begin() + first, blocks.begin() + i,
                                             [](auto &a, auto &b) { return a.cpu < b.cpu; });
            std::sort(t.begin(), t.end());
            std::printf("  %-16s first %7.1f us, median %7.1f, p99 %7.1f, worst %7.1f (block %d, "
                        "%ld faults); %ld faults in all; worst CPU %7.1f (block %d)\n",
                        last, blocks[first].us, t[t.size() / 2], t[t.size() * 99 / 100],
                        worst->us, worst->index, worst->faults, f, worstCpu->cpu, worstCpu->index);
        }
        if (i < blocks.size())
            last = blocks[i].phase;
    }
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    return 0;
}
