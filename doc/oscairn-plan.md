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

The converter is an Oscairn tool, kept outside this fork. It reads the AN-X Part blocks of a
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
| Fitted tables, converter | No: they live in Oscairn's tools, not here |

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
11. Tuning: Oscairn as the MTS-ESP master (plugins keep Surge's MTS-ESP client) or tuning
    only from each plugin's saved state (build with `SURGE_SKIP_ODDSOUND_MTS`). Jacob is
    deciding; the Oscairn build keeps the client until then.

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
tolerance. Recorded from commit `53886aa` (the RNG change, the first reproducible build); 633
of 641 patches compared, 6 left out as nondeterministic (above), 2 more because they render
differently twice within one process. This is the "old patches render as before" check every
later milestone runs.

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
  Surge's migrations are not idempotent for 83 older factory patches (revision 9/10): the first
  save after loading one differs from the next save. The test therefore requires the fixed
  point (save, load, save gives the same bytes) for every factory patch.
- *Notes.* CLAP note-on with velocity 0 stays a note-on. MIDI note-on with velocity 0 is a
  note-off, as MIDI defines. Note port dialects: CLAP (preferred) and MIDI; no MPE.
- *No Lua.* Patches with a Formula modulator in scene A are refused (the previous state is
  kept). Built with `SURGE_SKIP_LUA` and `SURGE_SKIP_JUCE_FOR_RACK`: `surge-common` does build
  without JUCE (verified on macOS arm64).
- *MTS-ESP: kept for now* (open question 11). Surge's MTS-ESP client stays in the Oscairn
  build, because Oscairn may become the MTS-ESP master and save tuning with the Performance.
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
| Reference renders | `[reference]` against the `53886aa` recording | 633 patches identical within 1e-5 |
| Linux ARM64, CI | `.github/workflows/oscairn.yml` (macOS arm64, Linux ARM64, ASan/UBSan and TSan on Linux ARM64, test runner on macOS) | written; first run on GitHub recorded below |

Not covered by the TSan test: a parameter change concurrent with a state save. The save's
retry makes the result correct, but the read it discards is still a data race in the C++
sense; TSan would report it. Leak checking was off (LeakSanitizer isn't supported on macOS
arm64). Value text for oscillator controls reads the parameter's current
type, which the audio thread changes when the oscillator type changes (marked `shortcut:` in
the code).
