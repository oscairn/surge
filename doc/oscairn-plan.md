# Oscairn extension plan: an analog-modelling engine on Surge XT

Status: plan, 2026-10-30. Milestone 0 is being implemented on branch `oscairn`; section 10
records what was built, how it was tested and where it departs from this plan. This fork is
[oscairn/surge](https://github.com/oscairn/surge), forked from
[surge-synthesizer/surge](https://github.com/surge-synthesizer/surge) at `5a4828a`.

The goal: extend Surge XT so that one instance can do everything a Yamaha MONTAGE M / MODX M AN-X
Part can do, and ship it as an Oscairn CLAP instrument that supports all of the host's
recommended features (`docs/plugin-porting.md` in [Oscairn](https://github.com/jbschooley/oscairn)). Yamaha
names appear in this document only as references to what we are matching (see
[Naming](#8-licence-and-naming)).

File paths are relative to this repository unless they start with `oscairn/`. Statements about
Surge code were checked against this checkout; anything not checked is marked **unverified**.

## 1. Goal, scope and relation to RFC 0015

### Goal

An analog engine with:

- the AN-X Part voice: 3 oscillators, noise, 2 filters, a wave folder, an amplifier, a pitch EG
  and LFO, an EG and LFO for every oscillator, filter, folder and amp section, voice-level unison,
  OSC reset, drift and ageing, and the Part's pan, delay, mono/poly and portamento behaviour;
- a lossless conversion path from an AN-X Part (from the MONTAGE M / MODX M Data List's
  parameter tables) to the engine's state;
- Oscairn's recommended behaviour: CLAP notes with note IDs and `NOTE_END`, deterministic and
  portable state, clear parameter metadata, native sound contexts, real-time safety, a Linux
  ARM64 build within a CPU budget, and the Oscairn test suite.

Out of scope here: the insert and system effects of a Performance (Oscairn RFC 0016), the
Performance-level controller layer (Super Knob, scenes, controller boxes as host features), and
any DSP or FPGA backend (RFC 0004). Section 5 says which Part data goes where.

### What forking Surge changes compared with RFC 0015

RFC 0015 recommended that Oscairn write its own voice and modulation structure and borrow only
the Surge team's DSP libraries (`sst-filters`, `sst-waveshapers`, `sst-basic-blocks`). Jacob has
now decided to fork Surge itself. In plain terms:

| Topic | RFC 0015 | With the fork |
| --- | --- | --- |
| Voice and modulation structure | New, written in Oscairn | Surge's: `SurgeVoice`, scenes, the modulation matrix, ADSR and LFO sources |
| Oscillators, filters, shapers | Borrowed from sst-* | The same libraries, plus Surge's oscillators (`src/common/dsp/oscillators/`) |
| Patch model and state | Oscairn's | Surge's patch XML (`src/common/SurgePatch.cpp`), carried in CLAP state |
| Where the code lives | In Oscairn (AGPL) | In this fork, GPL-3.0, loaded as a separate CLAP plugin (RFC 0001's plugin boundary) |
| What we get beyond AN-X | Only what we write | Surge's whole engine: more oscillators, filters, MSEG, a mature modulation matrix, effects |
| Cost | Writing a voice from scratch | Fitting Oscairn's rules (determinism, real-time state loading, sound contexts, metadata) into a large existing engine |
| DSP/FPGA backends (RFC 0004) | Designed in from the start | CPU only. A hardware backend would reimplement blocks; not planned |

RFC 0015's concern was "a second voice and state architecture inside Oscairn". The plugin
boundary answers most of it: the engine is a CLAP plugin and its state is opaque to the host.
RFC 0015 should be updated to record the decision (an Oscairn change, not made here).

### Recommendation: extend Surge's scene, don't add a parallel voice mode

Two ways to put an AN-X Part into Surge:

**A. Map onto Surge's existing scene and extend it.** One AN-X Part becomes one Surge scene.
Most of the topology already exists (section 3). Missing blocks are added to the scene as new
parameters appended after the current ones, all defaulting to "off" so existing patches render
exactly as before.

- Costs: new parameters touch `SurgeSceneStorage` (`src/common/SurgeStorage.h`), parameter
  registration in `SurgePatch.cpp`, patch revision and migration (`ff_revision`, currently 31),
  the GUI layout and skin connectors, and Surge's tests. The per-voice modulator count has to grow
  (see below), which is the largest single change.
- Gains: one voice to maintain, every Surge feature stays available to converted sounds, and
  generic extensions can go upstream.

**B. A dedicated "fixed-architecture analog" voice mode.** A new voice class beside
`SurgeVoice` with AN-X's fixed sections, each with its own built-in EG and LFO, selected per
scene.

- Costs: a second voice implementation (`SurgeVoice.cpp` alone is about 1,800 lines), a second
  parameter block and GUI, two code paths through the voice manager, and the modulation matrix
  has to be wired into both. Little of it could go upstream.
- Gains: AN-X's per-section modulators fit naturally, and the parameter set is exactly AN-X's,
  which makes the converter and a generated editor simpler.

**Recommendation: A.** The gaps are additive (section 3), and Surge's voice already takes its
scene and parameter block by pointer (`SurgeVoice::SurgeVoice(SurgeStorage*, SurgeSceneStorage*,
pdata *params, ...)`, `src/common/dsp/SurgeVoice.h`), which helps with sound contexts. The
deciding factor is the per-voice modulator budget:

| Per voice | AN-X needs | Surge has |
| --- | --- | --- |
| Envelopes | 7: oscillator 1–3, pitch, filter, folder, amp | 2 ADSRs (amp and filter, `n_egs = 2`) |
| LFOs | 7: oscillator 1–3, pitch, filter, folder, amp | 6 voice LFOs (`n_lfos_voice = 6`); each can run as an envelope (`lt_envelope`) |
| Part-level LFO | 1 | 6 scene LFOs (`n_lfos_scene = 6`) |

The amp and filter EGs map to Surge's two ADSRs. The other 5 EGs and 7 LFOs need 12 voice
modulators, so `n_lfos_voice` must go from 6 to 12. New modulation sources are appended at the
end of `enum modsources` (`src/common/ModulationSource.h`) so saved routings keep their numbers;
the existing `modsource_display_order` table already separates display order from enum order.
Each LFO adds 13 parameters per scene (`LFOStorage`), so this adds 78 per scene. If that
proves too invasive, B becomes the fallback, and the cost table above is the comparison to
revisit.

### Recommended shape of the Oscairn build

- **A separate CLAP target** for Oscairn (proposed `src/oscairn-clap/`), written directly against
  the CLAP API over the `surge-common` library, instead of the JUCE processor in `src/surge-xt/`.
  Section 4 lists what the JUCE path gets wrong for Oscairn (ports, parameter metadata,
  velocity-0 notes, state loading on the audio thread) and that it does not offer sound contexts.
  The upstream JUCE plugin stays as it is.
- **One scene, no Surge effect slots** in that target. A converted AN-X Part needs one scene;
  Part insert effects map to Oscairn effects (RFC 0016). This keeps CPU, memory and the
  parameter list small, and makes a sound context cheap. Surge's effects can be offered to
  Oscairn separately, through Surge XT Effects (`src/surge-fx/`), as RFC 0016 candidates.
- **No Lua** in that target (`SURGE_SKIP_LUA`, see section 4).

## 2. How Surge is structured today

### Synth, storage and patch

| Piece | Where | Notes |
| --- | --- | --- |
| Synth engine | `src/common/SurgeSynthesizer.{h,cpp}` | Voice allocation (`playNote`, `playVoice`, `releaseNote`, `chokeNote`), per-block `process()`, `NOTE_END` bookkeeping (`notifyEndedNote`) |
| Storage | `src/common/SurgeStorage.{h,cpp}` | Patch, tables, wavetables, tuning, data paths, RNG. Constants at the top of `SurgeStorage.h` |
| Patch | `src/common/SurgePatch.cpp` | Parameter registration (`assign(...)` calls), XML load and save, migrations |
| Parameters | `src/common/Parameter.{h,cpp}` | Control types (`ct_*`) define ranges, display and extendability |
| Modulation sources | `src/common/ModulationSource.h` | `enum modsources`, 8 macros (`n_customcontrollers = 8`) |

Structure constants (`src/common/SurgeStorage.h`, `src/common/globals.h`): 2 scenes, 3
oscillators per scene, 2 filter units per scene, 6 voice and 6 scene LFOs, 2 ADSRs, 16 effect
slots, 273 parameters per scene, `MAX_VOICES = 64`, and an internal block size fixed at compile
time (`SURGE_COMPILE_BLOCK_SIZE`).

Scene modes are Single, Key Split, Dual and Channel Split. Play modes are Poly, Mono, Mono
(Single Trigger), Mono (Fingered Portamento), Mono (Single Trigger & Fingered Portamento) and
Latch.

### The scene (`SurgeSceneStorage`, `src/common/SurgeStorage.h`)

- `osc[3]` (`OscillatorStorage`): type, pitch, octave, 7 type-specific parameters `p[]`,
  keytrack, retrigger.
- Scene pitch and octave, FM depth and FM routing, drift, noise colour, keytrack root.
- Mixer: level, mute, solo and route for oscillators 1–3, noise, ring 1×2 and ring 2×3, plus a
  pre-filter gain (`level_pfg`). Routes are Filter 1, Both, Filter 2 (`ct_oscroute`,
  `Parameter.cpp`).
- Amp: `vca_level`, `vca_velsense`, pan, width, volume, sends.
- `filterunit[2]` (type, subtype, cutoff, resonance, envmod, keytrack), filter 2 offset and
  resonance link, `wsunit` (one waveshaper: type, drive), filter configuration, balance, feedback,
  highpass.
- `adsr[2]` (amp and filter EG: A, D, S, R, three shapes, digital/analog mode) and `lfo[12]`
  (`LFOStorage`: rate, shape, phase, amplitude, deform, trigger mode, unipolar, and a DAHDSR:
  delay, hold, attack, decay, sustain, release).
- Modulation routings: `modulation_voice` and `modulation_scene` vectors.

### Voice signal flow (`src/common/dsp/SurgeVoice.cpp`)

`SurgeVoice::process_block` renders oscillator 3, then 2, then 1, in that order so FM sources
exist when their targets run (`fm_switch`: Off, 2 > 1, 3 > 2 > 1, 2 > 1 < 3; one depth,
`fm_depth`, `ct_decibel_fmdepth`, −48 to +24 dB). Each oscillator's output is scaled by its mixer
level and added to the filter 1 input, the filter 2 input or both, according to its route. Ring
1×2 and 2×3 are mixer channels computed from oscillator outputs by `all_ring_modes_block`, with
ring modulation and several "continuous XOR" modes (`CombinatorMode`). Noise (legacy or tilt
colour) follows. The pre-filter gain is applied and the voice's two inputs are written into a
quad of 4 voices for the filter chain.

Oscillators are created with placement new into preallocated voice memory (`spawn_osc`,
`src/common/dsp/Oscillator.cpp`). Per-voice pan is applied from the voice's modulated parameter
copy (`SurgeVoice.cpp`, around line 869), so pan can be modulated per voice.

### Filter chain (`src/common/dsp/QuadFilterChain.cpp`)

`ProcessFBQuad<config, A, WS, B>` processes 4 voices at once with SIMD. In `fc_serial1`:
filter 1 → waveshaper → mix → add the filter-2-routed input → filter 2 → mix → amp gain (the amp
EG and VCA level, computed in `SurgeVoice::SetQFB`). Serial 2 and 3, Dual 1 and 2, Stereo, Ring
and Wide change the order, feedback and stereo handling. The waveshaper sits after filter 1 in
serial modes and after the filter mix in dual modes. There is no path from an oscillator to the
amp that bypasses both filters.

### DSP libraries (`libs/sst/`)

- **sst-filters** (`libs/sst/sst-filters/include/sst/filters/FilterConfiguration.h`): LP 12/24,
  HP 12/24, BP 12/24, notch, legacy and vintage ladders (ladder subtypes 6/12/18/24 dB), OB-Xd 2-
  and 4-pole (4-pole subtypes 6/12/18/24 dB), K35, diode ladder, cutoff-warp and resonance-warp
  LP/HP/BP/notch/allpass (1–4 stages with tanh, soft clip, OJD or sine saturators), tri-pole,
  Cytomic SVF, and the OB-Xd Xpander (LP1–4, HP1–3, BP2, BP4, notch, phaser and combinations).
- **sst-waveshapers** (`libs/sst/sst-waveshapers/include/sst/waveshapers/WaveshaperConfiguration.h`):
  soft, hard, asymmetric, sine, digital, Chebyshev, rectifiers, single, dual and west-coast
  folders, additive harmonics, fuzzes, trigonometric shapes.
- **sst-basic-blocks**: SIMD helpers, `DriftLFO` and unison character
  (`include/sst/basic-blocks/dsp/OscillatorDriftUnisonCharacter.h`), `RNG` (`dsp/RNG.h`).
- **sst-effects**: effect implementations used by Surge's FX.

### Oscillators (`src/common/dsp/oscillators/`)

Classic, Sine, Wavetable, S&H Noise, Audio Input, FM3, FM2, Window, Modern, String, Twist, Alias.
The relevant ones:

- **Classic** (`ClassicOscillator.h`): shape, width 1, width 2, sub mix, sync, unison detune,
  unison voices. Band-limited impulse convolution; sync is a pitch amount (`ct_syncpitch`, 0–60
  semitones) against an internal leader; random start phase unless retrigger is on.
- **Modern** (`ModernOscillator.h`): saw mix, pulse mix, triangle mix (the third slot can be
  triangle, square or sine), pulse width, sync, unison detune, unison voices.
- **Sine** (`SineOscillator.h`): shape, feedback, FM behaviour, low cut, high cut, unison.

Unison in Surge is inside the oscillator (stacked oscillator voices), not a duplicated voice.
Drift is per oscillator voice, driven by `DriftLFO` and scaled by the scene's drift parameter.

### Modulation

Voice sources: velocity, release velocity, keytrack, poly aftertouch, timbre, amp EG, filter EG,
voice LFOs 1–6, random (bipolar and unipolar), alternate (bipolar and unipolar). Scene sources:
pitch bend, mod wheel, channel aftertouch, breath, expression, sustain, macros 1–8, scene LFOs 1–6,
lowest/highest/latest key. Routings are applied per block into the voice's parameter copy
(`SurgeVoice::applyModulationToLocalcopy`). A routing has one source and one depth; nothing
scales a routing's depth by a second source. LFO types: sine, triangle, square, sawtooth, noise,
sample & hold, envelope, step sequencer, MSEG, formula (Lua).

### Plugin wrapper and notes

- The plugin is a JUCE `AudioProcessor` (`src/surge-xt/SurgeSynthProcessor.{h,cpp}`) wrapped as
  CLAP by `libs/clap-juce-extensions` (CLAP ID `org.surge-synth-team.surge-xt`,
  `src/surge-xt/CMakeLists.txt`).
- It uses CLAP direct processing (`clap_direct_process`, `SurgeSynthProcessor.cpp` around line
  1135). Events are applied at internal block boundaries: an event is processed at the start of
  the internal block that contains it.
- CLAP note on, off and choke carry note IDs into `playNote`, `releaseNote` and `chokeNote`.
  Voices store `host_note_id` and the originating key and channel. When a voice finishes,
  `notifyEndedNote` queues it and the wrapper emits `CLAP_EVENT_NOTE_END` with the original
  note ID, key and channel (around line 1241).
- Note expressions (volume, pan, tuning, etc.) and polyphonic parameter modulation
  (`CLAP_EVENT_PARAM_MOD` with note ID, key, channel) are supported. Voice info reports
  overlapping-note support.
- Tests: `src/surge-testrunner/` (Catch2) including `UnitTestsNOTEID.cpp`, `UnitTestsVOICE.cpp`,
  `UnitTestsGOLDEN.cpp` (fixed renders compared with a 1e-5 tolerance) and `UnitTestsIO.cpp`.

## 3. Gap table against an AN-X Part

Source: every row of the AN-X Part tables of the MONTAGE M Data List (parameter blocks
`4p 00`–`4p 05` and `7p 00`), plus the Part-common rows an AN-X Part uses (mono/poly,
portamento, Part LFO). Status:

- **Exists**: a Surge parameter already does it; the converter maps the value.
- **Matrix**: done with modulation routings (and, for EGs and LFOs, the extra voice modulators
  from section 1).
- **Extend**: an existing Surge block needs a new option or range.
- **New**: a new block.
- **Converter**: no engine change; the converter folds it into other values.

"Velocity sensitivity" below means velocity scaling a static value (a Matrix routing from
velocity), unless it scales a *modulation depth* (an EG depth): Surge cannot do that, so those
rows need the routing "via" extension (scale a routing by a second source, see Extend list).
Every AN-X time, level, speed and cutoff value is a 0–255, 0–511, 0–415 or 0–1023 index whose
curve is not published; the converter needs fitted tables for all of them (section 6, milestone
1).

### Common block (`4p 00 00`)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Random Pan Depth | Matrix | `ms_random_bipolar` → scene pan (per voice) |
| Alternate Pan Depth | Matrix | `ms_alternate_bipolar` → pan. Confirm Surge's alternation rule matches by measurement |
| Scaling Pan Depth | Matrix | keytrack → pan. Centre key to measure |
| Key On Delay Time Length, Tempo Sync Switch, Note Length | New | Note delay in the voice manager (section 4: must keep note IDs and `NOTE_END` correct for delayed and cancelled notes) |
| Unison (Off, 2, 4) | New | Voice-level unison: start 2 or 4 voices per note. `playNote` already passes a `detune` to the voice, but as a whole-semitone `char`; it needs a fractional value |
| Unison Detune (0–15), Unison Spread (0–15) | New | Per-unison-voice pitch and pan offsets; curves measured |
| OSC Reset (Off, Phase, Tune, Full) | Extend | Per-oscillator `retrigger` covers phase. Add drift and ageing reset for Tune, both for Full |
| Voltage Drift (0–127) | Extend | Scene `drift` exists; model and depth to fit by measurement |
| Ageing (−100 to +100) | New | Per-voice, per-component static spread (pitch, cutoff, levels) seeded per voice slot. Measure first (RFC 0015 open question) |
| Pitch EG Attack, Decay, Sustain, Release Time | Matrix | A voice LFO in envelope mode → oscillator pitch, one routing per oscillator |
| Pitch EG Time Velocity Sensitivity | Matrix | Velocity → that envelope's time parameters |
| Pitch LFO Wave (Saw, Square, Triangle, Sine, Random) | Exists | Voice LFO shapes sawtooth, square, triangle, sine, sample & hold (or noise; choose by measurement) |
| Pitch LFO Speed (0–415) | Exists | LFO rate, through a fitted table |
| Pitch LFO Key On Reset | Exists | Trigger mode Keytrigger or Freerun |
| Pitch LFO Phase (16 steps) | Exists | LFO start phase |
| Pitch LFO Delay Time, Fade In Time | Exists | LFO delay and attack |
| Noise Generator Tone | Exists | Noise colour (tilt); curve measured |
| Noise Generator Out Select (Filter, Amp) | Extend | New mixer route value "post-filter" (see Extend list) |
| Noise Generator Out Level, Level Velocity Sensitivity | Exists, Matrix | Noise mixer level; velocity → level |
| Filter Cutoff EG Attack, Decay, Sustain, Release Time | Exists | Surge's filter ADSR (one EG for both filters, depth per filter, as in AN-X) |
| Filter Cutoff EG Time Velocity Sensitivity | Matrix | Velocity → filter ADSR times |
| Filter Cutoff LFO Wave, Speed, Key On Reset, Phase, Delay Time, Fade In Time | Matrix | A voice LFO, routed to both cutoffs with the per-filter depths |
| Amplitude Level | Exists | `vca_level` |
| Amplitude Level Velocity Sensitivity | Exists | `vca_velsense`; curve measured |
| Amplitude Level LFO Depth | Matrix | Amp LFO → VCA level |
| Amplitude Level Key Follow | Matrix | Keytrack → VCA level |
| Amplitude Saturator Drive (0–60 dB) | New | A saturator at the amp stage; position (before or after the VCA) to measure |
| Amplitude EG Attack, Decay, Sustain, Release Time | Exists | Surge's amp ADSR |
| Amplitude EG Time Velocity Sensitivity | Matrix | Velocity → amp ADSR times |
| Amplitude LFO Wave, Speed, Key On Reset, Phase, Delay Time, Fade In Time | Matrix | A voice LFO → VCA level |

### Oscillator switches (`4p 01 0o`, × 3)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Oscillator Controller Set 1–32 Switch | Converter | Only emit controller routings to this oscillator where the switch is on |
| Oscillator Key Controller Set 1–4 Switch | Converter | Same for key controllers |
| LFO Box 1–3 Switch | Converter | Same for the Part LFO's three destinations |

### Oscillator (`4p 02 0o`, × 3)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Oscillator Wave (Saw1, Saw2, Square, Triangle, Sine) | Extend | Modern covers saw, pulse, triangle and sine. A new oscillator type ("Analog", derived from Modern) adds the second saw, the wave shaper and the AN-X sync behaviour in one place. Saw2's shape is measured |
| Oscillator Octave (64'–1') | Exists | Oscillator octave (`ct_pitch_octave`, −3 to +3): 64' = −3, 8' = 0, 1' = +3 |
| Oscillator Pitch (±1200 cents) | Exists | Oscillator pitch (semitones, extendable range) |
| Pitch EG Depth (±4800 cents) | Matrix | Pitch envelope → this oscillator's pitch |
| Pitch EG Depth Velocity Sensitivity | Extend | Needs routing "via" velocity |
| Pitch LFO Depth (±4800 cents) | Matrix | Pitch LFO → this oscillator's pitch (confirm in the manual that this is the pitch LFO, not the oscillator's own LFO) |
| Self Sync Pitch (0–4800 cents) | Exists | Classic and Modern sync (`ct_syncpitch`, 0–60 semitones). Behaviour measured |
| Self Sync Pitch Velocity Sensitivity | Matrix | Velocity → sync |
| Self Sync EG Depth, LFO Depth | Matrix | Oscillator EG and LFO → sync |
| Pulse Width (1–99%), Velocity Sensitivity, EG Depth, LFO Depth | Exists, Matrix | Modern pulse width; velocity, oscillator EG and LFO → width |
| Wave Shaper (0–255), Velocity Sensitivity, EG Depth, LFO Depth | New, Matrix | Per-oscillator shaping inside the new oscillator type. Not available on Square in AN-X. What it does is measured |
| FM Level (0–255; oscillators 1 and 2) | Extend | Per-link FM depth instead of the scene's single `fm_depth`. Which oscillator modulates which is measured or taken from the manual |
| FM Level Velocity Sensitivity | Matrix | Velocity → the per-link depth |
| Ring Level (0–255; oscillators 1 and 2), Velocity Sensitivity | Extend, Matrix | Ring 1×2 and 2×3 mixer channels exist. Whether AN-X's ring level is a separate channel or crossfades the oscillator is measured |
| Out Select (Filter, Amp) | Extend | Post-filter mixer route |
| Out Invert Enable | Extend | Polarity switch per mixer channel |
| Out Level (0–511), Velocity Sensitivity | Exists, Matrix | Mixer level (dB in Surge; table) |
| Oscillator EG Attack, Decay, Sustain, Release Time | Matrix | One voice LFO in envelope mode per oscillator, routed to that oscillator's PW, shaper and sync |
| Oscillator LFO Wave, Speed, Key On Reset, Phase, Delay Time, Fade In Time | Matrix | One voice LFO per oscillator, routed the same way |
| Part LFO Destination 1–3 Depth Ratio | Converter | Multiply into the Part LFO routing depth for this oscillator |

### Filter switches (`4p 03 0f`, × 2)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Filter Controller Box 1–32 Switch, Key Controller Box 1–4 Switch, LFO Box 1–3 Switch | Converter | As for the oscillator switches |

### Filter (`4p 04 0f`, × 2)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Filter Type: Thru | Exists | Filter Off |
| LPF24, LPF18, LPF12, LPF6 | Exists | Candidates: OB-Xd 4-pole subtypes 24/18/12/6 dB, Xpander LP4/LP3/LP2/LP1, ladder 24/18/12/6 dB. Choose one family by frequency-response and resonance measurements so all slopes share a character |
| HPF24, HPF18, HPF12, HPF6 | Exists | HP 24 dB, Xpander HP3/HP2/HP1. HP 18 and HP 6 exist only in the Xpander; check its resonance behaviour |
| BPF12, BPF6 | Exists | BP 12 dB, Xpander BP2/BP4. What "12" and "6" mean for a band-pass is measured |
| (if no family fits) | Extend | One multimode filter with all 11 responses, in sst-filters |
| Filter Cutoff (0–1023) | Exists | Cutoff, through a fitted table |
| Cutoff Velocity Sensitivity | Matrix | Velocity → cutoff |
| Cutoff EG Depth (±9600 cents) | Exists | `envmod` (`ct_freq_mod`, ±96 semitones): exact range match |
| Cutoff EG Depth Velocity Sensitivity | Extend | Routing "via", or velocity → `envmod` (works because `envmod` is a parameter) |
| Cutoff LFO Depth (±9600 cents) | Matrix | Filter LFO → cutoff |
| Cutoff Key Follow (Off, 1/3, 1/2, 2/3, 1, 2 oct) | Extend | `keytrack` is ±100% (`ct_percent_bipolar`) and not extendable. "2" needs 200% |
| Resonance (0–255), Velocity Sensitivity | Exists, Matrix | Resonance; not available on LPF6 and HPF6 |
| Saturator Drive (0–60 dB), Velocity Sensitivity | New, Matrix | A saturator per filter. Position (before or after the filter) measured. sst-filters' driven subtypes are a fallback |
| Out Level (±12 dB) | Extend | Output gain per filter unit |
| Part LFO Destination 1–3 Depth Ratio | Converter | As for oscillators |
| (topology) | Exists | AN-X's filter order is not in the Data List; the defaults (filter 2 an HPF at cutoff 0) suggest series. Surge's Serial 1 fits if measurement confirms |

### Wave folder (`4p 05 00`)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Wave Folder (0–255), Velocity Sensitivity, EG Depth, LFO Depth | New, Matrix | A folder stage separate from Surge's waveshaper, at AN-X's position (measured; likely before the filters) |
| Wave Folder Texture (0–255) | New | Measured |
| Wave Folder Type (Soft, Hard) | New | Start from sst-waveshapers' folders; fit to measurements |
| Modifier EG Attack, Decay, Sustain, Release Time, Time Velocity Sensitivity | Matrix | A voice envelope → folder amount |
| Modifier LFO Wave, Speed, Key On Reset, Phase, Delay Time, Fade In Time | Matrix | A voice LFO → folder amount |

### Ring / FM control (`7p 00 00`)

| AN-X parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Oscillator 1 Ring / FM, Oscillator 2 Ring / FM (Ring 255 – 0 – FM 255) | Converter (open) | Its relation to the oscillator block's FM and Ring Level is not documented in the Data List. Find out from the manual or by measurement before mapping |

### Part-level parameters an AN-X Part uses (`1p 00 01`–`07`)

| Parameter | Status | Surge mapping or work |
| --- | --- | --- |
| Mono/Poly Mode | Exists | Play mode Poly or Mono (with single-trigger variants) |
| Portamento Switch, Time | Exists | Scene portamento |
| Portamento Mode (Fingered, Full-time) | Exists | Mono (Fingered Portamento) vs Mono |
| Portamento Time Mode (Rate 1, Time 1, Rate 2, Time 2) | Extend | Surge has a constant-rate option; four modes to measure |
| Pan | Exists | Scene pan |
| Pitch Bend Range Upper, Lower | Exists | `pbrange_up`, `pbrange_dn` |
| Velocity Sensitivity Depth, Offset | Extend | A velocity curve at the scene input (Surge has none) |
| Part LFO: wave (13 incl. User), speed, tempo speed, phase, delay, fade in, hold, fade out, key on reset (Off, Each-On, 1st-On), 3 destinations and depths, random speed depth | Exists, Matrix | A scene LFO (or a voice LFO for Each-On reset). User LFO → step sequencer (16 steps). Delay, fade in, hold and fade out → the LFO's DAHDSR. 1st-On → Surge's "first note only" retrigger mode. The 13 waves are checked one by one |

### Summary of engine work

Extend:

1. Voice modulators 6 → 12 (`n_lfos_voice`).
2. Routing "via": scale a routing by a second source (velocity), for EG-depth velocity
   sensitivity.
3. Mixer: a post-filter route and a polarity switch for each channel.
4. Per-link FM depth.
5. Filter units: output gain, keytrack range to 200%.
6. Drift: reset modes; fit the model.
7. Portamento time modes; a scene velocity curve.

New:

1. "Analog" oscillator type (second saw, wave shaper, AN-X sync behaviour).
2. Saturators: one per filter and one at the amp.
3. Wave folder stage with soft/hard and texture.
4. Voice-level unison 2/4 with detune and spread.
5. Ageing.
6. Key-on delay.

## 4. Oscairn integration

Checked against `oscairn/docs/plugin-porting.md`. "Surge today" means the JUCE CLAP build in
`src/surge-xt/`. "Plan" is the Oscairn CLAP target recommended in section 1.

### Required checks at load

| Requirement | Surge today | Plan |
| --- | --- | --- |
| Plugin factory, complete interfaces | Provided by clap-juce-extensions | Write the factory and all interface functions; covered by Oscairn's scanner test |
| Audio or note outputs | Stereo output | Same |
| ≤ 64 channels, no empty ports, fixed layout | Main stereo out, an enabled stereo **Sidechain input**, optional Scene A and B outputs (`SurgeSynthProcessor.cpp` lines 79–83) | One stereo output, no input. Fixed. Oscairn's instance pool for sound switching only takes instruments with no audio input, so the input has to go |
| ≤ 16 note ports | One note port; MIDI, MPE and CLAP dialects | One port, CLAP preferred, MIDI and MPE accepted |
| Parameters: unique IDs, finite min ≤ default ≤ max, ≤ 32,768 | IDs from storage names. Every parameter is reported as 0–1 with default 0 (`getDefaultValue()` returns 0, marked FIXME, `SurgeSynthProcessor.h`). Valid, but defaults are wrong. About 776 parameters by the constants (219 global + 2 × 273 scene + 1, plus 8 macros and bypass); count it in a test | Real units and real defaults. One scene and no effect slots gives about 273 + new parameters (about 78 for the extra LFOs plus the new blocks). Well under the limit |
| State and audio-ports extensions complete | Yes, through the wrapper | Yes |
| Activates at host rate and max block | Yes; internal processing is in fixed blocks | Same |
| Linux ARM64 build | `cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake` exists; SIMD goes through SIMDe on ARM. Not built here (**unverified**) | CI job for Linux ARM64 and macOS |

### Recommended behaviour

**Real-time safety.** Conflicts found:

- Patch loading runs on the audio thread. `SurgeSynthesizer::enqueuePatchForLoad` is "safe from
  any thread", but `processEnqueuedPatchIfNeeded` is "only safe from audio thread"
  (`SurgeSynthesizer.h`) and is called from `SurgeSynthesizer::process`. It calls `loadRaw`,
  which parses XML, allocates, calls `stopSound()` and reloads effects
  (`SurgeSynthesizerIO.cpp`). Plan: parse and build a context off the audio thread, then swap a
  pointer at a block boundary (this is the sound-contexts design below).
- The JUCE path updates JUCE's virtual-keyboard state and notifies its listeners for every CLAP
  note on the audio thread (`process_clap_event`). Not needed in the Oscairn target.
- Oscillator and voice construction use placement new into preallocated memory (good). The rest
  of `process()` must be checked under an allocation guard (milestone 0).
- Lua: the Formula LFO type and wavetable scripts run LuaJIT; Formula modulators evaluate during
  processing. Lua's garbage collector allocates. Build the Oscairn target with
  `SURGE_SKIP_LUA` (`src/common/CMakeLists.txt`) and reject patches that use Formula.

**File access.** `SurgeStorage`'s constructor resolves data and user paths and can scan
patches and wavetables (`SurgeStorage.cpp` from line 71); a `skipLoadWtAndPatch` path exists.
`configuration.xml` is an embedded resource. `surge-common` also links SQLite (patch database)
and ODDSound MTS-ESP (`SURGE_SKIP_ODDSOUND_MTS` exists). Plan: construct storage with patch and
wavetable loading skipped, don't create the patch database, skip MTS-ESP, and never touch files
in `process`. The Convolution effect and user wavetables need files; they are out of the
Oscairn target (no effect slots; wavetable oscillators may be allowed later through Oscairn's
assets extension, RFC 0009).

**GUI dependency.** The JUCE plugin carries JUCE's GUI. `surge-common` links a `surge-juce`
target privately (`src/common/CMakeLists.txt`); the `SURGE_SKIP_JUCE_FOR_RACK` option suggests a
JUCE-free core build exists for VCV Rack (**unverified** that it builds without JUCE). Oscairn
does not show plugin windows yet; the Oscairn target has no GUI. Editing comes from parameter
metadata, and later from an editor description.

**State: deterministic and independent of architecture.** Surge's state is a `sub3` header, the
patch XML and optional wavetables (`SurgePatch::save_patch`). Integers in the header and
wavetables are written little-endian explicitly; floats are written in the classic locale
(`float_to_clocalestr`, TinyXML's `SetDoubleAttribute`). That is already
architecture-independent. Still to do:

- Leave out host- and editor-specific "DAW extra state" (`populateDawExtraState`: editor state,
  MPE settings, tuning) or make it fixed in the Oscairn target, so the same sound always saves the
  same bytes.
- Keep ports, note dialect and parameter metadata independent of the loaded state.
- A byte-identical save → load → save round trip test.

**Deterministic rendering.** Needed for `oscairn-render` comparisons. Surge isn't deterministic
today:

- `SurgeStorage::RNGGen` seeds `std::minstd_rand` from the system clock (`SurgeStorage.h`
  around line 2148). It drives random oscillator start phases, S&H and random modulation
  sources.
- `sst::basic_blocks::dsp::RNG`'s default constructor also seeds from the clock (`RNG.h` line 38);
  Surge's `noiseRng` uses it.
- `DriftLFO` calls C `rand()` (`OscillatorDriftUnisonCharacter.h` lines 45 and 52), which is
  global and shared across instances.
- `std::uniform_*_distribution` output differs between libc++ and libstdc++, so even seeded
  output would differ between macOS and Linux.

Plan: one seedable RNG per context with our own integer-to-float conversion, reseeded on
activation and in a defined way per voice (voice counter, context ID). The same seed and input
then give the same samples on one machine. Bit-identical output across x86 and ARM is not a
goal: compilers contract floating point differently. Cross-architecture tests compare within a
tolerance.

**CLAP notes, note IDs and `NOTE_END`.** Already good: note IDs reach voices, `NOTE_END` is
emitted with the original ID, key and channel, choke is handled, and
`UnitTestsNOTEID.cpp` exists. To fix in the Oscairn target:

- A CLAP note-on with velocity 0 is treated as a release (`process_clap_event`). Oscairn's
  reference instruments keep it a note-on (RFC 0005). Follow the host's convention.
- Events are applied at the start of Surge's internal block (fixed at compile time), so an event
  can take effect up to one internal block early. Oscairn's tests use exact-sample note-ID
  releases. Choose: split internal blocks at event times, or document the quantum and set test
  tolerances to it. Splitting is better if it can be done without breaking the 4-voice SIMD quad
  and LFO and envelope block rates. Decide in milestone 0 after measuring.
- Unison and key-on delay: a note with 4 unison voices must still end with one `NOTE_END` for
  its ID, after the last voice. A delayed note released or choked before it starts must still
  end with a `NOTE_END`.
- Rendering does not depend on host block size: the internal block position carries across
  `process` calls. A 37-frame vs 512-frame identical-output test should therefore pass once RNG
  is seeded (**unverified**).

**Parameter metadata and module paths.**

| Item | Surge today | Plan |
| --- | --- | --- |
| Names | Clear (`getParameterName`, e.g. scene and section prefixes) | Neutral names (section 8), no scene prefix in a one-scene build |
| Module paths | JUCE groups by "clump", flattened into CLAP `module` by the wrapper | Explicit paths: `Oscillator 1`, `Oscillator 1/Envelope`, `Mixer`, `Filter 1`, `Folder`, `Amp`, `Pitch`, `Voice`, `LFO` |
| Value text | Yes (`get_display`) | Same, in real units |
| Stepped | Only booleans (wrapper) | All integer and choice parameters |
| Read-only | None | None expected |
| Ranges | All 0–1 | Real ranges, and the same whatever state is loaded |
| Polyphonic modulation | Supported | Keep; the Oscairn host doesn't send it yet (not in its supported list) |

**Native sound contexts (RFC 0005, `oscairn.sound-contexts/draft-4`).** Missing in Surge.
Design:

- A context is a scene's parameter block, modulation routings and modulator storage, plus its
  own DSP-side state (scene LFOs, smoothing). Up to 8 per instance, preallocated.
- `SurgeVoice` already takes `SurgeSceneStorage*` and `pdata*` at construction; a voice binds to
  its context at note-on and keeps it. `SurgeVoice.cpp` still calls `storage->getPatch()` in 6
  places; those must go through the voice's context.
- One shared voice pool across contexts, with Surge's stealing policy.
- Preparation from patch text (draft 3 allows engine-defined patch text): the Surge patch XML
  for one scene. Parse on the control thread, validate, and reject with a message on error.
  The selection queue applies at the next process boundary, before that call's events.
- Each context sums its voices separately, as RFC 0005 describes.
- The exclusive snapshot interface stores all slots and selections.
- Memory per context has to be measured. Tables in `SurgeStorage` (dB, envelope rates, pitch)
  are per instance and should be shared.

This is the largest Oscairn-specific piece and stays in the fork, not upstream.

**Other extensions.** Effect contexts: not for this instrument (they apply to a Surge effects
build, RFC 0016). Param-target: not needed (it's for sequencers). Sound assets: not needed
without wavetables or samples. Editor descriptions: planned in Oscairn but not defined; until
then, module paths and value text do the job. When it is defined, describe the section grid
(oscillators, mixer, filters, folder, amp, envelopes and LFOs per section).

**Linux ARM64 and CPU budget.** Surge processes voices in SIMD quads and offers
`ct_polylimit` (Polyphony Limit). Measure the cost of an AN-X-style voice (3 oscillators, 2
filters, folder, 3 saturators, 12 voice modulators) and of 4-voice unison on target-class
hardware. Oscairn's architecture mentions i.MX8M Mini and RK3588 class boards; which one is the
target is an Oscairn decision. Set polyphony from the measurement. Cheap paths:

- the Modern oscillator core (differentiated polynomial waveforms) instead of Classic's impulse convolution;
- skipping unused modulators and stages (Surge already skips muted oscillators and unused
  filter units through the `A`, `WS`, `B` template flags).

### Testing (Oscairn's list, applied to the fork)

| Test | Exists in Surge | Plan |
| --- | --- | --- |
| Deterministic offline render (`oscairn-render`) | Golden renders in `UnitTestsGOLDEN.cpp` with a tolerance | Seeded RNG, then `oscairn-render` comparisons per milestone; 37- and 512-frame blocks; 44.1, 48 and 96 kHz |
| Rendering never allocates | No | Allocation guard around `process` (copy Oscairn's reference test) |
| State round trip | `UnitTestsIO.cpp` tests patch and wavetable IO | Byte-identical save → load → save; old Surge patches render unchanged after migration |
| Worker process (`*-workers`) | No | Same tests in an Oscairn worker |
| Sound contexts | No | Copy `oscairn/core/engine/tests/sound_contexts.cpp` cases: held notes across switches, exact-sample note-ID releases, sustain, queue exhaustion, stale IDs, concurrent prepare/select/discard under TSan |
| ASan, UBSan, TSan | Not checked | CI jobs for the Oscairn target and the testrunner |
| Notes | `UnitTestsNOTEID.cpp` | Add unison and key-on-delay `NOTE_END` cases, velocity-0 note-on |
| Existing Surge tests | `src/surge-testrunner/` | Must stay green on every milestone |

## 5. Converting an AN-X Part

The converter is kept outside this fork, in its own private repository
(`oscairn/performance-converter`, open question 12). It reads the AN-X Part blocks of a
Performance (the Data List's `4p 00`–`4p 05` and `7p 00` blocks, and the Part-common blocks
`1p 00 01`–`1p 00 09`) and writes one scene of the engine's patch. Oscairn then prepares it as
a sound context from patch text.

| AN-X data | Engine state |
| --- | --- |
| Common block | Scene parameters: pan routings, voice unison, OSC reset, drift, ageing, amp; pitch EG and pitch LFO as voice modulators 1 and 2 with routings to the three oscillator pitches |
| Oscillator × 3 | Oscillator *n*: "Analog" type with wave, octave, pitch, sync, PW, wave shaper; mixer level, route, polarity; per-link FM depth; ring channel level. Oscillator EG and LFO as dedicated voice modulators with routings to PW, shaper and sync |
| Filter × 2 | Filter units 1 and 2 in Serial 1 (if confirmed): type chosen from the fitted model family, cutoff, resonance, `envmod`, keytrack, saturator, output gain. Filter EG = Surge filter ADSR. Filter LFO = a voice modulator routed to both cutoffs |
| Wave folder | Folder stage; modifier EG and LFO as voice modulators |
| Amp | `vca_level`, `vca_velsense`, amp saturator, amp ADSR, amp LFO routing |
| Velocity sensitivities | Velocity routings, or routing "via" for EG depths |
| Part LFO | Scene LFO 1 (voice LFO if Each-On reset) with up to three destinations; depth × each oscillator's or filter's destination ratio |
| Switch blocks | Decide which controller and LFO routings are written |
| Mono/poly, portamento, pitch bend | Scene play mode, portamento, bend ranges |
| Controller sets and boxes, Super Knob, scenes, insert effects, arpeggio, zones | Not engine state. Oscairn's Performance and controller layer, and Oscairn effects (RFC 0016) |

All index values (times, levels, speeds, cutoff, depths) go through fitted tables owned by the
converter (section 6). The engine stays generic: it doesn't know AN-X units. If a measured
response can't be reached by a table (for example a velocity *curve* rather than a scale), the
engine gets a generic option (a curve shape on the routing) rather than an AN-X special case.

Losslessness check: for each converted Part, an inverse mapping recovers every AN-X value from
the engine state (within each table's resolution). Run on Jacob's machine over the factory and
user AN-X Parts. The files and results stay out of every repository.

## 6. Milestones

Measurement follows RFC 0013's rules:

- A harness renders the Yamaha software offline with fixed settings, outside every repository.
- The same probe sounds render in the engine; features are compared by measurement (pitch in
  cents, envelope segment times and levels, filter magnitude response, LFO rate and shape, level
  in dB, drift statistics).
- Tables are fitted from these measurements. The harness and tables are our own work.
- Reference renders, Yamaha audio and Yamaha presets are never committed. Fixtures in tests are
  generated.
- Each feature has a written tolerance. Listening acceptance is Jacob's.

| # | Deliverable | Test gate |
| --- | --- | --- |
| 0 | Groundwork: Oscairn CLAP target (`src/oscairn-clap/`), one scene, no effects, no Lua; fixed stereo port; real parameter metadata; seeded RNG; velocity-0 handling; off-thread state preparation; Linux ARM64 and macOS CI | Oscairn scanner accepts it; byte-identical state round trip; identical renders in 37- and 512-frame blocks; no allocation while rendering; ASan/UBSan/TSan clean; all Surge testrunner tests pass |
| 1 | Measurement harness and fitted tables for EG times and levels, LFO speeds, cutoff, levels; filter family chosen for the 11 types; the converter's first version for oscillators, filters, amp and EGs | Per-feature tolerances met on swept probes (e.g. EG segment time within a set percentage, cutoff within a set number of cents, LFO rate within a set percentage); aliasing measured against a floor |
| 2 | Voice modulators 6 → 12; routing "via"; post-filter route and polarity; per-link FM depth; filter output gain and 200% keytrack | Old Surge patches render as before (golden tests); new parameters migrate; modulation and FM measurements within tolerance |
| 3 | "Analog" oscillator type (second saw, wave shaper, sync), saturators, wave folder | Per-block measurements: harmonic spectra per shaper/folder setting, saturator transfer curves, within tolerance |
| 4 | Voice unison 2/4 with detune and spread; OSC reset modes; drift and ageing; key-on delay; portamento time modes; velocity curve | Statistical tests for drift and ageing against measured distributions; unison detune and spread measured; `NOTE_END` tests for unison and delayed notes |
| 5 | Native sound contexts (draft 4), exclusive snapshots | Oscairn's sound-context test cases pass, including TSan concurrency; memory per context measured and documented |
| 6 | Converter coverage | Every AN-X parameter has a destination or a documented reason; the inverse mapping recovers all values for the factory set (run locally) |
| 7 | CPU budget | Voice and unison cost measured on the target board; polyphony set so the worst-case patch stays within the budget agreed with Oscairn |

## 7. Upstreamability

Jacob's decision (2026-10-31): the generic commits go upstream eventually, not yet. Nothing has
been offered upstream.

| Change | Upstream to surge-synthesizer? |
| --- | --- |
| Seedable, platform-independent RNG; removing `rand()` from `DriftLFO` (sst-basic-blocks) | Likely: helps Surge's own golden tests |
| Velocity-0 CLAP note-on handling | Discuss; depends on what Surge wants for its hosts |
| Splitting internal blocks at event times | Possibly, if the cost is small |
| Per-link FM depth, post-filter route, channel polarity, filter output gain, extended keytrack range | Possibly: generic, default off, appended parameters |
| More voice LFOs | Possibly; needs GUI layout work, which upstream would have to want |
| Routing "via" (modulation depth scaled by a second source) | Possibly; it is a general feature |
| Saturators, wave folder stage, voice unison, ageing, key-on delay | Possibly, as generic features with neutral names |
| A multimode filter in sst-filters, new folder shapes in sst-waveshapers | Likely, as library additions |
| "Analog" oscillator type | Possibly |
| Oscairn CLAP target, sound contexts, one-scene build, Lua-free and file-free configuration | No: Oscairn-only |
| Fitted tables, converter | No: the converter lives in its own repository; the fitted tables are in the Oscairn target's tests (`src/oscairn-clap/tests/FittedTables.h`) |

Rule for the fork: keep generic changes in separate commits with neutral names, default off, and
with Surge's tests passing, so they can be offered upstream as small PRs. The Oscairn target
lives in its own directory so merging upstream changes stays easy.

## 8. Licence and naming

### Licences (checked from the LICENSE files in this checkout)

| Component | Licence |
| --- | --- |
| Surge XT (this repo) | GPL-3.0 (`LICENSE`) |
| sst-basic-blocks | GPL-3.0; its README says a few individual headers are also available under MIT |
| sst-filters | GPL-3.0 |
| sst-waveshapers | GPL-3.0 |
| sst-effects | GPL-3.0 |
| sst-plugininfra | MIT, with TinyXML and strnatcmp under zlib |
| clap-juce-extensions | MIT (not used by the Oscairn target) |
| JUCE, LuaJIT, SQLite, MTS-ESP | Not checked here; the Oscairn target is meant to drop all of them |

The engine is a CLAP plugin, a separate work under RFC 0001's plugin boundary, and stays
GPL-3.0. It isn't linked into the Oscairn core. GPL-3.0 code may be combined with AGPL-3.0 code
(GPLv3 §13), so moving parts into the core later is possible.

Oscairn's extension headers (`oscairn/core/clap-ext/include/oscairn-clap/*.h`) are MIT-licensed
since Oscairn commit `10a3c27` ("License the CLAP extension headers under MIT"), so including
them (for sound contexts, milestone 5) adds no licence constraint. Oscairn's tests and tools
(`oscairn-render`, the allocation guard in `core/engine/tests/AllocationGuard.h`) are still
AGPL-3.0; the fork runs the built tools but doesn't copy their code.

Keep Surge's copyright notices, `AUTHORS` and the sst-* licence files, and credit the Surge
Synth Team in Oscairn's `CREDITS.md` when the plugin ships.

### Naming (RFC 0013 IP rule 5)

No Yamaha names in the plugin, its parameters or any UI. "AN-X", "MONTAGE", "MODX", "Motion
Control", "Super Knob" and similar appear only in design documents like this one.

| Item | Proposed name |
| --- | --- |
| Plugin | "Oscairn Analog", with "based on Surge XT" in its description. CLAP ID `org.oscairn.analog`, so it doesn't collide with `org.surge-synth-team.surge-xt` |
| Oscillator type | "Analog" |
| Self Sync Pitch | "Sync" (Surge's existing name) |
| Wave Shaper | "Shape" |
| Out Select Filter/Amp | "Route": Filter 1 / Both / Filter 2 / Post-Filter |
| Out Invert | "Polarity" |
| Wave Folder, Texture, Soft/Hard | "Folder", "Fold Character", "Smooth/Sharp" |
| Saturator Drive | "Drive" |
| Voltage Drift | "Drift" (existing) |
| Ageing | "Component Spread" |
| OSC Reset Off/Phase/Tune/Full | "Retrigger": Off / Phase / Drift / Phase + Drift |
| Key On Delay | "Note Delay" |
| Unison, Detune, Spread | "Voice Unison", "Unison Detune", "Unison Spread" |
| Part LFO | "Scene LFO" (existing) |
| Part | "Sound" (in Oscairn), "Scene" (in Surge) |

Compatibility statements ("imports MODX M Performances") are facts reviewed before release, per
the same rule. Before any commercial release, counsel reviews the features (IP rule 6).

## 9. Risks and open questions

Risks:

- **Size of the change to Surge's scene.** Raising `n_lfos_voice` touches arrays sized by
  `n_lfos` across storage, the GUI, MSEG, formula and step-sequencer storage, and patch
  migration. If it proves too invasive, fall back to option B (section 1).
- **Matching responses.** Surge's envelopes, LFOs and filters have their own curves. Fitted
  tables cover static values, but some responses (envelope segment curvature, resonance
  behaviour, velocity curves) may need new options. Each one is found by measurement and added
  as a generic option.
- **Sound contexts in Surge.** `SurgeStorage` mixes the patch with shared tables and global
  state. Separating per-context state is real work, and memory per context is unknown until
  measured.
- **Event timing.** Splitting internal blocks may cost CPU or interact with the 4-voice quad.
  Keeping the quantum may fail Oscairn's exact-sample tests.
- **CPU on embedded targets.** Unmeasured. Unison 4 on a full AN-X-style voice may limit
  polyphony on a Cortex-A53-class board.
- **Merging upstream.** A long-lived fork drifts. Keeping generic changes small and offered
  upstream limits that.
- **IP hygiene.** This fork is public. Only the Data List, manuals, measurements and Surge
  source may inform the code. No reverse-engineering notes, binaries or Yamaha renders enter it.

Open questions (each to settle from the manual or by measurement):

1. Which oscillator modulates oscillators 1 and 2 for FM, and what Ring Level mixes with what.
2. What the `7p 00` Ring/FM control does relative to the oscillator block's levels.
3. Whether the per-section LFOs are per voice or shared across the Part, and whether "Oscillator
   Pitch LFO Depth" uses the pitch LFO.
4. The filter topology (series assumed), and the folder and saturator positions.
5. What Saw2 and the wave shaper are, and what folder texture does.
6. What Voltage Drift and Ageing model (RFC 0015's open question).
7. Whether AN-X's 11 filter types are the same models as the AWM2/FM-X filters of the same
   names, so the fitted tables can be shared with RFCs 0013 and 0014.
8. AN-X polyphony, and the polyphony Oscairn wants on its device.
9. Whether Oscairn will send polyphonic parameter modulation (Surge supports it; useful for
   controller boxes with per-voice destinations).
10. ~~The licence of Oscairn's extension headers~~ Settled: MIT (section 8).
11. ~~Tuning~~ Settled: Oscairn is the MTS-ESP master and offers `clap.tuning` (RFC 0017);
    the plugin keeps Surge's MTS-ESP client (section 10).
12. ~~Where the converter lives~~ Settled (Jacob, 2026-10-31): in its own private repository,
    https://github.com/oscairn/performance-converter (empty for now). Not built in
    milestone 1 (section 10).

### Not verified in writing this plan

The first draft of this plan was written without building anything. Section 10 records what
milestone 0 has since checked (macOS arm64 builds, tests, block-size independence, the
parameter count). Still not verified: a Linux ARM64 build, and any CPU or memory measurement.

## 10. Implementation log

What has been built on branch `oscairn`, how it was tested, and where it departs from the
sections above. Builds and tests ran on macOS arm64 (Apple clang, CMake 4.4, Ninja) unless a
line says otherwise. Renders and reference files live outside the repository.

### Milestone 0: groundwork

**Seedable RNG** (generic, upstreamable). `SurgeSynthesizer::seedRandom(seed)` reseeds
storage's RNG, the noise RNG, the scene random modulators and the scene LFOs' generators.
Storage's RNG converts `minstd_rand` output itself instead of using std distributions.
`DriftLFO` became a Surge-side copy with its own generator instead of C `rand()`; the Alias
oscillator's 8-bit RNG and MSEG evaluator states are seeded from storage; random modulators no
longer use `std::random_device`. Without `seedRandom`, storage is still clock-seeded, so the
Surge XT plugins behave as before. Remaining sources of nondeterminism, none on the Oscairn
target's voice path:

- `sst::basic_blocks::dsp::RNG` (the noise RNG) and the LFO noise and MSEG generators still use
  std distributions, so seeded output differs between libc++ and libstdc++. Fixing the first
  needs a change in sst-basic-blocks (upstream).
- The Fuzz waveshapers in sst-waveshapers keep a function-static generator shared across
  instances.
- Some effects (FX_Aggero, FX_Space Adventure 1) and Lua formulas keep their own random state.
  The Oscairn target has neither.

**Reference renders of the factory patches** (`[reference]` in the test runner). Every factory
patch, two notes, seeded, compared with renders recorded earlier, at the golden tests' 1e-5
tolerance. Recorded from commit `53886aa` (the RNG change, the first reproducible build); 635
of 641 patches compared, 6 left out as nondeterministic (above): 4 rendered differently in a
later recording run, 2 differently twice within one process. (This line said 633 until
2026-10-31: the 2 were counted twice. The test prints "Compared 635 patches, skipped 6".)
This is the "old patches render as before" check every later milestone runs.

**Oscairn CLAP target** (`src/oscairn-clap/`, `SURGE_BUILD_OSCAIRN=ON`). Written against
CLAP directly over `surge-common`. Decisions taken in the code, replacing section 4's
proposals where they differ:

- *One scene, no effects.* `n_scenes` stays 2 at compile time; the target exposes only scene A
  and three global parameters (volume, polyphony limit, character), and on every state load
  forces single-scene mode, scene A active and every effect slot disabled. Effect memory is
  still allocated. 272 parameters (counted by the test).
- *Parameter values.* Surge's own values and ranges (`val_min`/`val_max`/`val_default`),
  stepped for integer and boolean parameters. Parameters whose range or meaning follows
  another parameter (the 7 controls of each oscillator, filter subtypes, filter 2's cutoff,
  which switches between absolute and offset) are exposed as 0..1 with stable names
  ("Control 1".."Control 7"); their value text comes from the current type. A test checks,
  over all factory patches, that no parameter exposed in Surge's units changes range. IDs are
  FNV-1a hashes of Surge's storage names. Module paths follow Surge's control groups
  (`Oscillator 1`, `Mixer`, `Filter 1`, `Amp Envelope`, `Voice LFO 1`, `Scene LFO 1`, `Scene`,
  `Global`); the finer paths of section 4 wait for the new blocks.
- *Event timing.* Decided: keep Surge's internal block (32 samples) and apply each event at the
  first internal block boundary at or after its time, counted from activation. Events are
  never early, at most 31 samples late (0.65 ms at 48 kHz), and output doesn't depend on the
  host's block size. Splitting internal blocks was not attempted. Oscairn's exact-sample
  release tests need a tolerance of one internal block for this plugin.
- *State loading.* The full context design (prepare off the audio thread, swap a pointer)
  comes with sound contexts (milestone 5). Until then the main thread loads the patch itself:
  it raises a flag, waits until `process()` is not running (at most one block), loads, and
  clears the flag. `process()` never waits; while the flag is up it outputs silence. All
  parsing, allocation and wavetable loading happen on the main thread. Notes ended by a load
  get their `NOTE_END` in the next block. Saving doesn't stop audio: it retries if a parameter
  change happened during the save and only then falls back to the same exclusion.
- *State bytes.* Surge's patch (`sub3` header, XML, wavetables). Never written: the DAW extra
  state. The patch's "tempo on save" is fixed at 120 BPM in this target
  (`SurgeStorage::saveFixedTempo`) so the bytes don't depend on the host's tempo.
  Surge's migrations were not idempotent for 83 older factory patches: the first save after
  loading one differed from the next save. Jacob asked for the cause to be found and fixed
  (2026-10-31). Two causes, both fixed in separate commits with an `[io]` test that every
  factory patch, once loaded and saved, loads and saves back to the same bytes:
  - 81 patches (revision 12 and older) with a Sine, FM2 or FM3 oscillator: the migration
    switched the feedback parameter to the unipolar `ct_osc_feedback` type, which isn't saved,
    so the next load came back bipolar; for the Sine oscillator the feedback mode
    (`deform_type`) also kept whatever the parameter held before the load, because `load_xml`
    decides a missing `deform_type`'s default with the previous patch's control types. Fix:
    keep the current type (same sound for these patches' values, 0 to 1) and set the Sine
    feedback mode to 0. The reference renders are unchanged.
  - 2 patches with a wavetable oscillator but no wavetable data of their own: the save wrote
    the table with an empty name, and the load names unnamed data "(Patch Wavetable)". Fix:
    save the name a load would give. Not fixed: such an oscillator plays whatever table it
    held before the load (order-dependent sound); the plugin's state always carries the table,
    so this only affects loading those old patches.
  The plugin test now requires save == load→save from the first save.

  The wider `load_xml` issue (a missing `deform_type` defaults by the control type in place
  before the load) remains for patches between revision 13 and the revision that added each
  deform. No factory patch shows it in bytes; not investigated further.
- *Notes.* CLAP note-on with velocity 0 stays a note-on. MIDI note-on with velocity 0 is a
  note-off, as MIDI defines. Note port dialects: CLAP (preferred) and MIDI; no MPE.
- *No Lua.* Patches with a Formula modulator in scene A are refused (the previous state is
  kept). Built with `SURGE_SKIP_LUA` and `SURGE_SKIP_JUCE_FOR_RACK`: `surge-common` does build
  without JUCE (verified on macOS arm64).
- *MTS-ESP: kept.* Jacob's decision (2026-10-31, Oscairn RFC 0017 "Host-level tuning"):
  Oscairn is the MTS-ESP master and also offers CLAP's draft `clap.tuning`. Plugins use host
  tuning first, then their own stored tuning, then MTS-ESP, then 12-TET. Surge's MTS-ESP
  client stays in the Oscairn build. `clap.tuning` is the target but no milestone of this plan
  covers it yet; it belongs with sound contexts (milestone 5) or its own step.
  The CI workflow first built with `SURGE_SKIP_ODDSOUND_MTS`; it now builds with the client.
  The local sanitizer runs above used `SURGE_SKIP_ODDSOUND_MTS`; the plain macOS build with the
  client passes the same plugin tests. Not checked: whether the MTS-ESP client does anything
  real-time unsafe while a master is running (the tests run without one).
- *Licences* (Jacob's decision, 2026-10-31): DSP blocks shared between Oscairn and the forks
  (filters, envelopes, LFOs) are GPL-3.0-or-later; test helpers are MIT
  (`src/oscairn-clap/tests/OscairnAnalogTests.cpp`). Measurement analysis against reference
  renders of the instrument being matched (pitch, level, spectrum, envelopes, filter response,
  tolerances) goes in the private, MIT-licensed `oscairn/measurement` repository (`analog/`,
  `common/`), not in this fork. Nothing of that kind is in the fork yet; the `[reference]`
  test compares Surge with itself.
- *Allocation on the audio thread.* Found by the allocation guard and fixed: the voice and
  hold-buffer `std::list`s allocated a node per note (now `Surge::FixedPoolAllocator`, with a
  heap fallback past the pool size); `purgeHoldbuffer` and
  `purgeDuplicateHeldVoicesInPolyMode` built temporary containers; queued wavetable loads ran
  in `process()` (now done right after the state load).
- *Files.* Storage is constructed with patch and wavetable scanning off and without creating
  the user directory (new `SurgeSynthesizer` constructor taking a `SurgeStorageConfig`).
  **Not done:** storage still resolves the user data path and reads `SurgeXTUserDefaults.xml`
  if a Surge XT user folder exists, and constructs (but doesn't open) the patch database, and
  rescans FX and modulator presets. On the device there is no such folder; on a desktop with
  Surge XT installed, user defaults can change behaviour (smoothing modes, for example). TODO
  before relying on desktop renders matching device renders.
- *UBSan* found misaligned 16-bit wavetable copies in patch load and save (the data sits at
  an odd offset after the XML). Now byte copies.

Tests, all run in this session (macOS arm64):

| Gate | How | Result |
| --- | --- | --- |
| Oscairn scanner accepts it | `oscairn-plugin-scan --worker oscairn-analog.clap` (Oscairn `10a3c27` build) | exit 0, descriptor listed |
| Loads in the Oscairn host | `oscairn-render` with Oscairn's step sequencer playing it | loads, renders, peak 0.62 |
| Identical renders, 37/512 frames | `oscairn-render` at 44.1, 48 and 96 kHz, `cmp` of the WAV files; also a repeat at 512 | identical |
| Identical renders, plugin test | `oscairn-analog-tests`: 4 states × 3 rates × blocks 512/37/512/1, notes, choke, expression, MIDI, sustain, parameter changes, an oscillator type change | identical |
| Byte-identical state round trip | `oscairn-analog-tests`: default state and 625 factory patches (16 refused for Formula) | pass (fixed point, see above) |
| No allocation while rendering | `oscairn-analog-tests` with a replaced `operator new` around every `process()` | pass |
| ASan, UBSan | `oscairn-analog-tests` built with `-fsanitize=address,undefined` | no reports |
| TSan | `oscairn-analog-tests` built with `-fsanitize=thread`; state loads and saves while another thread renders notes | no reports |
| Surge test runner | `surge-testrunner` (standard build) | 180 passed, 1 skipped (`[reference]` without its directory) |
| Reference renders | `[reference]` against the `53886aa` recording | 635 patches identical within 1e-5 (633 in the first version of this table; see above) |
| Linux ARM64, CI | `.github/workflows/oscairn.yml` (macOS arm64, Linux ARM64, ASan/UBSan and TSan on Linux ARM64, test runner on macOS) | all 5 jobs pass (run 41180706531, commit `f98a5a9`) |

On GitHub's Linux ARM64 runner the allocation guard found one more allocation that libc++
hides on macOS (an OSC address string built on an oscillator type change, now only built when
someone listens), and the ASan job needs about 27 minutes. The CI build includes Surge's
MTS-ESP client (open question 11); the macOS sanitizer runs above were built without it.

Not covered by the TSan test: a parameter change concurrent with a state save. The save's
retry makes the result correct, but the read it discards is still a data race in the C++
sense; TSan would report it. Leak checking was off (LeakSanitizer isn't supported on macOS
arm64). Value text for oscillator controls reads the parameter's current
type, which the audio thread changes when the oscillator type changes (marked `shortcut:` in
the code).

### Milestone 1: measurement and fitted tables (in progress, gate partly met)

Status on 2026-10-31 (second session). Analysis code is in the private `oscairn/measurement`
repository (`analog/features.py`, `fit.py`, `export.py`, `tolerances.json`). Probe
performances, reference renders, engine renders and fit outputs stay in
`forks/measurements/analog/` on Jacob's machine. `oscairn-analog-render` renders the engine;
its render code is now `src/oscairn-clap/tools/RenderNote.cpp`, shared with the tests.

**Method.** A table maps the reference's parameter index to a Surge value. The reference is
swept densely (steps of 4 or 8); the table is fitted from part of the sweep and checked at
indices it was not fitted from, by rendering the engine at the table's value and measuring
the same feature on both (`fit.py fit`/`check`). Passed tables are exported to
`src/oscairn-clap/tests/FittedTables.h` (`export.py`) with the reference's measured value at
every checked index, and `oscairn-analog-tests` ("fitted tables") renders those indices
through the plugin and measures them again (`tests/Features.h`, the same definitions in
C++). The test was seen to fail when a reference value is moved by 1 dB.

Measurement changes from the first session:

- Windows count from each render's own onset (the recorder's start varies by about 70 ms).
- LFO rate: envelope spectrum refined by a Fourier-series fit (0.2 % on engine renders with
  three periods); rates above about 45 Hz from the sidebands of a sine carrier at note 84.
  Both agree where they overlap (indices 256-292).
- Filters: the response is the ratio of a sawtooth's harmonics (note 36, or 24 for low
  cutoffs) through the filter and with it off. Cutoff is matched by response shape (the
  engine cutoff whose response is closest after removing a gain offset), because a -3 dB
  corner read between harmonics wobbles by about 30 cents below 400 Hz. The noise probe is
  gone.
- Envelopes: release from the 1 dB knee to 40 dB down (independent of exactly when note-off
  lands); analysis windows of whole oscillator periods (one at note 60, two at note 96).
- The recorder occasionally writes a run of exact zeros mid-render (6 renders so far);
  such renders are detected, not measured, and re-recorded.

**Findings about the reference.**

- LFO speed is piecewise linear in Hz between breakpoints (within a segment the step in
  semitones shrinks as 1/rate; above index 290 the segments are 24 indices long and the slope
  doubles from one to the next). It is interpolated in Hz. It reaches 1357 Hz at index 415.
- Envelope times double every 16 indices in the long range and lie between log and linear
  interpolation within those 16; log is closer. Times under about 30 ms vary by 1.5-3 ms
  (standard deviation) between repeated renders of the same probe: the reference's own
  timing resolution.
- Cutoff has kinks every 64-80 indices; between them it is smooth.
- Amp level is smooth; index 56 is -63 dB, 511 is -25 dB (relative to the probe's
  full-scale saw).

**Tolerances.** The first session's 10 % / 2 % / 0.5 dB / 50 cents were never approved. Now,
for Jacob to confirm (`tolerances.json`):

| Feature | Tolerance | Why |
| --- | --- | --- |
| Amp level | 0.1 dB | All checks within 0.04 dB |
| LFO speed | 2 % | Measurement resolves 0.2 %; checks are within 0.7 % below index 290, but up to 1.95 % next to the breakpoints above it (8-index knots straddle them). Identifying the breakpoints would allow 1 % (the FM engine's gate) |
| Envelope times | 2.5 % or 5 ms | Release and decay within 1.8 %, attack within 2.4 %, from 8-index knots; 5 ms is about twice the reference's render-to-render spread at short times. The FM gate (2 %) would need step-4 knots |
| Cutoff | 50 cents | Unchanged |
| Aliasing | floor -70 dB (power off the harmonics relative to on them) | Proposed; see below |

**Results** (Surge's analog envelope mode, drift off; "checks" are indices the table was not
fitted from):

| Table | Surge parameter | Valid indices | Checks | Committed |
| --- | --- | --- | --- | --- |
| Amp level | `a_vca_level` | 56-511 (below: under -63 dB, clamped) | 57 of 57 within 0.1 dB | yes |
| LFO speed | `a_lfo0_rate` | 18-380 | 46 of 46 within 2 % (242 re-recorded: 0.47 %) | yes |
| Amp EG attack | `a_env1_attack` | 100-255 | 17 of 17 (third session, with the longer range) | yes |
| Amp EG decay | `a_env1_decay` | 100-255 | 17 of 17 | yes |
| Amp EG release | `a_env1_release` | 112-255 | 16 of 16 (108: 8.4 % off, outside) | yes |
| Cutoff, LPF24 | `a_filter1_cutoff` (LP 24 dB, resonance 0.06) | 320-1004 (note 24 below 384, note 36 from there) | Third session: fitted from every 8th index plus the first index after each kink, pieces between kinks interpolated separately: 154 of 154 within 50 cents, 51 of them next to kinks (median 3 cents at note 36, 7 at note 24; largest 20 and 35) | yes |
| Cutoff, LPF18 / LPF12 / LPF6 | Legacy Ladder 18 / 12 / 6 dB | 384-992 / 384-992 / 384-944 | LPF24 shape plus a smooth difference from a step-32 sweep; 19 of 19 / 24 of 24 (fourth session, with probes at 936-960; before: 18 of 19, 944 143 cents off) / 18 of 19 (976: 50.4 cents) | yes, the valid spans |
| Cutoff, HPF18 / HPF12 / HPF6 | Xpander HP3 / OB-Xd 12 dB HP / Xpander HP1 | 384-992 | 19 of 19 each (largest 34, 25, 25 cents) | yes |
| Cutoff, BPF12 / BPF6 | BP 24 dB / BP 12 dB | 416-992 / 384-992 | 18 of 18 / 19 of 19 | yes |
| Cutoff, HPF24 | HP 24 dB, resonance 0.05 | 384-992 | 24 of 24 (fourth session, with probes at 936-960; before: 16 of 16 up to 912, 944 129 cents off). At resonance 0 (the first choice): 14 of 17 | yes |
| Oscillator 1 level | `a_level_o1` at `a_vca_level` = amp level table [431] | 80-511 | 14 of 14 within 0.1 dB (largest 0.022) | yes (fourth session) |

Outside the valid spans, and why:

- Envelope times below index 100 (under about 60 ms): the reference's spread (above) makes
  checks there meaningless at 2.5 %; averaging repeated renders would be needed. In
  addition Surge's analog-mode decay cannot get shorter than about 28 ms to -40 dB (a slow
  tail after a fast drop), while the reference decays in 6-26 ms at indices 40-76; release
  bottoms out at about 7 ms.
- Long times (done in the third session): the reference's attack reaches 44 s (10-90 %) and
  its release and decay 45-51 s (to -40 dB) at index 252-255; Surge's analog-mode attack covers
  10-90 % in about 0.21 of the set time, so it needs 2^7.7 s. The plugin's envelope times now
  reach 2^8 s (decision below).
- LFO above index 380: past Surge's 512 Hz LFO maximum. Below 18 (under 0.05 Hz): renders
  would need over a minute.
- Cutoff below index 320 (about 30 Hz): under the probe's lowest harmonic. LPF24 above 1004:
  the reference flattens at 65.9 engine semitones from 1008; the shape match there sits at
  the edge of the engine sweep. The other types below 384 (416 for BPF12): not fitted.
- Cutoff kinks: the reference's cutoff curve has kinks at multiples of 1024/12 indices
  (85.33); right after each the curve rises steeply (up to 0.6 semitones per index, against
  about 0.13 just before), then flattens until the next. Measured at every index within 3 of
  each kink from 341 to 939.

**Filter family.** Each of the 10 filter types (Thru aside) was rendered at cutoffs 256, 512,
640, 768 and 896 with resonance 0 and compared in shape (RMS dB difference over the part of
the response within 40 dB of its peak, below 16 kHz, gain offset removed) with 38 Surge
filter types and subtypes, each swept over cutoff. Best matches (RMS dB at 512 / 640 / 768):

| Type | Surge filter | Shape error | Gain offset |
| --- | --- | --- | --- |
| LPF24 | LP 24 dB (Standard), resonance 0.06 | 0.18-0.56 dB at resonance 0.05 and 0.075 (0.06, used, not measured itself) | about -3 dB (at resonance 0) |
| LPF18 | Legacy Ladder, 18 dB | 0.05 / 0.25 / 0.58 | +0.4 dB |
| LPF12 | Legacy Ladder, 12 dB | 0.07 / 0.24 / 0.91 | +0.9 dB |
| LPF6 | Legacy Ladder, 6 dB | 0.43 / 0.40 / 0.35 | +0.4 dB |
| HPF24 | HP 24 dB (Standard), resonance 0.05 (third session) | 0.30 / 0.60 / 0.59 at resonance 0; 0.09 / 0.14 / 0.29 at 0.05 | +6.3 dB (at resonance 0) |
| HPF18 | OB-Xd Xpander, HP3 | 0.02 / 0.11 / 0.26 | +0.6 dB |
| HPF12 | OB-Xd 12 dB HP | 0.02 / 0.07 / 0.30 | +0.6 dB |
| HPF6 | OB-Xd Xpander, HP1 | 0.00 / 0.00 / 0.00 | -2.5 dB |
| BPF12 | BP 24 dB (Standard) | 0.99 / 0.86 / 0.85 | +1.8 dB |
| BPF6 | BP 12 dB (Standard) | 0.43 / 0.45 / 0.63 | +2.2 dB |

No single Surge family is best for all types; the choice above is per type (Legacy Ladder
for the low-passes but LPF24, the Xpander or OB-Xd for the high-passes, the standard
band-passes). At resonance 0 the reference's LPF24 has a sharper knee than any Surge
4-pole at resonance 0 (1.4 dB off); a little resonance (0.05-0.075) matches it. At cutoff
896 (3-11 kHz corners) most choices are 0.7-2 dB off. The engine's cutoff
for a given index differs between types by up to about 11 semitones (high-passes most), so each
type needs its own cutoff table; a sweep of all types at steps of 32 is recorded but not yet
fitted. Gain offsets go to the filter output gain of milestone 2 (measured after the third session at
the chosen filters, engine minus reference, median over the cutoff checks: LPF24 -1.4 dB, LPF18
+0.4, LPF12 +0.7, LPF6 +0.5, HPF24 +4.6 (varies with cutoff), HPF18 0.0, HPF12 +0.1, HPF6
-2.65 (varies), BPF12 +1.7, BPF6 +2.0). Resonance was recorded
(4 values per type at cutoff 512) but not fitted.

**Aliasing.** Sawtooth and square at notes 72-108, power off the harmonics relative to on
them (20 Hz-20 kHz): the engine (Surge's Classic oscillator) is between -88.5 and -74.5 dB,
the reference between -100 and -87 dB, so the engine is 2-17 dB worse but under a -70 dB
floor everywhere. The "Analog" oscillator of milestone 3 should be measured the same way.

**Not done in milestone 1:** cutoff at index 944 for LPF12 and HPF24 (over 125 cents off,
just after the kink at 938.7; the tables stop at 912); envelopes under index 100; the other envelopes and LFOs (pitch and filter LFO probes are
recorded, not analysed; the other envelopes not probed); oscillator and mixer levels;
resonance; the converter (open question 12: it goes in its own repository, not built here).

**Tests run for these commits** (macOS arm64): `oscairn-analog-tests` all passed (standard,
ASan/UBSan and TSan builds of commit `848e2c7`, no sanitizer reports), `surge-testrunner` 181 passed and 1 skipped, `[reference]` 635
patches compared within 1e-5 (6 skipped as nondeterministic). CI: see the report for this
session.

**Third session (2026-10-31).**

- *Envelope range* (decision for Jacob). The plugin exposes scene A's attack, decay and
  release (amp and filter envelopes) up to 2^8 s instead of Surge's 2^5 s
  (`longestEnvelopeTime` in `OscairnAnalog.cpp`). No generic engine change was needed: the analog
  mode computes its rates from the value without an upper limit, the digital mode's rate
  table reaches 2^15 s, and modulation
  depths are stored in the same units, so only the exposed range grows. It is the plugin's
  behaviour, not an option. Surge XT is unchanged, `[reference]` is unchanged (635 patches),
  and a Surge patch loaded into the plugin sounds as before. A patch saved by the plugin with
  a time above 5 should play that time in Surge XT too (its patch loader doesn't clamp
  values, from reading the code; not tried), but Surge XT's slider can't show it. Not done: the scene LFOs' envelope stages keep 2^5 s.
- *Cutoff tables.* `fit.py` gained `breaks` (interpolate each piece between kinks from its
  own points) and `base` (another fit's dense curve plus a smooth difference, used for the
  types swept at step 32). The plugin test checks each table by response shape: the header
  holds the reference's response at every harmonic the match uses (tenths of a dB; the
  header is now 378 KB), and the test renders the table's value and half a semitone either
  side and fits a parabola to the three shape errors. Subsets of 40 harmonics disagreed with
  the full match by up to 90 cents for LPF6 and BPF6. A table value moved by 0.6 semitone
  fails the test (seen).
- *Resonance, first look* (not fitted). LPF24 at cutoffs 512 and 768, resonance 0-255 in
  steps of 16, against Surge's LP 24 dB at the table's cutoff with resonance 0-0.99: the best
  resonance rises from 0.05-0.08 (index 0) to 0.37 (index 64) with shape errors of 0.1-0.85
  dB, but above index 96 no resonance matches (1.1-1.3 dB RMS at cutoff 512, where the best
  value stays at 0.52; 1.2-4.1 dB at 768). The best match's gain over the reference grows by 9-12 dB
  from index 0 to 255. Next: search cutoff and resonance together, and try the other
  4-pole filters.
- *Pitch and filter LFO speeds* follow the amplitude LFO's curve: at indices 112, 176 and
  240 the pitch LFO (vibrato rate) is 0.621, 2.409 and 11.948 Hz and the filter LFO 0.620,
  2.397 and 11.945 Hz, against 0.620, 2.398 and 12.07 Hz interpolated from the amplitude
  LFO's measurements (within 1.0 %). The converter can use the one LFO speed table for all
  three; three points, so not a full check.
- *Envelope times under index 100, averaged* (not committed). Five more repeats at note 96
  of indices 40-96 (step 8), with the earlier repeats of 56-76: standard errors of the means
  0.1-1.8 ms. Fitted from the means at 40, 56, 72, 88 and 96 and checked at the others
  (48, 60, 64, 68, 76, 80): attack and release pass, but only through the 5 ms floor (attack
  48: 7.7 ms reference, 9.1 engine; release 68: 17.2 and 20.0); decay fails at 48-68 (9.4-21.2
  ms against the engine's 28.3 ms floor). With averaging, the 5 ms floor is larger than the
  reference's noise, so these aren't committed as a pass; tightening the floor for averaged
  checks is for Jacob to decide with the tolerances.
- *`[reference]` count.* The plan said 633 patches compared; the test compares 635 (the 2
  within-process cases had been counted twice). Corrected above.
- *CI runner* (decision for Jacob). The macOS jobs moved from `macos-14` to `macos-15`:
  GitHub retires `macos-14` on 2026-11-02 and is already browning it out.

**Tests run in the third session** (macOS arm64, commit `4eafdef`, the last code change):
`oscairn-analog-tests` all passed in the standard, ASan/UBSan (81 s) and TSan (8 min) builds,
no sanitizer reports; sanitizer builds check every 8th cutoff index (the full set took
15 minutes under TSan; that run, of commit `1c03103`, also passed with no reports).
`surge-testrunner` 181 passed, 1 skipped, and `[reference]` "Compared 635 patches, skipped
6", both on the tree of `c65dfaf` (later commits touch only the plugin's tests). CI for the
second session's last commits `d6bdbe1` and `93620f8`: all 5 jobs passed. CI for this
session's push: see the report.

**Fourth session (2026-10-31).**

- *CI headroom.* The ASan job's test step took 38 of its 60 minutes (envelope checks render up
  to 340 s of audio each). Sanitizer builds now check only envelope times whose render is under
  30 s (as for cutoff, where they check every 8th index); standard builds check everything.
  On CI (run 41221092760) the test step went from 37.9 to 30.8 minutes (fitted tables 17.3 to
  10.3; the state round trips, 12.5 minutes, and the parameter test, 6, are unchanged). Locally
  ASan went from 81 to 64 s and TSan from 8 to 4.5 minutes.
- *Short analog decay: not an envelope limit.* The 28.3 ms decay floor came from Surge's scene
  highpass (`a_lowcut`, on by default at 6.9 Hz, Q 0.4, after the voices): a burst a few blocks
  long has a DC component, and the highpass's step response leaves a tail about 20 dB under the
  peak at note 96 (27 dB at note 60) that takes about 25 ms more to reach -40 dB. With the
  highpass off the engine decays to -40 dB in 2.5 ms at the shortest setting, and the averaged
  short-envelope fit (indices 40-96, fitted from 40, 56, 72, 88, 96, checked at 48-80) passes
  for decay too: 6 of 6 for attack, decay and release each, but only through the 5 ms floor
  (largest differences 1.8 ms attack, 2.6 ms decay, 2.6 ms release). As before these aren't
  committed (decision on the floor for averaged checks still open). Above index 100 the
  highpass changes engine decay times by at most 0.6 % (1.2 % at 30 ms) and release not at
  all, so the committed tables stand. Decision (for Jacob): the converter switches the scene
  highpass off; the reference shows no such tail. The render tool takes `a_lowcut.off=1`.
- *Envelope overshoot* (looked at, nothing changed). In isolation Surge's analog-mode ADSR
  overshoots full level by about 40 % with the shortest attack (it charges toward 1.5 and
  starts the decay two blocks after passing 1). Plugin renders show no such overshoot: with
  attack -8, decay 2 and sustain 100 % the level is within 0.2 dB of the sustain level from the
  third oscillator period on, the same as with attack -6, and the reference shows none either.
  A storage option clamping the attack at 1 was written and dropped: it showed no level
  difference in the plugin but moved the pitch-EG decay times below further from the
  reference. Not understood why the voice's level differs from the bare envelope's.
- *Pitch EG against the amp EG tables* (not fitted). Pitch EG probes at indices 128, 160 and
  192 on a sine (depth raw 309, which gives 185 cents, not the 1200 the Data List's step
  suggests), against Surge's filter EG in analog mode at the amp tables' values routed to
  pitch (Sine oscillator, scene highpass off), 10-90 % times: attack -3.6 / +0.4 / +0.2 %, decay -1.9 / +12.8 / +7.1 %, release
  -6.8 / -6.0 / -5.6 % (with the dropped attack clamp above, decay was +6.9 / +17.6 / +12.0 %). The attack can use the amp attack table; decay and release need their
  own tables (dense probes next). The pitch EG decays exponentially in cents.
- *Filter EG* (LPF24 at cutoff 512, depth raw 352 = +4800 cents; the cutoff over time from the
  LP 24 dB shape match in 60 ms windows). Held at full level the EG moves the cutoff by +51.2
  engine semitones, not 48. Its decay is exponential with time constants 0.193 s (index 160)
  and 0.78 s (192), against the pitch EG's 0.196 and 0.82 s; its attack at 192 follows the pitch
  EG's within 0.03 of full travel at 0.1-4 s. So the filter EG, pitch EG and (for attack) amp
  EG share their time curves to within a few percent; the release probe was cut short by the
  amp release. Not probed: oscillator EGs.
- *Levels.* Oscillator 1's out level follows the amp level curve within 0.02 dB (80-511);
  committed as a table with 14 checks. Oscillators 2 and 3 measure the same as oscillator 1
  within 0.01 dB at 128, 256, 384 and 511, so the converter can use the one table. Noise out
  level is the same curve plus 0.45 dB; a table fitted from 128 and 511 passes at 256 and 384
  (0.093 and 0.067 dB, against 0.1), not committed: two checks of a random signal, with the
  noise tone not matched.
- *Resonance* (LPF24, cutoffs 512 and 768, indices 0-255 step 16, cutoff searched ±6
  semitones in 0.5 together with resonance 0-0.99 in 0.03). LP 24 dB matches to 0.35 dB RMS up
  to index 48 and 0.9 dB up to 96, with resonance 0.09/0.15/0.24/0.30/0.36/0.45/0.48 (cutoff
  512) and 0.06/0.12/0.21/0.30/0.36/0.42/0.48 (768) at indices 0-96, the cutoff within 0.5
  semitone of the table. Above 96 nothing in Surge fits both cutoffs: LP 24 dB 0.9-1.3 dB (512)
  and 1.0-3.5 dB (768); Legacy Ladder 24 dB, Vintage Ladder and OB-Xd 24 dB 2.1-2.7 dB at 512
  and 0.9-4.1 dB at 768 (often at their resonance limit); Diode Ladder 4-10 dB. The reference behaves like a ladder: the pass band
  drops as resonance rises (-14 dB at 255, a feedback gain of about 4) while the peak gets
  narrow (+28 dB over the pass band at 768). Surge's LP 24 dB keeps its pass band, and its
  ladders don't have the reference's knee at low resonance. A filter with both would be new
  work (a ladder with LP 24 dB's knee); stopped here.

**Milestone 2 measurements** (fourth session, raw probes at note 36 or 48; not yet in the
engine):

- *Filter out level* is exactly 0.375 dB per step: -12.04, -6.01, +0.07, +6.02, +12.10 dB at
  raw 32, 48, 64, 80, 96. It also applies with the filter at Thru (+12.09 at 96). As long as
  nothing after the filters is non-linear, the converter can fold it, and the gain offsets
  above, into the VCA level; with saturators it needs the filter unit's own output gain.
- *Out select "Amp"* bypasses the filters: oscillator 1 through a closed LPF24 measures
  -42.8 dB routed to the filter and -27.3 dB routed to the amp, the same as with the filter at
  Thru (-27.4). Same for noise (-56.2 and -26.8 dB).
- *Out invert* is an exact polarity flip: two phase-reset saws at equal level, one inverted,
  cancel to digital silence (peak 0).
- *Saturators* (filter and amp, drive raw 0-80 = 0-60 dB, on a sine). Both are asymmetric
  (second harmonic -55 dB at raw 8, -26 dB at raw 48; odd harmonics grow later) and nearly
  level-compensated (fundamental +1 to +3.5 dB over the clean sine). The spectrum depends only
  on drive plus input level: drive raw 40 at oscillator level 128 (24 dB lower) gives the
  spectrum of drive raw 8 at level 511 (32 steps of 0.75 dB). The amp saturator has slightly
  less second harmonic than the filter's at the same drive. The filter saturator is bypassed
  when the filter is Thru. Position (before or after the filter, before or after the VCA) not
  measured yet.
- *Wave folder* (sine at note 48): odd harmonics only for Soft at every amount; Hard adds even
  harmonics at high amounts (-48 dB at 255). Texture changes the fold pattern at amount 128
  (third harmonic 0 to -36 dB over texture 0-255; 128 is the default).
- *Wave shaper* on sine, saw and triangle at 0-255: on a sine mostly odd harmonics (third -19
  dB at 64); on a saw the second harmonic overtakes the fundamental at 192 and above; on a
  triangle 128 and above turn it into a different wave. Transfer curves not fitted.

Engine work these imply (started in the fifth session, below): a post-filter route and a polarity switch per mixer
channel (the voice mixes into the two filter inputs, and the quad filter chain runs filter,
shaper and VCA per voice lane, so the post-filter route needs a third input to the chain
before the VCA), saturators, folder and shaper as new blocks (section 3). Each changes the
patch format (new parameters, a revision and migration) and the plugin's parameter list.

**Tests run in the fourth session** (macOS arm64). Each code commit's tree (`01be3b3`,
`f967ac1`, `c7aa171`, `e3698df`): `oscairn-analog-tests` all passed in the standard,
ASan/UBSan and TSan builds with no sanitizer reports. `surge-testrunner` (181 passed, 1
skipped) and `[reference]` ("Compared 635 patches, skipped 6") on the trees of `01be3b3` and
`e3698df` (the commits between touch only the plugin's tests and render tool). A shifted
reference value (0.2 dB) makes the new oscillator level check fail (seen). CI for `01be3b3`:
all 5 jobs passed (run 41221092760); for the push ending at `699b968` (which added `f967ac1`,
`c7aa171`, `e3698df` and this record): all 5 passed (run 41225707828, which ran on the head only).

**Fifth session (2026-10-31): milestone 2 engine work.**

*Block positions, measured first* (batch `m2b`, sine at note 48 unless noted; harmonic levels
relative to the fundamental):

- Filter saturator: **after** its filter. Through LPF24 at cutoff 480 (about 184 Hz) drive raw
  48 leaves the second harmonic at -28.4 dB (open filter: -26.4); a saturator before the filter
  would have it filtered away. Through HPF24 at 576 the attenuated fundamental drives it less
  (-71 dB). Filter 2's saturator measures the same as filter 1's in the same place.
- Filter out level: **after** the filter saturator (the filter saturator's spectrum is the same
  at out level 32, 64 and 96) and **before** the amp saturator (the amp saturator's spectrum
  changes with it).
- Amp saturator: **before** the amp level (identical spectra at amp level 128, 256, 431 and
  511), and **after** the out-select join: an oscillator routed to the amp is saturated by it,
  not by the filter saturator.
- The two filters are in series (LPF24 640 into HPF24 640 gives the product, not the sum).
- Wave folder: **after** the mixer (two sines 4 Hz apart give intermodulation products),
  **before** the filters (LPF24 at 480 removes what it adds), on the filter path only (an
  oscillator routed to the amp is not folded), before the amp level.

So the reference's voice is: mixer (filter path) → folder → filter 1 → saturator 1 → out level
1 → filter 2 → saturator 2 → out level 2 → + channels routed to the amp → amp saturator → amp
level and EG.

*Engine changes* (generic Surge commits, each off by default; each new parameter is appended to
the scene, has no control in Surge XT's skin yet, and is reset to its default by `load_xml`
before a patch is read):

| Commit | What |
| --- | --- |
| `9a5a4a5` | Mixer: Invert and Post-Filter switches for each of the six channels. Post-filter channels go to a new chain input added just before the amp gain |
| `96ef6c0` | Filter units: Drive (off by default), Drive Asymmetry, Output Gain; scene: Amp Drive, Amp Drive Asymmetry. Saturator = (tanh(g x) + a tanh²(g x)) / g, then a DC blocker at about 5 Hz. Filter saturator only while its filter runs; output gain also with the filter off |
| `a1830c0` | Pre-Filter Shaper Type (any sst waveshaper, off by default) and Drive on the filter 1 and 2 inputs: the folder's place |

Decisions taken (for Jacob to confirm):

- No patch revision change: resetting the new parameters before every load makes old patches
  load as before, and a revision number of our own would collide with upstream's next one.
- Post-Filter is a separate switch per channel, not a fourth value of Route: Surge XT's route
  control is a three-position switch.
- Feedback configurations (Serial 2/3, Dual, Ring, Stereo, Wide) keep tapping the filtered signal
  without the post-filter input and the amp saturator. In Stereo the post-filter signal goes
  half to each filter's pan position.
- The saturators run at Surge's 2x oversampled rate with no further anti-aliasing; tanh is
  sst's clamped Padé approximation.
- The folder is a generic stage running sst's existing waveshapers, not a new folder curve (see
  the fit below).
- Off switches (`52280db`, Oscairn-specific): every Surge parameter that can be switched off
  (filter types, scene highpass, LFO rates, the new drives and shaper) is now also a stepped
  plugin parameter "<name> Off" (ID: FNV-1a of "<storage name>.off", 1 = off, default from the
  init patch). The plugin has 327 parameters (272 at milestone 0; 284, 292, 294 after the three
  engine commits).
- Added in the sixth session (details there): voice unison allows 1-16 voices, and a mono-mode
  group keeps its size if the count changes mid-note; segment drift keeps no memory between
  notes, has normal nodes, and leaves Surge's own drift in place (the converter sets it to 0);
  the saturators divide by tanh(drive) instead of the drive; Saw2 maps to Modern (saw plus sine)
  rather than a new oscillator; OSC Reset Phase and Full map to retrigger and Tune to nothing;
  ageing is not modelled; the "Analog" oscillator waits for a shaper and self-sync model.
- Added in the seventh session (details there): twelve voice LFOs stored in one block with
  the scene LFOs, but streamed in the six-LFO numbering (no revision change, no conversion of
  old patches); voice LFOs 7-12 only run in voices that start with a routing from them; the
  LFO envelope gets an analog mode (Surge's analog ADSR on the LFO's A/D/S/R, hold unused)
  for the pitch, oscillator and modifier EGs and the LFO fade-ins; the plugin lets voice LFO
  attack and decay reach 2^8 s, but not release (its maximum means "never releases").

*Saturator fit* (engine renders through the plugin at osc level table[511], amp level
table[431], against the `m2` probes; drive in the engine = offset + 0.75 dB × raw):

| Saturator | Offset | Asymmetry | Harmonics 2-6 above -50 dB, worst difference |
| --- | --- | --- | --- |
| Amp | -40.0 dB | 0.10 | raw 48: 1.6 dB; 64: 1.0 dB; 80: 3.5 dB (second harmonic) |
| Filter (LPF24 at 1004, resonance 0.06) | -41.4 dB | 0.226 | raw 48: 1.8 dB; 64: 1.3 dB; 80: 3.1 dB (second harmonic) |

Below raw 48 the second harmonic is within 0.7 dB (amp) and 0.3 dB (filter), but the reference
has odd harmonics at -55 to -80 dB that grow as the level falls (third harmonic -56 dB at
osc level 64), which no static curve gives; the engine has none. Its odd harmonics also grow
faster in the middle of the range (fifth harmonic at raw 48: -53.9 reference, -59.1 engine): a
sharper knee than tanh. The reference's small-signal gain varies irregularly with drive (+0.6 to
+3.6 dB, not monotonic); the engine's is 1, so the converter folds that into the out level and
amp level that follow. The filter offset includes the LPF24 pass-band offset (engine about
1.4 dB lower) and needs the other types' offsets. Proposed tolerance (for Jacob): harmonics
above -50 dB within 3 dB; **not met** at raw 80 (3.5 and 3.1 dB). No plugin check committed.

*Folder fit* (engine pre-filter shaper, types Sine, Single/Dual/West Fold and Soft Fold, drive
-24 to +36 dB in 1 dB steps, best RMS dB over harmonics 2-12 floored at -60): Soft at amount 32,
64, 96 matches Soft Fold at -9, -1, +6 dB (1.3, 2.1, 3.0 dB RMS); above that and for Hard the
best match jumps between types and drives (2.6-10 dB RMS). A folder curve fitted to the
reference (and texture) remains to be done; the transfer curves (sine probes reconstructed with
150 harmonics, input phase from symmetry) show soft compression at 64, one fold by 96 and
several by 255; texture 255 at amount 128 is a gentle compression, texture 0 folds more.

*Wave shaper* (not implemented): on a saw (ramp) it folds like a·sin(g x) up to amount 96
(2 % of peak RMS error), not beyond; on a sine, odd harmonics. Belongs to the "Analog"
oscillator (milestone 3).

*Pitch EG* (batches `m1c`, `m1d`; sine at note 60):

- Depth mapping (raw → cents, attack 0, sustain full; batch `m1e`, every 8th raw value and 27
  between): piecewise linear in cents, symmetric about raw 247 (0 cents): 50 cents per step
  from 0 to 72 (-4800 to -1200) and from 420 to 494 (+1180 to +4800), 10 per step from 72 to
  160 and 332 to 420, 5 per step from 160 to 208 and 288 to 332, and a smooth curve in between
  (-69.7 at 208, -15.2 at 232, +1.2 at 248, +17.0 at 264, +46.3 at 280, +80.3 at 288; raw 309,
  used for the probes, is +185.2). Linear interpolation of every 8th value plus the breakpoints
  332 and 420 is within 0.4 cents at the values in between, except 268 (1.2) and 284 (3.5).
  The engine side is linear: a filter EG routed to oscillator 1's pitch gives 1400.18 cents per
  unit of depth, and depths above 1 work (3.43 gave 4803.6 cents). This is a converter lookup;
  no engine table was committed.
- Decay and release, depth raw 309, against Surge's filter EG (analog mode) routed to pitch,
  fitted from every 8th index (96-208) and checked at the indices 4 between (100-204). With the
  time measured from 90 % to 10 % by threshold crossings, checks scatter by up to 11 % (decay)
  and 8 % (release). Measured instead from a straight-line fit of log(cents) against time over
  that range (`pitch_fall_time` in the measurement repository, kind `pitchfall`, tolerance as
  for envelope times: 2.5 % or 5 ms, for Jacob to confirm): **committed** (`e1489f5`) as
  `pitchEgRelease` (valid 128-208, 10 of 10 checks within 1.2 %) and `pitchEgDecay` (valid
  144-208, 8 of 8 within 1.9 %), with the same check in `oscairn-analog-tests` (C++
  `pitchFallTime`; a reference value moved by 5 % fails it, seen). Below those spans: release
  at 124 is 6.4 % off (the reference's times step unevenly there), decay is 2.6-14 % off below
  148 (short decays scatter; repeated probes needed). The reference's times step by about 22,
  20, 15 and 18 % per 4 indices, repeating every 16. The filter EG shares these curves (fourth
  session), so the converter can use the tables for it too (not checked separately).

**Tests run in the fifth session** (macOS arm64). On the tree of each code commit (`9a5a4a5`,
`96ef6c0`, `a1830c0`): `oscairn-analog-tests` passed in the standard, ASan/UBSan and TSan
builds with no sanitizer reports (the plugin exposes 284, 292 and 294 parameters); `surge-testrunner`
passed with `[reference]` included: 185, 188 and 189 test cases, "Compared 635 patches, skipped
6" each time. New `[signalpath]` tests: invert cancels exactly, post-filter skips a closed filter
and changes nothing with the filter off, the amp saturator's harmonics match the formula within
0.02 dB and don't change with the amp level, the filter saturator runs only with its filter,
output gain applies with the filter off, the shaper sits before the filters and post-filter
channels skip it, and every new parameter resets on loading an old patch and round-trips.
For `52280db`: `oscairn-analog-tests` passed in the standard, ASan/UBSan and TSan builds (TSan:
see the report), with a new check that the amp drive's off switch is a parameter, follows its
event and is carried by the state. For `e1489f5` (plugin tests only): `oscairn-analog-tests` passed in the standard, ASan/UBSan and
TSan builds with no sanitizer reports; `surge-testrunner` was not rerun (no engine change). CI:
see the report for this session.

**Sixth session (2026-10-31): milestone 3, voice level.** Batches `m3a`-`m3d` (sine at note 60
unless noted, drift 0 unless it is the variable; the recorder plays one note per load, so
statistics come from repeated loads).

*Unison* (sine at note 84, detune and spread swept). Two voices sit at +-d cents, four at
+-2d and +-0.66d (span 4d), where d is the detune index times 0.9975 cents (index 8: +-7.98
cents; four voices +-15.96 and +-5.27). Each voice plays at 1/sqrt(n) (0.707 and 0.500 of a
single voice). Spread pans only by attenuating the far side, never boosting the near one; the
far gains of two voices are whole multiples of 0.375 dB (1, 1, 2, 4, 5, 7, 9, 12, 15, 18, 23,
27, 33, 39 steps for spread 2-15; spread 1 does nothing). With far = cos^2(pi x / 2), x for two
voices is 0, 0.132, 0.132, 0.186, 0.261, 0.291, 0.342, 0.385, 0.439, 0.486, 0.527, 0.584,
0.624, 0.675, 0.717 (spread 1-15, about 0.048 per step); four voices sit at 2x and 2x/3,
which predicts the four-voice gains within 0.007. Unison voices drift independently (two voices
at drift 127: -14.6 and +21.8 cents).

*Engine: voice unison* (`bf59112`, generic). Scene parameters Voice Unison (voices per note),
Voice Unison Detune (cents per unit offset, Surge's `ct_oscspread`) and Voice Unison Spread
(0-100 %). Voice u of n has offset k from -n/2 to n/2, evenly spaced; pitch moves by k times
the detune, pan position is k times the spread (clamped), the far side is scaled by cos^2 and
each voice by 1/sqrt(n). Against the reference (engine at detune 0.08, spread 0.385 / 0.195 /
0 for unison 2 and 4): offsets within 0.06 cents (the reference's inner four-voice offset is
0.659 d, the engine's 2/3 d), channel gains within 0.009. The voices of a note share a group:
they count against the polyphony limit, the mono modes legato and reclaim the whole group, and
the host hears one note end when the last voice ends or is reclaimed. Decisions (for Jacob):
any voice count 1-16 is allowed (the reference has 2 and 4); in the mono modes a reclaimed group
keeps the size it started with if the count changes while the note is held (marked
`shortcut:`). Proposed tolerance: offsets within 0.1 cent, channel gains within 0.02. Not done:
a plugin check against the reference values (the `[unison]` tests check the engine's own
formula).

*Drift* (sine; drift 0-127; 6-43 loads per depth, 4 notes of 20 s at 127). With drift 0 the
pitch is exact (0.01 cents). With drift on, each oscillator's pitch moves in straight lines
between random values at nodes about 5.69 s apart (intervals 5.59-5.85 s from kinks in the long
notes; a one-break piecewise-linear fit leaves 0.004 cents RMS on 4 s notes). Oscillators move
independently, and so do the two voices of a unison note. The value at a note's start is spread
like the interpolation of independent nodes (at 127: mean +11.9, deviation 19.9 cents, range
-45.6 to +47.9, 43 loads; a Kolmogorov-Smirnov test against the engine below gives p = 0.30).
Deviation of the value at note start, cents (loads): 16: 0.9 (10), 32: 0.9 (16), 48: 1.8 (10),
64: 1.8 (16), 80: 3.6 (10), 96: 5.2 (16), 112: 10.5 (18), 127: 19.9 (43); means 0.1, 0.3, 1.2,
2.1, 3.4, 3.0, 4.0, 11.9, so the reference drifts sharp on average (about 0.5 deviations; real
in three separate batches, but the loads within a batch are correlated in time: consecutive
loads often start close together, so the reference's generator seems to run on between loads).
The cutoff drifts too (saw at note 36 through LPF24 at 512, harmonic 9 converted with the
engine's 1.9 dB per semitone): deviation 0.30, 1.12 and 5.5 semitones at 64, 96 and 127, mean
about 0, independent of the pitch drift, also in straight segments. Ageing (-100, 0, +100) has
no measurable effect on pitch (one voice, four unison voices, or with drift 127); on the cutoff
of voice 1 it moves harmonic 5 by +0.3 / -0.1 dB.

*Multi-note probes* (batch `m3e`, the coordinator's new sequence recorder `esp_seq`, 8 notes
of 1 s every 1.5 s on one key, or 12 of 0.8 s every 1.1 s). Drift 127 with each OSC Reset mode
(two runs each): every note starts at an unrelated value in all four modes (for example Off:
-2.1 after +19.7, +23.1 after +13.7), with one exception (Off, notes 1-2: +40.1, then +40.3).
Which voice each note used is not known, so this doesn't separate "each voice keeps its own
running generator" from "every note starts afresh"; the engine's per-note start is consistent
with it. Ageing -100 / +100 with drift 0, 12 notes: pitch within 0.1 cents on every note, and
the saw-through-LPF24 harmonic 9 identical on all 12 notes (-53.02 and -53.42 dB, against
-53.3 at ageing 0): a fixed cutoff shift of about +0.16 / -0.05 semitones, no spread between
notes. Ageing is not modelled; the converter can ignore it or fold that shift into the cutoff.

*Engine: segment drift* (`fb304aa`, generic). One generator per oscillator pitch and per filter
cutoff in each voice: normal nodes (mean 0, deviation 1) Segment Drift Time apart (2^value s,
default 2.5 = 5.66 s), linear between them; a voice starts at a random point of a random
segment. Segment Drift Pitch (semitones, shown in cents) and Segment Drift Cutoff (semitones)
scale the nodes; Segment Drift Offset adds to every oscillator's pitch. The instantaneous
deviation is sqrt(2/3) of the node deviation, so the converter sets Pitch = 1.22 x the table's
deviation and Offset = the mean. Engine check (300 s note, Pitch 0.24, Offset 0.12, Time 2.508):
mean 11.05, deviation 18.66 cents, segments 5.70 s, against the reference's 11.9, 19.9 and 5.69.
Decisions (for Jacob): no memory between notes (every note starts at a random point; for one
note this is the same distribution as a generator that never stops, but two notes in a row on
one voice are not correlated as the reference's seem to be); normal nodes (the node
distribution can't be told from 43 loads); Surge's own drift stays and the converter sets it to
0. Proposed tolerance: the start-value distribution at 127 passes a two-sample
Kolmogorov-Smirnov test at 5 %, segment time within 2 %. Not done: the drift table's fit (the
deviations above are the converter's table, points 16 steps apart with a jump at 127); a plugin
statistics check.

*OSC Reset* (saw at note 48, three loads per mode): Phase and Full start every note at the same
phase; Off and Tune at random phases. With drift 127, Tune and Full start notes at random
offsets just as Off and Phase do (no reset to zero). What Tune resets cannot be seen in one note
per load. Converter: Phase and Full set Surge's oscillator retrigger; segment drift already
starts each note afresh, which is what a drift reset would do.

*Oscillator waves* (note 48). Saw1 is a 1/n sawtooth. Saw2 is the sawtooth plus its own
fundamental: harmonics 2-40 are 1.145 (1.12-1.18) times Saw1's, the fundamental 1.492 times.
Surge's Modern oscillator gives this with the third shape set to sine at about -0.186 of the
saw (deform type 2; rendered at -0.19, harmonics 1-6 relative to the second are within 0.04 dB
of the reference's; -0.186 is interpolated), so
Saw2 needs no engine change: Modern, saw 100 %, sine -18.6 %, level +1.18 dB. Pulse width:
duty = raw / 255 (16: 0.065, 32: 0.130, 64: 0.254, 96: 0.377). Square, triangle and sine are
Modern's.

*Self sync is not hard sync.* At 1200 cents (raw 48) Surge's Classic and Modern sync give a
clean octave saw; the reference keeps a fundamental at -11 dB. At 3600 cents its spectrum is a
peak at harmonic 8 with symmetric sidebands. Period reconstructions show the slave restarting
at the master period with a fade around the restart (on a sine at 2400 cents the slave's first
cycle after the restart and the end of its last are attenuated; on a square the fade looks
longer). Not fitted. The wave shaper is not fitted either: on a sine it doesn't follow the
a sin(g x) curve that fits the saw probes up to 96 (fifth session; at 32 the sine's fifth
harmonic is 17 dB under its third, where sin(g x) gives about 39 dB), so it may not be one
static curve. The
"Analog" oscillator (shaper, self sync) is therefore not built; Saw1, Saw2, square, triangle and
sine map to Modern.

*Pitch LFO* (sine carrier, depth raw 309). Depth: the LFO swings +-half the pitch EG depth's
cents at the same raw value (raw 309: +-92.5; 280: +-23; 372: +-350; 494: +-2400; 128: -320,
inverted), so the converter uses the pitch EG depth lookup halved. Waves with key-on reset:
saw (rising, starts mid-ramp), square (high half first), triangle and sine (start at 0,
rising), random (held per period). Phase 90 / 180 / 270 degrees start the sine at its peak / 0
falling / trough. Key-on reset off: random phase per load. Delay: the LFO switches on fully
after t = 0.64 s x (raw / 24)^1.53 (raw 8-72 within 0.01 s of the half-amplitude time; 80
about 4.0 s from a coarse track; 4 not resolved).
Fade-in: times double every 16 steps (to 90 %: 0.89, 1.78, 3.53, 5.34, 7.13 s at 160, 176,
192, 200, 208), but 96 and 144 measured as instant; 144-160 needs probes. Speed: the amplitude
LFO table (third session). Surge's voice LFO covers these; the converter's start phases per wave
and the delay/attack mapping to Surge's LFO envelope are not checked against engine renders.

*Oscillator EG* (pulse width 50 %, PW EG depth +127, duty from the second-to-first harmonic
ratio): decay time constants 0.186 s (index 160) and 0.76 s (192) against the pitch EG's 0.196
and 0.82 s, so the oscillator EGs share the pitch/filter EG curves within about 5 %. PW EG depth
+32 moves the duty by 0.246 (about 1/130 per step); +64 saturates. Mapping the oscillator EGs
and LFOs (three each, plus pitch, filter, amp and modifier LFOs and the pitch and modifier EGs)
needs the 6 -> 12 voice modulators of milestone 2, which are not built yet: changing
`n_lfos_voice` renumbers the modulation sources that patches store by number, so it needs a
migration. Part LFO: its rate table is fitted in the measurement repository for the FM engine
(`fm/lfo_rate.py`, milestone M6 of that plan); the Part LFO is common to both engines but this
was not checked on an analog Part.

*Saturator level at high drive* (`204ac02`, generic). The saturators divided by the drive
(small-signal gain 1), so a full-scale sine fell 18 dB at +20 dB of drive, while the reference
stays within +0.6 to +3.6 dB of its clean level at every drive. They now divide by tanh(drive):
the converter's level correction after each saturator goes from +0.6 ... +19.6 dB (amp) and
+0.9 ... +16.5 dB (filter) to: amp, raw 8-80: +0.63, +0.89, +1.81, +1.25, +2.42, +3.31, +1.43,
-0.40 dB; filter: +1.19, +1.30, +0.91, +1.55, +2.25, +2.22, -0.74, -2.14 dB (`satgain.py`,
reference against engine fundamental at full scale). The harmonics are unchanged (rechecked:
amp raw 80 second harmonic -36.6 reference, -40.1 engine, as before). Still not met: the 3 dB
proposal at raw 80 (3.5 and 3.1 dB), and the reference's low-level odd harmonics.

*Render tool* (`3c0f557`, Oscairn-specific): `name.deform=n` sets a parameter's deform type.

*Measurement repository* (`6822ce9`): `analog/voice.py` (unison partials and pan, drift tracks
and segments) and `analog/shaper.py` (transfer curves, saturator fits, fundamental level, the
folder's harmonic RMS), with self-tests. `shaper.transfer` now picks the input polarity whose
output rises with the input (the symmetry search alone can return the curve mirrored; the
saturators' fitted asymmetry signs from the fifth session may be flipped, which doesn't change
their spectra). Not moved: the table gate (`gate.py`, local paths throughout) and the probe
builders.

**Tests run in the sixth session** (macOS arm64).
On the tree of each code commit (`bf59112`, `fb304aa`, `204ac02`, `3c0f557`):
`oscairn-analog-tests` passed in the standard, ASan/UBSan and TSan builds with no sanitizer
reports (standard 71-79 s, ASan 72-80 s, TSan 316-366 s; the plugin exposes 330 parameters
after unison, 334 after segment drift); `surge-testrunner` passed with `[reference]` included:
195, 198, 198 and 198 test cases, "Compared 635 patches, skipped 6" each time. New tests:
`[unison]` (offsets, pans and gains of four voices, one note end for the group, one voice
changes nothing, far-side attenuation and the 1/sqrt(n) level, the polyphony limit, the four
mono modes, reset on load and round trip) and `[segmentdrift]` (the generator's stationary
deviation sqrt(2/3) and its segment count, off by default with an exact 440 Hz, the offset
moving pitch by 50 cents, depth scaling each generator, reset on load and round trip); the amp
saturator test now checks the level against the tanh(drive) scaling and was seen to fail with
the old scaling. Measurement repository self-tests: 22 passed. CI: see the report for this
session.

**Seventh session (2026-10-31): voice modulators, LFO envelopes, pitch LFO.**

*Engine: twelve voice LFOs* (`1b1e292`, generic). `n_lfos_voice` is 12; `ms_lfo7`-`ms_lfo12`
follow `ms_lfo6` in `modsources`, so storage index = source - `ms_lfo1` still holds for every
LFO (voice 0-11, scene 12-17) and the code that maps a source to an LFO is unchanged. Streams
keep the old numbering: `modsourceToStreamed`/`modsourceFromStreamed` write sources as before
with voice LFOs 7-12 at 41-46 (after Latest Key), and `lfoStreamedIndex` (its own inverse)
keeps parameter names (`a_lfo6_*` is still scene LFO 1; voice LFO 7 is `a_lfo12_*`), step
sequences, MSEGs, formulas, labels, extra-amplitude flags and editor state where they were.
Old patches therefore need no conversion; `[reference]` passes unchanged. The new LFOs'
parameters (78 per scene) are appended to the scene and reset before a patch loads, with their
step sequences. Voice LFOs 7-12 assign and start only in voices whose scene has a voice routing
from them (or whose step sequencer retriggers an envelope): assigning an LFO draws from
storage's random generator, and without this every factory patch's randomness moved (484 of
635 `[reference]` patches differed in the first try). A routing added while a note plays reaches
them from the next note. Surge XT's modulation grid has no place for them: the editor skips
them when laying out source buttons (the `surge-xt` GUI target was built, not run). Found on the
way: `n_scene_params` must grow with the scene (it is only asserted in debug builds); the
plugin's LFO modules were all "Scene LFO 12" and up (`90a556c`, Oscairn-specific, with a test).
Not done: Python stubs (`surgepy`) still list six voice LFOs.

*Engine: analog LFO envelope* (`0c7d7c7`, generic). A per-LFO Envelope Mode (all 18 LFOs, 18
more scene parameters): Analog runs `ADSRModulationSource`'s analog mode on the LFO's attack,
decay, sustain and release after the delay stage. A voice LFO in this mode matches the filter EG
in analog mode within 1e-6 per block for three settings including release to silence
(`[lfoenvmode]`), so the pitch EG decay and release tables fitted against the filter EG
(fifth session) apply to an LFO-based pitch or oscillator EG unchanged. The plugin has 442
parameters (334 before; +78 LFO parameters, +12 LFO off switches, +18 envelope modes).
Range (`293c902`, Oscairn-specific): voice LFO attack and decay reach 2^8 s in the plugin; LFO
release keeps 2^5 s (its maximum means "never releases"), so oscillator EG releases above about
30 s cannot be reached.

*Mapping, for the converter:* pitch EG, oscillator EGs 1-3 and modifier EG → five voice LFOs,
shape Envelope, mode Analog, delay at minimum (and switched on: a deactivated delay makes the
envelope constant), hold at minimum; pitch, filter, amplitude, oscillator 1-3 and modifier LFOs
→ the other seven, with the measured delay and fade-in below; Part LFO → a scene LFO.

*Pitch LFO fade-in* (`m3d` `pfi_*` at speed 240, `m3b` `plfo_fi*` at 176; vibrato amplitude per
half LFO period from the cents track). The fade-in is an exponential charge toward 1.543 x full
amplitude, cut off when it reaches full: the shape of Surge's analog attack (toward 1.5, switch
at 1), fit RMS 0.00-0.02 cents. Time constants: 144: 0.494 s, 160: 0.991, 176: 1.981, 192: 3.961,
200: 5.940, 208: 7.919, 214: 10.505 (from the 2.4 Hz probe, normalised to the known full depth);
doubling every 16 steps, 200 exactly halfway in seconds, 214 between linear and log
interpolation. 32, 64 and 128 at 2.4 Hz and 96 at 11.9 Hz are too fast for the method (reaching
full within the first window; 128 looks like about 0.25 s). The sixth session's "96 and 144
instant" was wrong for 144. Engine mapping: analog-mode LFO attack = log2(0.957 tau) + 2 (tau =
2^(a-2) s in that mode; 0.957 makes the 1.5 target's curve closest to the 1.543 one, 0.4 % of full
at worst in theory). Engine renders (voice LFO 1 on oscillator 1's pitch, 44.1 kHz) against the
six `pfi_*` probes: largest vibrato-amplitude difference 0.47-0.91 % of full (proposed tolerance,
for Jacob: 2 % of full).

*Pitch LFO delay* (`m3d` `pdl_*` at 11.9 Hz; switch-on time by a grid search for a step on the
steady sinusoid fitted after it). The LFO runs from note-on (its phase at the switch is the
free-running phase, not a restart) and switches on fully: Surge's LFO delay. Switch-on times:
4: 0.049 s, 8: 0.130, 12: 0.239, 24: 0.669, 40: 1.429, 48: 1.881, 56: 2.370, 72: 3.459, 80:
4.049; t = 0.6656 (raw/24)^1.5 s fits all nine within 3.7 ms (replaces the sixth session's 0.64
(raw/24)^1.53 from half-amplitude times). Engine (LFO delay = log2 t of that formula) measured
the same way: within 2.5 % or 5 ms of the reference at 8 of 9 points; 4 is 5.3 ms early (-10.8 %);
48's engine fit is poor (RMS 24 cents) and reads +9.5 ms. The method's own error, from engine
renders at known delays, is about 2-6 ms. `m3b` `plfo_dl16/32/64`: 0.361, 1.019, 2.899 s at 2.4 Hz
(the formula: 0.363, 1.025, 2.900); 96 and 127: the method found no switch-on in the 5 s note (not looked into).

*Part LFO on an analog Part* (batch `m7a`, new: triangle Part LFO on oscillator 1's out level,
LFO box destination 83; the Part LFO has no pitch destination on an analog Part). Rates at
speeds 16, 24, 32, 40, 48: 0.2073, 0.4135, 0.8256, 1.6472, 3.2868 Hz, within +0.18 to +0.28 % of the
FM-engine Part LFO table (`m4/m6i/lfofit.json`), so one table serves both. Above 48 not checked
(the FM table's own fits are unreliable there).

*Self sync* (time-boxed, not modelled). A restarted slave (sine or ramp) with raised-cosine fades
at both ends of each master period, ratio and fade fitted to harmonics 1-40 of the `m3b` probes,
fits badly (5-8 dB RMS) and the best ratios don't follow the nominal ones (raw 24 on a sine: 2.51
for a nominal 1.41; raw 96: 7.59 for 4; saw raw 48: 3.22 for 2; saw raw 96: 2.98 for 4). Either
the pitch scale isn't 25 cents a step or the slave isn't a plain restarted wave; not resolved.
The wave shaper was not looked at again. The "Analog" oscillator stays unbuilt.

*Not done this session:* oscillator EG tables of their own (three probes from the sixth session
only), plugin checks of unison spread and drift statistics, the milestone 1/2 time-boxed items,
milestone 4.

*CI.* Pushes `90a556c` (with `1b1e292`), `0c7d7c7`, and `293c902` with this record started no workflow run (checked for the first two); a
manual dispatch was refused ("Actions has been disabled for this repository", HTTP 422) although
the org and repo settings show Actions enabled. No workaround was tried. For these commits the
Linux ARM64 jobs (standard, ASan/UBSan, TSan) and the CI macOS jobs are unverified; the local
gates below run on macOS arm64 only.

**Tests run in the seventh session** (macOS arm64). `1b1e292`: `surge-testrunner` 200 passed, 1
skipped (201 test cases); `[reference]` "Compared 635 patches, skipped 6"; `oscairn-analog-tests`
passed in the standard (424 parameters), ASan/UBSan (0 reports) and TSan (0 reports) builds; the
`surge-xt` GUI library built. `90a556c` (plugin only): `oscairn-analog-tests` standard, ASan and
TSan passed, 0 reports; the new module check was seen to fail without the fix; testrunner not
rerun. `0c7d7c7`: testrunner 203 test cases passed, `[reference]` 635 compared, 6 skipped;
`oscairn-analog-tests` standard (76 s), ASan (79 s) and TSan (359 s) passed, 0 reports.
`293c902` (plugin only): `oscairn-analog-tests` standard (76 s, 442 parameters), ASan (79 s) and TSan (354 s) passed, 0 reports; testrunner not rerun. New tests: `[voicelfos]` (stream numbering,
voice LFOs 7-12 modulate exactly like LFO 1, only routed ones run, stream and reset),
`[lfoenvmode]`, the plugin's LFO module and range checks.

**Where this stopped** (seventh session). Twelve voice LFOs and the analog LFO envelope are in;
the pitch LFO's delay and fade-in mappings are measured and checked against engine renders; the
Part LFO's rate table is the FM one. Next: oscillator EG tables (dense PW-EG probes), the pitch
LFO wave start phases against engine renders, self sync (probe the pitch scale first: a sync
sweep on a sine at fine steps), the shaper, then the items in the sixth session's list.

**Where this stopped** (sixth session). Milestone 3: voice unison and segment drift are in, OSC
reset maps to retrigger, Saw2 and the pulse width map to Modern, the pitch LFO's depth, waves,
phase and delay are measured. Not done: the "Analog" oscillator's wave shaper and self sync
(the shaper doesn't follow one simple curve and self sync isn't hard sync; more probes and a
model needed), ageing (no spread between notes, only a fixed cutoff shift of at most 0.16
semitones), the drift and spread tables committed as plugin checks, the LFO fade-in below 160,
the 6 -> 12 voice modulators (needed for oscillator EGs and LFOs), and the time-boxed milestone
1 and 2 items other than the saturator level (folder above 96 and Hard, saturator curve, filter
saturator offsets for other types, resonance above 96, short envelopes, noise tone).

**Where this stopped** (fifth session). Milestone 2 engine blocks are in (mixer invert and
post-filter, saturators, filter output gain, pre-filter shaper), all off by default with
`[reference]` unchanged; their off switches are plugin parameters. Next: a folder curve fitted
to the reference (soft/hard, texture) in place of sst's folders above amount 96; a
sharper-kneed saturator option if the 3 dB proposal at raw 80 matters; the saturators'
small-signal gain per drive into the converter; the per-oscillator wave shaper with the
"Analog" oscillator (milestone 3); short pitch EG decays (repeated probes); then milestone 3 (unison,
OSC reset, drift, ageing, pitch LFO, Part LFO), not started.

**Where this stopped** (fourth session). Next steps: pitch EG decay and release tables (dense
probes, which can serve the filter EG too; attack can use the amp table); the depth scale of
the filter EG (+4800 cents gave +51.2 semitones); oscillator EGs;
resonance above index 96 needs a new filter (ladder with LP 24 dB's knee) or stays approximate;
milestone 2 engine work from the measurements above: post-filter route and polarity per mixer
channel, then saturators (position to measure first), folder, shaper.

Earlier (third session):

**Where this stopped.** Next steps: index 944 for LPF12 and HPF24 (probe 940-950 for those
types); resonance (LPF24 resonance 0-255 at cutoffs 512 and 768 is recorded,
not fitted); a shorter analog decay in the engine (a generic change: the decay's slow
tail); probe the other envelopes and oscillator and mixer
levels; then milestone 2.
