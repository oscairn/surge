# Oscairn extension plan: an analog-modelling engine on Surge XT

Status: plan, 2026-10-30. Milestone 0 is being implemented on branch `oscairn`; section 10
records what was built, how it was tested and where it departs from this plan. This fork is
[oscairn/surge](https://github.com/oscairn/surge), forked from
[surge-synthesizer/surge](https://github.com/surge-synthesizer/surge) at `5a4828a`.

The goal: extend Surge XT so that one instance can do everything a Yamaha MONTAGE M / MODX M AN-X
Part can do, and ship it as an Oscairn CLAP instrument that supports all of the host's
recommended features (`docs/plugin-porting.md` in [Oscairn](https://github.com/oscairn/oscairn)). Yamaha
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
| Mono release returning to the held key when several voices hold the released key (`84cac66`) | Likely: a bug in Surge's own code, though without unison only reachable through an uber-released voice on the same key (not reproduced) |
| Every unison voice gliding (`62bd6a2`) | With voice unison only (the loop it fixes is the fork's) |
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
for Jacob to confirm (`tolerances.json`; accepted by Jacob in the twelfth session, with the later
ones listed there):

| Feature | Tolerance | Why |
| --- | --- | --- |
| Amp level | 0.1 dB | All checks within 0.04 dB |
| LFO speed | 2 % | Measurement resolves 0.2 %; checks are within 0.7 % below index 290, but up to 1.95 % next to the breakpoints above it (8-index knots straddle them). Identifying the breakpoints would allow 1 % (the FM engine's gate) |
| Envelope times | 2.5 % or 5 ms | Release and decay within 1.8 %, attack within 2.4 %, from 8-index knots; 5 ms is about twice the reference's render-to-render spread at short times. The FM gate (2 %) would need step-4 knots |
| Cutoff | 50 cents | Unchanged |
| Aliasing | floor -70 dB (power off the harmonics relative to on them) | Accepted (twelfth session); see below |

**Results** (Surge's analog envelope mode, drift off; "checks" are indices the table was not
fitted from):

| Table | Surge parameter | Valid indices | Checks | Committed |
| --- | --- | --- | --- | --- |
| Amp level | `a_vca_level` | 56-511 (below: under -63 dB, clamped) | 57 of 57 within 0.1 dB | yes |
| LFO speed | `a_lfo0_rate` | 18-380 | 46 of 46 within 2 % (242 re-recorded: 0.47 %) | yes |
| Amp EG attack | `a_env1_attack` | 100-255 | 17 of 17 (third session, with the longer range) | yes |
| Amp EG decay | `a_env1_decay` | 72-255 (72-95 from averaged probes, thirteenth session) | 17 of 17; below 100, 2 of 2 averaged checks within 2.5 % or 2 ms | yes |
| Amp EG release | `a_env1_release` | 112-255 | 16 of 16 (108: 8.4 % off, outside) | yes |
| Cutoff, LPF24 | `a_filter1_cutoff` (LP 24 dB, resonance 0.06) | 320-1004 (note 24 below 384, note 36 from there) | Third session: fitted from every 8th index plus the first index after each kink, pieces between kinks interpolated separately: 154 of 154 within 50 cents, 51 of them next to kinks (median 3 cents at note 36, 7 at note 24; largest 20 and 35) | yes |
| Cutoff, LPF18 / LPF12 / LPF6 | Legacy Ladder 18 / 12 / 6 dB | 384-992 / 384-992 / 384-944 | LPF24 shape plus a smooth difference from a step-32 sweep; 19 of 19 / 24 of 24 (fourth session, with probes at 936-960; before: 18 of 19, 944 143 cents off) / 18 of 19 (976: 50.4 cents) | yes, the valid spans |
| Cutoff, HPF18 / HPF12 / HPF6 | Xpander HP3 / OB-Xd 12 dB HP / Xpander HP1 | 384-992 | 19 of 19 each (largest 34, 25, 25 cents) | yes |
| Cutoff, BPF12 / BPF6 | BP 24 dB / BP 12 dB | 416-992 / 384-992 | 18 of 18 / 19 of 19 | yes |
| Cutoff, HPF24 | HP 24 dB Clean, resonance 0.05 (fourteenth session; before: HP 24 dB Standard) | 352-992 | 33 of 33, with probes every 4 between 528 and 592 (fourteenth session). Standard, before: 24 of 24 (fourth session, with probes at 936-960; before: 16 of 16 up to 912, 944 129 cents off); at resonance 0 (the first choice): 14 of 17 | yes |
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
- Added in the eighth session (details there): analog envelopes in the plugin end their attack
  at 1 instead of overshooting (a storage option, off in Surge XT, on in the plugin, not
  streamed: a Surge XT patch from the converter would overshoot); the plugin refreshes the
  scene data after every load; the pitch LFO saw maps to Surge's ramp inverted with its phase
  moved by half a period; the proposed tolerance for the oscillator EG (duty fall) times is the
  envelope one (2.5 % or 5 ms).
- Added in the ninth session (details there): the amp velocity amplitude law is a storage option
  (off in Surge XT, on in the plugin, not streamed), with k = -(Velocity > VCA Gain) / 12 so the
  existing parameter carries it; negative sensitivities are left to a velocity routing. Key-on
  delay is a generic scene parameter in the engine (not the plugin), block-quantized, with a
  fixed array of 128 waiting notes; a note released before it starts never sounds and is ended
  at once (as the reference: silent), and the sustain pedal doesn't hold a note that hasn't
  started. The plugin's portamento range reaches 2^3.99 s; Portamento Time 122-127 clamp
  there. Proposed tolerances: key-on delay 2.5 % or 15 ms, portamento 5 % or 5 ms, amp
  velocity 0.3 dB, cutoff drift spread 20 %, pitch LFO start phases 6 cents RMS.
- Added in the tenth session (details there): the noise tilt is two scene parameters (a
  first-order shelf: gain 1 below the frequency, the tilt above), skipped at 0 dB, the default;
  the algebraic saturator curve is an engine option like the attack clamp, not a parameter;
  the three reference-matching engine options (attack clamp, amplitude velocity law,
  algebraic saturators) are now saved in the patch, each its own setting (generic names, so
  each means something in Surge XT), off when absent, and the plugin no longer forces them:
  a new or existing Surge patch plays as in any Surge instance, and the converter turns all
  three on for a converted Part (replaces the eighth and ninth sessions' "on in the plugin,
  not streamed"); no plugin parameter for them, only the state (a patch-level choice, not
  something to automate); a fourth such option, `lfoLongRelease`, lets voice LFO releases
  reach 2^8 s with 8 as "never" (off: 5 and above never release, as in Surge); the portamento octave limit is a scene switch (off by default)
  that applies to both time modes; the plugin's Velocity > VCA Gain reaches -1536 dB (k = 128)
  and above +192 the check is on k (2 %), not the level; the pitch LFO delay maps through
  measured values at indices 2-6 instead of the formula. Proposed tolerances: noise tone shape
  1 dB RMS and level 1 dB over half-octave bands; saturator harmonics above -50 dB within 3 dB
  (as before, now also for the other filter types); portamento octave limit jump 0.5 semitone
  and slope 5 %.

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

**Eighth session (2026-10-31): Linux checks without CI, oscillator EG, a first-block bug.**

*Local substitute for the Linux CI jobs.* GitHub Actions stays disabled for `oscairn/surge`
(no runs since `cac0d8e`). The workflow's three Linux ARM64 jobs were reproduced in Docker on
this Mac (aarch64): image `surge-ci-linux:24.04` (`ubuntu:24.04` plus build-essential, cmake,
ninja-build, git, python3: GCC 13.3, CMake 3.28.3, Ninja 1.11.1; the runner image's tool
versions were not compared), the
workflow's configure flags, targets and test command and its `UBSAN_OPTIONS`/`TSAN_OPTIONS`,
on an exact export of each commit's tree with its submodules at their gitlinks (`git archive`,
recursively; script and logs in `forks/surge-ci-linux/`, outside the repository). Memory: 7 GB
per container, 6 compile jobs (ASan with 6 was killed during the build, so 4). This is a local
substitute, not CI: one machine, Docker's Linux VM, not GitHub's runner image. Results so far
(each "passed" = exit 0, "all passed", 0 sanitizer reports): `59aacd6` standard (85 s of
tests), ASan/UBSan (2560 s), TSan (298 s); this covers the seventh session's `1b1e292`,
`90a556c`, `0c7d7c7` and `293c902` only as part of that tree, not one by one. Further commits: see "Tests run in the eighth
session". Every commit pushed this session had the standard job run on it, and the engine
changes the sanitizer jobs, before or soon after its push.

*Python bindings* (`9ef05b1`, generic): `ms_lfo7`-`ms_lfo12` in the constants and the mod
source map, and the constants stub renumbered (every source after `ms_lfo6` moved up by six).
Not part of any gate: built separately on macOS (`SURGE_BUILD_PYTHON_BINDINGS=ON`, Lua off);
every `ms_*` value in the stub matches the built module, and the Python tests pass except
`test_check_formula`, which needs Lua (36 passed, 1 skipped, 1 failed for that reason).

*Oscillator EG tables* (batch `m8a`: square at note 48, pulse width raw 64, PW EG depth +16,
attack 0; decay with sustain 0, release from sustain 511, indices 128-208 every 4, 128-156
twice; amp release 240). The duty is read from the first two harmonics (acos(h2/h1)/pi, a new
`duty_track`/`duty_fall_time` and fit kind `dutyfall` in the measurement repository, `e83746f`),
and timed like the pitch EG: a straight line through log(duty offset) between its 90 % and 10 %
crossings. Base and full duty: 0.2538 and 0.377 in the reference, 0.254 and 0.377 in the
engine (Modern pulse, width 0.254, voice LFO 1 in analog envelope mode on the width, depth
0.123). Repeat renders agree within 0.5 % (the pitch track scattered by up to 14 % below 148).
The oscillator EG's decay and release times are the same curve (within 1.7 % at every index)
and match the pitch EG's reference times within 2.8 % at the pitch EG decay's check indices
(148-204); the oscillator EG tables also reach down to 128. Committed (`a07069b`, refitted in `7264e87`, see below) as `oscEgDecay` and
`oscEgRelease`, valid 128-208, 10 of 10 checks each within 1.55 % (fitted from every 8th
index, checked at the ones between), with the same measurement in C++ (`dutyFallTime`, the
straight-line fit now shared with `pitchFallTime`); moving a reference value by 5 % fails the
check (seen). Tolerance: the envelope one, proposed (2.5 % or 5 ms).

*Bug: a note in the first block after a load started with stale parameters* (`7264e87`,
Oscairn-specific). A voice copies its parameters from the scene data, which Surge refreshes
from the patch at the start of each engine block. The render tool and the plugin tests load a
patch and start the note at the first sample, so the voice started with the scene data of
before the load (all zero after init): LFO start phases, read only when a voice starts, were
lost for the whole note (found because `a_lfo0_phase` had no effect on renders), and level and
envelope settings were wrong for the first 32 samples. The plugin now refreshes the scene data
after every load and at init; a test checks that a note in the first block after a load starts
at the loaded LFO phase (it fails without the fix, seen). This changed renders, and so:

- *Analog attack overshoot is real in the plugin.* The fifth session saw no overshoot in
  plugin renders "and didn't understand why": it was this bug. With it fixed, Surge's analog
  ADSR at the shortest attack overshoots by 2.9 dB, decaying with the decay time (still +2.3 dB
  after 0.3 s at decay 2), which the reference doesn't do. New generic storage option
  `analogEnvelopeAttackClamp` (`fd6c495`): scene and LFO envelopes in analog mode switch to
  decay in the block their level passes 1, held at 1 (test: `[lfoenvmode]` "Analog Envelope
  Attack Clamp"; off it overshoots above 1.3, on it peaks at exactly 1 and the LFO envelope
  still follows the filter EG within 1e-6). Off by default and not streamed, so Surge XT and
  `[reference]` are unchanged; the plugin turns it on.
- *Tables refitted.* With the fix, the amp EG decay checks failed by +18 to +27 % (0 of 17;
  the old engine renders were timed from a 10 dB level transient at the note start) and the
  oscillator EG decay by up to -14 %. Both, and the oscillator EG release, were refitted from
  new engine sweeps (`surge/eglong2`, `surge/oegsweep2`) with the same method and gates: amp EG
  decay 17 of 17 (largest +4.5 % at 100, inside the 5 ms floor; otherwise within 1.8 %; before:
  17 of 17 within 1.8 %), oscillator EG decay and release 10 of 10 each within 1.55 %. Every
  other table's checks pass unchanged with the fix.
- *Pitch LFO delay and fade-in rechecked* with the fixed render: fade-in largest difference
  0.37-0.74 % of full (seventh session: 0.47-0.91 %); delay switch-on times as before (8 of 9
  within 2.5 % or 5 ms; 4 is 5.4 ms early, -11 %; 48 reads +9.4 ms with a poor engine fit).
- *Short envelopes (time-boxed, nothing committed).* The third session's short-envelope gates
  (single renders at note 96, indices 0-120, scene highpass on) rerun on a new engine sweep
  (`surge/egshort2`): decay 10 of 15 checks (before the fix: 6 of 15; the engine's shortest
  decay is now 14 ms to -40 dB, against 3-6 ms in the reference at 4-36; 92 and 108 are -12 and
  -6 %), attack 14 of 15 (116: -17 %, as before), release 13 of 15 (36 and 44: the engine's
  12 ms floor, as before). Averaged reference probes are still the way to commit these.

*Noise tone (time-boxed, not fitted)* (batch `m8b`: noise only at out level 511, tone 0-127
every 16, against Surge's noise colour -1..1 in steps of 0.05; spectra in octave bands from
62.5 Hz to 16 kHz relative to 1-2 kHz). Tone 64 is white, like colour 0 (bands within 0.7
dB). The reference's tone is a gentle tilt over the whole range: tone 0 has the lows 5 dB up
and the highs falling to -9 dB at 8-16 kHz, tone 127 the lows 16 dB down and the highs 19 dB
up. Surge's colour acts mostly at the top octaves, so the best colour per tone fits only from
48 to 112 (colour -0.05, 0, 0, +0.1, +0.3 at 48, 64, 80, 96, 112; 0.4-1.9 dB RMS over the
bands, worst 2.9 dB); 0-32 and 127 are 3.4-6.2 dB RMS off (worst 13 dB at tone 0). The tone
also moves the reference's level (-37.3 dB at 0, -26.8 at 64, -25.3 at 96-112, -27.5 at 127,
in this setup). A real match needs a tilt filter on the noise (a generic engine change);
for now the converter can use the colours above with a level correction, and tones below 48
stay approximate.

*Amplitude level velocity sensitivity (milestone 4, measured, not mapped)* (batches `m8c`,
`m8d`: sine at note 60, Part velocity depth and offset at their defaults, sensitivity S from
-128 to +255, velocities 1-127; level 0.2-0.7 s after the onset). The level follows
40 log10(a) dB relative to velocity 127 (S > 0) or to velocity 0 (S < 0), with the amplitude
factor a = 1 - k (1 - v/127) for S > 0 and a = 1 - k v/127 for S < 0, silent where a <= 0, and
k = 2^((|S| - 128)/64): k = 0.5, 1, 2 at |S| = 64, 128, 192 (every audible probe within 0.13
dB; the silent ones are where the formula gives below -60 dB). At +255 only velocity 127 sounds (118-126 silent), where the formula
gives k = 3.95 and sound down to velocity 96: the top value is a special case or the scale
steepens; not resolved. At S = 0 the level doesn't depend on velocity. Surge's amp velocity
(`vca_velsense`) is linear in dB (gain = level + sensitivity x (1 - v/127) dB), so it can match
the reference at one velocity only; matching the curve needs an engine option for this law
(a generic change, not made).

*Portamento time modes (milestone 4, measured, not mapped)* (batch `m8e`, the sequence
recorder: mono, Part and performance portamento switches on, fingered, a sine; note 48, then
48 + 5, 12 or 24 legato; the performance's master portamento switch is off by default and
has to be on). Every glide is a straight line in cents (90-50 % over 50-10 % time 0.9-1.25, glides without the octave jump below).
At an octave all four modes take the same time: 10-90 % 0.036, 0.074, 0.259 and 1.275 s at
time 16, 32, 64 and 96. Rate 1: constant rate (two octaves take twice as long). Time 1:
constant time (5, 12 and 24 semitones within 3 %). Rate 2 and Time 2: as Rate 1 and Time 1 up
to an octave, but a larger interval first jumps to an octave below the target, then glides
the last octave (at 24 semitones the pitch steps by 12 at once). Surge has constant time and
constant rate (`porta_constrate`) and a linear curve; the "2" modes' octave limit has no
Surge equivalent (an engine option would be needed). Surge's portamento (through `surgepy`,
linear curve) glides in 2^v s (10-90 % = 0.8 x 2^v within 1 % at v = -3, -2, -1.5, -1, -0.5; below -3 the
measurement is noisy), and with constant rate takes 2^v s per octave, so Time 1 maps to
constant time and Rate 1 to constant rate with v = log2(the reference's octave glide time):
-4.47, -3.43, -1.63 and +0.67 at time 16, 32, 64 and 96 (four points; a table needs denser
probes, and engine renders through the plugin weren't compared).

*Pitch LFO start phases against engine renders* (voice LFO 1, 2.397 Hz = the LFO speed table
at 176, on oscillator 1's pitch, against the `m3b` probes, cents tracks compared over the
first 1.25 s). Sine and triangle start like Surge's sine and triangle at phase 0, square like
Surge's square (high first), all with a constant -0.005-period shift (the reference's onset
latency, about 2 ms) and 0.26-0.32 cents RMS after it (square 4.0, its edges). The saw rises
from 0; Surge's ramp falls from its top, so the converter uses the ramp with a negative depth
and phase 0.5 (2.9 cents RMS after the shift). Phase 90/180/270 degrees = Surge phase
0.25/0.5/0.75 on the sine (0.26-0.34 cents RMS). Before the fix above none of this could be
checked: the phase was ignored.

*Unison in the plugin against the reference* (`23be4b0`, test only). Unison 2 and 4 at detune
8 and spread 0, 4, 8, 15 (2 voices) and 0, 4, 8 (4 voices) through the plugin at the
converter's values (detune 7.98 cents; spread = the far-side position x: 0.186, 0.385,
0.717), each partial's offset and channel level against the reference's: largest difference
0.05 cent (inner four-voice offset, 2/3 against 0.659) and 0.009 in level; proposed tolerances
0.1 cent and 0.02 (a moved reference fails, seen).

*Drift in the plugin against the reference* (`1627e4d`, test only). 150 notes in one instance
(sine at note 60, segment drift pitch = 1.22 x the reference's start deviation), the
deviation of each note's mean pitch 30-200 ms after its start: 18.5 cents against the
reference's 19.9 at drift 127 (43 loads), 9.8 against 10.5 at 112 (18 loads); proposed
tolerance 20 %; without the 1.22 factor it fails (15.2, 8.0; seen). The reference's positive
mean (+11.9 cents at 127) is not modelled, and the cutoff drift has no plugin check.

**Tests run in the eighth session.** macOS arm64 gates (`analog_gates2.sh`) on the trees of:
`a07069b` (includes `9ef05b1`): `surge-testrunner` 203 test cases passed, `[reference]` 635
compared, 6 skipped; `oscairn-analog-tests` standard (82 s), ASan/UBSan (90 s), TSan (436 s)
passed, 0 sanitizer reports. `fd6c495`: testrunner 204 test cases passed, `[reference]` 635/6;
plugin tests standard (78 s), ASan (84 s), TSan (396 s) passed, 0 reports. `7264e87`:
testrunner 204 passed, `[reference]` 635/6; plugin tests standard (79 s), ASan (85 s), TSan
(389 s) passed, 0 reports. `23be4b0` and `1627e4d` (plugin tests only; testrunner not rerun):
standard (84 s, 91 s), ASan (97 s, 92 s), TSan (496 s, 432 s) passed, 0 reports. Docker Linux (local substitute for CI, as above):
each job exited 0 with "all passed" and no sanitizer reports. Standard: `9ef05b1` (83 s of tests),
`a07069b` (86 s), `fd6c495` (100 s), `7264e87` (86 s), `23be4b0` (88 s), `1627e4d` (88 s).
ASan/UBSan and TSan, for the engine changes: `fd6c495` (2687 s, 337 s) and the head `1627e4d`
(which holds `7264e87`'s): 2756 s, 364 s. This record (`doc/` only) was not run separately.
Measurement repository (`e83746f`: duty track and `dutyfall`; `13332d5`: `glide_times`): self-tests
24 passed.

**Ninth session (2026-10-31 to 11-01): amp velocity law, key-on delay, portamento table, cutoff drift.**

GitHub Actions stayed disabled (no runs since `cac0d8e`); every commit below was checked with the
local Docker substitute for the Linux jobs (see "Tests run in the ninth session"), which is not CI.

*Amp velocity sensitivity* (`2b2ac55` generic, `350d952` Oscairn). New storage option
`vcaVelocityAmplitudeLaw`: the amp gain is a * a with a = 1 - k (1 - velocity/127), silent where
a <= 0, and k = -(Velocity > VCA Gain) / 12, so the parameter's -48..0 dB spans k = 4..0; off by
default and not streamed (Surge XT and `[reference]` unchanged), on in the plugin, like the
attack clamp. Test `[signalpath]` "VCA Velocity Amplitude Law" (k = 0.5, 1, 2 at four
velocities within 0.05 dB of 40 log10(a), silent where a <= 0, and the old dB-linear law when
off; fails with the option ignored, seen). Converter: Velocity > VCA Gain = -12 k dB with
k = 2^((S - 128)/64) for S > 0. Plugin check against the reference (`m8c`, `m8d`: S = +64, +128,
+192, 13 velocities): largest difference 0.136 dB, proposed tolerance 0.3 dB (fails with the
option off, seen: -5.95 against -11.90 dB at S = 64, velocity 1). Not covered: S < 0 (the
reference inverts velocity, a = 1 - k v/127; Surge's parameter can't go positive, so the
converter needs a velocity-to-level routing instead, which is linear in dB) and S = +255 (only
velocity 127 sounds; unresolved, as before).

*Key-on delay* (`3c7e451` generic, `4062aae` Oscairn tests). New scene parameter Key-On Delay
(`keyon_delay`, Surge's deactivatable envelope time: 2^value s, -8..5, off by default,
tempo-syncable): `SurgeSynthesizer::playNote` holds a note-on back for its scene's delay in a
fixed array of 128 waiting notes (past that a note starts at once, marked `shortcut:`), and
`process` starts it at the start of the block its time comes in (block-quantized: 0.7 ms at
44.1 kHz). A note released, choked, released by note ID, or ended by all-notes-off or a stop
(a state load) before it starts never sounds, and its end is reported at the next block. The
parameter is appended to the scene and reset before a patch loads. Tests: `[keyondelay]`
(starts after 0.25 s to the block; tempo sync doubles it at 60 BPM; released before it starts,
by key, by note ID or by all-notes-off: silent and ended; another key's waiting note
unaffected; an old patch loads with it off) and plugin checks (a delayed note starts at 12000
samples; released before it starts: silent, NOTE_END; loaded over: silent, NOTE_END). Undone
once each: without the delay 4 assertions fail; without the cancel in `releaseNote` 2; without
the cancel in `stopSound`/`allNotesOff` the load check fails.

Reference (batches `m9b`, `m9d`, `m9e`, `m9f`: a sine at note 60, Key On Delay Time Length
0-127, holds 4 or 16 s). The recorder's note times move between renders, but note-on and note-off
move together: the time from note-off to the release knee read 6.8-7.0 or 19.1-19.2 ms at hold 4
(four renders) and 33.6 ms at hold 16. That looks like the recorder's events landing on
about 12 ms buffer boundaries (inferred, not verified). The delay is measured as
hold + that lag (mean of the four at hold 4) - (onset to release knee) (`held_length`, fit kind
`keyondelay`). 0 is no delay; 1: 21 ms, 2: 27, 3: 31, 4: 54, 8: 143 ms (means of three
renders), 16: 0.36 s, 32: 1.04, 64: 2.91, 96: 5.33, 127: 8.10 s. Above about 64 it rises by
about 90 ms per step. A note released before its delay ends never sounds (index 96 with a
0.05 s hold: silent). Tempo sync at the default tempo 120 follows the note length: 16th 117 ms,
8th 245, 4th 503, 2nd 1002, 4th x4 2001, 4th x8 3999 ms (within the recorder's step), so the
converter sets Key-On Delay's tempo sync and 2^value = the note length at 120 BPM. Table
`keyOnDelay` (exponent per index 1-127; fitted from 24 renders, checked at 19 others, the last
6 at hold 16): 19 of 19 within 2.5 % or 15 ms, all within 2.5 % except index 5 (+9.0 ms,
+14.9 %) and 18 (-13.8 ms, -3.1 %), inside the floor. Proposed tolerance: 2.5 % or 15 ms (the
recorder's step, not the envelope's 5 ms). The plugin test renders the table's values and
checks the onsets (moving a reference value by 6.6 % fails it, seen).

*Portamento* (`414c6d2`, `566c7a4`, both Oscairn). The render tool takes `--legato n
--legato-at s` (a second note, the first released 0.1 s later). Reference (batches `m9a`,
`m9c`, `m9g`, `m9h`: Time 1, an octave from 48 or, for the short glides, from 84; at an octave
every time mode takes the same time, eighth session): 0 is no glide; 1: 24.5 ms, 8: 31.7 ms,
32: 71 ms, 64 about 0.26 s, 88: 0.81 s, 100: 1.62 s, 112: 3.97 s, 116: 5.44 s, 120: 8.87 s,
121: 11.3 s, 122: 13.1 s, 124: 18.5 s; 127 is longer than the 28 s the render allowed. Every
glide is straight in cents ((t90 - t50)/(t50 - t10) 0.76-1.17 below 0.3 s, 0.98-1.03
above). One `121` render had a dropout and was recorded again. Engine through the plugin: an
octave takes 0.8 x 2^v s (10-90 %) within 1 % for v >= -3 and 0.2 % for v >= -1.5 (below -3
the measurement reads 0.77-1.05 of it, the resolution of the track). The plugin's portamento
range now reaches 2^3.99 s (Surge stops at 2^2; the engine clamps at 2^4); loaded values above
Surge's range already played, so this only fixes the parameter's range. Table
`portamentoTime` (Surge's portamento per index 1-121; fitted from 20 renders, checked at 16):
16 of 16 within 5 % or 5 ms (a proposed tolerance; under the envelope tolerance, 2.5 % or
5 ms, 14 of 16: 60 is +2.7 % (6.1 ms) and 117 -4.2 %; the largest errors at the top come from
the curve steepening between probes). Index 0 maps to Surge's minimum (off); 122-127 glide
longer than the engine can and clamp at 2^3.99 s. Rate 1 maps to constant rate and Time 1 to
constant time with the same table; the "2" modes' octave jump has no engine equivalent (not
built). Plugin test: mono fingered portamento, an octave
from note 84 at the table's values (moving a reference by 5.8 % fails it, seen).

*Cutoff drift in the plugin* (`3a7ce21`, test only). The reference's cutoff drift (sixth
session: 10 loads per depth in `m3d`), with 30 more loads at 96 and 127 (`m9i`): harmonic 9 of a
saw at note 36 through LPF24 at index 512, measured at the note's own pitch (the pitch drifts
too; measured at the nominal pitch, the Hann window's scalloping had read 4.48 and 25.9 dB):
spread 0.58 dB at 64 (10 loads), 2.28 dB at 96 and 9.80 dB at 127 (40 loads each), that is
0.29, 1.16 and 4.97 semitones at the engine's 1.97 dB per semitone there (sixth session: 0.30,
1.12, 5.5). The plugin test plays 150 notes with Segment Drift Cutoff = 1.22 x the deviation
(1.37 and 6.71 semitones) and gets 2.01 and 9.69 dB (-12 % and -1 %); proposed tolerance 20 %,
as for the pitch. Without the 1.22 factor 96 fails (1.65 dB, seen); 127 alone would not
(7.99 dB, -18 %).

*Pitch LFO start phases in the plugin* (`aa71d84`, test only). The eighth session's
comparison as a plugin check: voice LFO 1 at 2.397 Hz on oscillator 1's pitch, +-92.5 cents,
key trigger, against the `m3b` probes; the mean pitch of each 50 ms from 0.05 to 1.25 s after
the onset. RMS over the 24 bins: sine 2.20, triangle 2.02, square 4.33, saw (ramp inverted at
phase 0.5) 2.56, sine at 90/180/270 degrees 2.88/2.18/2.62 cents; an inverted LFO is 96-170
cents off. Proposed tolerance 6 cents (the reference's onset latency of about 2 ms stays in).
The saw mapped as a plain ramp at phase 0 fails (39.8 cents, seen).

*Not done this session* (time-boxed or blocked): folder above 96 and Hard, saturator curve at
high drive, filter saturator offsets for other types, resonance above 96 (each needs a new
curve or filter; not started); short envelopes (still waiting for the decision on the floor for
averaged checks); the noise tone's tilt filter (not started); the pitch LFO delay at 4 (not
looked at again); self sync and the wave shaper (no model).

**Tests run in the ninth session.** macOS arm64 gates (`analog_gates2.sh`) on each commit's
tree: `2b2ac55`: `surge-testrunner` 205 test cases passed, `[reference]` 635 compared, 6
skipped; `oscairn-analog-tests` ASan/UBSan (96 s) and TSan (446 s) passed with 0 reports; the
standard plugin run (93 s, passed) is not counted: its binary was rebuilt with uncommitted
changes while the gate script ran, possibly before that run started (the Docker standard job
covers that tree, and `350d952`'s macOS runs hold the same engine). `350d952`: testrunner 205
passed, `[reference]` 635/6, plugin tests standard (92 s), ASan (126 s), TSan (535 s) passed, 0
reports. `3c7e451`: testrunner 206 passed, `[reference]` 635/6, standard (81 s), ASan (90 s),
TSan (436 s), 0 reports. `4062aae`, `414c6d2`, `566c7a4` and `3a7ce21` (plugin tests only: the
testrunner doesn't build anything they change, so it was not rerun): standard 81, 81, 85, 91 s;
ASan 93, 93, 96, 110 s; TSan 456, 460, 466, 574 s; all passed, 0 reports. `aa71d84` (plugin
tests only, the plan's edits uncommitted in the tree): standard (86 s), ASan (103 s), TSan (539
s) passed, 0 reports. A full testrunner run in a separate build of `3c7e451` plus the then
uncommitted plugin changes also passed (206). Docker Linux (local substitute for CI, as in the
eighth session; each job exited 0 with "all passed" and no sanitizer reports): standard on
`2b2ac55` (85 s of tests), `350d952` (86 s), `3c7e451` (87 s), `4062aae` (89 s), `414c6d2` (92
s), `566c7a4` (89 s), `3a7ce21` (93 s), `aa71d84` (91 s); ASan/UBSan and TSan on the head of
each push, which holds the engine changes before it: `350d952` (2788 s, 363 s; covers `2b2ac55`)
and `566c7a4` (2946 s, 394 s; covers `3c7e451`). This record (`doc/` only) was not run
separately. Measurement repository (`07ffb9c`: `held_length`; `8f89922`: fit kinds `glide` and
`keyondelay`, per-render options, proposed tolerances): self-tests 25 passed.

**Tenth session (2026-11-01): saturator curve, noise tilt, portamento octave limit, velocity to +255.**

GitHub Actions stayed disabled (no runs since `cac0d8e`); the Linux jobs ran in the local Docker
substitute (see "Tests run in the tenth session"), which is not CI. New batches: `m10a` (noise
tone between the `m8b` tones, filter saturator after every type, amp velocity sensitivity +224 to
+254, pitch LFO delay 2-6 repeated), `m10b` (portamento "2" modes at 17, 36, -17 and -24
semitones, `esp_seq`), `m10c` (Part velocity sensitivity depth and offset); no dropouts (gaps
over 64 samples) found; builder `scripts/probes10.py`.

*Saturator curve at high drive* (`6074120` generic, `86258b1` Oscairn). The reference's amp and
filter saturators are u / sqrt(1 + u^2) plus a square term, not tanh: fitted to the `m2` sine
probes (raw 8-80 and the level series) as a static curve, the worst harmonic 2-6 above -50 dB is
0.43 dB (amp) and 0.20 dB (filter), against 1.97 and 1.87 dB for the best tanh (curve alone);
knees u / (1 + |u|^n)^(1/n) with n = 1.5, 2.5-6 are worse (2.6-12.6 dB). New storage option
`saturatorAlgebraicCurve` (both saturators, scaled back by the same curve at the drive; off by
default; at first on in the plugin for every patch, now saved per patch: see below). Engine through the plugin: offsets -40.4 dB (amp,
asymmetry 0.115) and -42.1 dB (filter after LPF24 at 1004, asymmetry 0.254), worst harmonic
above -50 dB 0.50 dB (amp) and 0.30 dB (filter) over raw 8-80 and the level series (fifth
session with tanh: 3.5 and 3.1 dB at raw 80). Checked only against the renders it was fitted
to. The proposed 3 dB tolerance is now met at every drive. Still not modelled: the reference's
low-level odd harmonics (-56 dB third harmonic at osc level 64), which no static curve gives.
Level correction after each saturator (`satgain.py` with the new curve and offsets, reference
against engine fundamental): amp raw 8-80: +0.63, +0.89, +1.80, +1.24, +2.40, +3.25, +1.54,
-0.33 dB; filter: +1.19, +1.30, +0.91, +1.54, +2.23, +2.16, -0.56, -2.00 dB (sixth session's
tanh values: see there). Plugin check: raw 32-80 for both saturators, worst 0.43 dB (fails with
tanh, seen: filter raw 48 third harmonic 3.16 dB).

*Filter saturator offsets for the other types* (`f2741c3`, test only; `m10a`: a sine at note
60, LPFs at index 1023, HPFs at 0, BPF12 at 537, BPF6 at 532, raw 0, 48, 80). Every type fits the
same curve and asymmetry (0.252-0.253); the model offsets differ only by the reference filter's
gain at the note (LPFs identical to LPF24; HPFs 0.2-0.7 dB lower; BPFs 10 and 8 dB). Engine
offsets (the model offset moved by the engine filter's pass-band level at the note against
LPF24's, measured with the drive off): LPF18/12/6 -43.60 (the engine's LPF24 pass band is 1.5 dB
lower, hence its -42.1), HPF24 -42.52, HPF18 -38.46, HPF12 -40.49, HPF6 -38.96, BPF12 -41.71,
BPF6 -45.43 dB; harmonics above -50 dB within 0.22-0.32 dB of the reference at raw 48 and 80
(fitted to the same probes). The HPF and BPF offsets carry the engine filters' level at those
cutoffs (HPF18 at index 0, below its table's valid range, is 4.2 dB down at 262 Hz), so they
hold at those settings only; the general rule is one offset of -43.6 dB on the engine filter's
output at unity gain, with the filter level matched first. Plugin check: all nine types at raw
48 and 80, worst 0.31 dB; with -43.6 for every type 10 checks fail (seen).

*Noise tone* (`8ae52ad` generic, `eb2a41c` Oscairn test). The reference's tone is a first-order
shelf whose corner moves with the tone: (1 + g s/w) / (1 + s/w) fits every probed tone within
0.16-0.41 dB RMS over half-octave bands 62.5 Hz-16 kHz (worst band 1.11 dB, tone 0), measured
against the engine's own white noise (eighth session, with Surge's noise colour: 0.4-1.9 dB RMS
at 48-112 only, 3.4-6.2 dB RMS elsewhere). Tone 0 is nearly a lowpass at 192 Hz (tilt -42 dB),
64 white, 127 a +16.3 dB shelf at 7.9 kHz. New scene parameters Noise Tilt (dB, `ct_decibel`)
and Noise Tilt Frequency (`ct_freq_audible`): a bilinear one-pole at the oversampled rate with
the corner prewarped, after either noise type, skipped at 0 dB (the default); appended and reset
on load. Mapping (fitted at 0, 8, 16, 32, 48, 64, 80, 96, 112, 120, 127, linear between; 64
has tilt 0 and the corner of its neighbours; tone, frequency in Surge units, tilt dB, level
correction dB): 0 -14.34 -42.19 +7.46; 8 -7.24 -19.51 +5.84; 16 -2.11 -14.50 +5.29; 32 3.22
-8.14 +3.51; 48 5.37 -3.79 +2.02; 64 8.55 0 0; 80 11.73 2.54 -1.93; 96 26.13 4.48 -2.78; 112
38.44 7.74 -5.29; 120 44.39 10.49 -7.91; 127 50.10 16.28 -14.46. The level correction is the
reference's band-mean level change relative to tone 64 minus the engine's (it is positive at the
low tones, where the tilt cuts; Noise Volume cannot go above 0 dB, so the converter has to put
it elsewhere, for instance by lowering the other channels). With 120 left out of the knots, the
level at 120 was 1.5 dB off (linear interpolation from 112 to 127), hence the knot. Plugin
check: every 8th tone and 127, worst shape 0.71 dB RMS, level 0.40 dB; checks between knots at
24, 40, 56, 72, 88, 104 (0.33-0.60 dB RMS); no check between 112 and 127 other than the knots.
Without the tilt 14 of 17 fail (seen). Measurement repository: `half_octave_bands` (`b26fff1`).

*Portamento "Rate 2"/"Time 2"* (`5e0a27b` generic, `422f34c` Oscairn test; `m10b` with
`m8e`). Over an octave both "2" modes jump to an octave from the target and glide the last
octave, up and down (17, 24, 36 up, 17 and 24 down; a straight line through the glide's
20-80 % lands 11.77-11.96 semitones from the target at the moment the pitch leaves the first
note, slope 36.45-36.60 semitones/s, so an octave in 328 ms at time 64). New scene switch
Portamento Octave Limit: in `update_portamento` the glide's start is clamped to an octave (the
scale length) from the target, so constant time and constant rate both take an octave's time;
appended, off by default, reset on load. Test `[portamento]` (start and octave time in both
modes up and down, unchanged at 7 and 12 semitones, reset and round trip; fails without the
clamp, seen). Converter: Rate 2 = constant rate + octave limit, Time 2 = constant time + octave
limit, same time table. Plugin check (Time 2 only: constant rate is a flag on the portamento
parameter, not a plugin parameter): jumps within 0.18 semitone, slopes within 1.1 % (proposed
0.5 semitone and 5 %; 5 of 5 fail with the limit off, seen).

*Amp velocity sensitivity +255* (`134e4cf`, Oscairn). Implied k from the levels (a = 1 - k (1 -
v/127)): +192: 2.01; +224: 4.02, 4.00, 3.96; +240: 8.04, 8.01, 7.93; +248: 16.02, 15.89; +252:
31.77 (velocities 112-124). So k = 128 / (256 - S) above +192 (+254: k = 64, velocity 124
silent as predicted; +255: k = 128, only 127 sounds), joining 2^((S - 128)/64) at +192; between
+192 and +224 not probed. The plugin's Velocity > VCA Gain reaches -1536 dB (k = 128). The
plugin check keeps 0.3 dB up to +192; above, it checks k within 2 % (worst 0.79 %, i.e. 0.38 dB
at +252, velocity 124, which a 0.3 dB level check would fail: there the level moves 1.7 dB per
unit of k) and silence (at least 60 dB down) where the reference is silent. With the
exponential k above +192 it fails (seen). S < 0 stays as before (a velocity routing).

*Part velocity sensitivity depth and offset* (`m10c`, measured, not mapped in a check; amp
sensitivity +128 so the level gives the effective velocity v'). v' = v x depth / 64 + 2 x (offset
- 64), clamped to 0-127 (depth 127 acts as about 2.0: v' = 64.5 at v = 96, offset 0); every
probe within 1 of it. Below the clamps this is affine in v, so the converter can fold it into
each velocity sensitivity and the matching level; where it clamps (for example depth 96 or
offset 96: every velocity from 64 up plays as 127) Surge has no equivalent (not built).

*Pitch LFO delay at 2-6* (`m10a`, mapping only). Switch-on times (7 renders at 4, 2 at the
others; 1-sigma scatter 1.0-1.3 ms): 2: 19.1, 3: 29.3, 4: 48.4, 5: 58.0, 6: 78.5 ms (formula:
16.0, 29.4, 45.3, 63.3, 83.2). The reference steps unevenly here, so 4's -5.4 ms was the formula,
not the measurement. With these values as a lookup, the engine (voice LFO delay = log2 t) reads
-2.2, -0.9, -0.9, -2.9, -0.5 ms from the reference (all 5 within 2.5 % or 5 ms, 2 and 5 by
the 5 ms floor; the engine's delay itself steps: 16.9 ms whether set to 16 or 19 ms). Converter: these values at 2-6, the formula from 8 (7 not probed).

*Reference-matching options per patch* (`ec14676` and `00f6869` Oscairn, `95e6f97` generic). Until now the
plugin turned the analog attack clamp, the amplitude velocity law and (this session) the
algebraic saturators on for every patch, so a plain Surge patch did not play in the plugin as in
Surge. The three options are now saved in the patch (`nonparamconfig`, each written only when on,
reset to off before every load) and the plugin takes them from the patch (`ec14676` first
moves the plugin's forcing to every load, so the commit in between still passes). Tests: `[signalpath]`
"Engine Options Are Saved In The Patch" (off by default and not written; each round trips;
a patch without them turns them off; fails without the read, seen); plugin "plain patches match
stock Surge": every 8th factory patch (79) and one built to use all three options, loaded into
the plugin and into a stock `SurgeSynthesizer` (default options and ranges; the plugin's
one-scene, no-effects rules applied by hand), a note at velocity 70 rendered in both: identical
samples for all 80; forcing any one option on in the plugin fails it (clamp: 1 patch, velocity
law: 13, saturator: 1; seen); with the options on in a patch, the plugin's state keeps them and
round trips byte for byte (fails if the plugin drops one, seen). The reference checks now render
with the options on (`RenderSpec::referenceMatching`; the render tool's `--reference`; every
reference check passes as before). This is the plugin path, not `[reference]` itself: the
plugin plays one scene and no effects, so the 635 reference patches can't be bit-identical to
their stock renders through it; the comparison above is the plugin-path equivalent.
The plugin's longer ranges (envelopes and voice LFO attack/decay 2^8 s, portamento 2^3.99 s,
Velocity > VCA Gain down to -1536 dB) don't change what a stored value plays: the engine uses
`val_max`/`val_min` only for a parameter's limits, the normalisation of host modulation (which
the plugin undoes with the same range) and, for portamento, an assertion; the test above passes
with them. One exception, in Surge's patch loader and not hit by any factory patch: a patch of
revision below 2 (Surge 1.x) sets an LFO's sustain from whether its decay equals `val_max`, and
the plugin's voice LFO decay maximum is 8, not 5. Left as is (no such patch to test).

*Staying mergeable with upstream.* Upstream `main` fetched into `refs/remotes/upstream/main`
only (`0852de8`, 2026-10-31; nothing pushed, no remote added). The fork's base is `5a4828a`;
upstream has one commit since (an accessibility fix in the MSEG editor). A test merge
(`git merge-tree` of `upstream/main` into `oscairn` at `00f6869`) is clean: no conflicts. The
risk is in the future: outside `src/oscairn-clap/`, `doc/` and new test files, the fork changes
95 hunks in the four engine files upstream edits most (`SurgeSynthesizer.cpp` +329 lines,
`SurgePatch.cpp` +306, `SurgeVoice.cpp` +288, `SurgeStorage.h` +93; upstream touched them 26,
33, 5 and 38 times in the last year), plus `QuadFilterChain.cpp` (+185, untouched upstream in a
year) and smaller hooks in the LFO, ADSR, modulation-source and oscillator files. Likely
conflict points: the end of the scene's parameter block and the reset list in `load_xml`
(`SurgePatch.cpp`), the storage options next to other storage flags (`SurgeStorage.h`), and the
voice-start and block code in `SurgeVoice.cpp`/`SurgeSynthesizer.cpp`. `n_scene_params` and
`ff_revision` are lower risk than they look: upstream hasn't changed `n_scene_params` in two
years, and the fork doesn't change `ff_revision`. Ways to cut conflicts, none done this session:
move the larger self-contained blocks (key-on delay in `SurgeSynthesizer.cpp`, segment drift
and unison in `SurgeVoice.cpp`) into new files with one-line call sites; keep every new scene
parameter in one appended block (already so) and every new storage flag in one block (already
so). From here on new work keeps Oscairn code in `src/oscairn-clap/` and new files, with small,
off-by-default engine hooks.

*Long LFO releases* (`68f0e94` generic, `4fdcec4` Oscairn). The voice LFOs that serve as the
pitch, oscillator and modifier envelopes couldn't release longer than 2^5 s, because an LFO
release at its maximum means "never releases". New engine option `lfoLongRelease` (saved in the
patch like the other three, off when absent): the release reaches 2^8 s and only 8 means never.
Off, a release at or above 5 never releases, even where a host's range goes past 5, so Surge
patches play as before; the plugin's voice LFO release range is now 2^8 s. Tests:
`[lfoenvmode]` "LFO Long Release" (off: 5 never releases with a range of 5 or 8, 4.9 releases,
6 never; on: 5 and 7.9 release, 8 never; comparing against the parameter's maximum instead
fails it, seen); the plugin's stock comparison now includes an LFO envelope released at 5, and
forcing the option on fails it on that patch and on factory patches (Deep End, E-Bass, Aliens:
Surge patches do use 5 as "never"). Converter: set `lfoLongRelease` with the other three for a
converted Part. Not done: a plugin check of releases above 30 s against reference renders (the
oscillator EG release table stops at index 208).

*Part Volume and Performance Volume* (`m10d`: a sine at note 60, everything else as the other
probes; `pv_default` had a dropout and is not used: `pv_part_100` is the same setting). Level
relative to Part Volume 100: 0 silent, 16 -31.90, 32 -19.88, 64 -7.87, 112 +1.88, 127 +4.13 dB,
that is 40 log10(v / 127) within 0.1 dB (an amplitude of (v / 127)^2). Performance Volume,
relative to 127: 0 silent, 32 -24.01, 64 -12.00, 100 -4.50, 112 -2.62 dB, 40 log10(v / 127)
within 0.45 dB (largest at 100 and 112). The probes ran at Part Volume 100 and Performance
Volume 127 (Dry Level 127), so every absolute level fitted from them (the amp level table and
its checks; the oscillator level table is checked at the amp table's value, so it moves with
it) includes the Part Volume's -4.15 dB (-4.13 measured), and the Performance Volume's 0 dB.
For engine-only tables, `ampLevel` and the references of its and `osc1Level`'s checks need
+4.13 dB, and the converter maps Part Volume and Performance Volume to Surge's scene and
global volume with the curves above. **Not done**: the tables were not re-exported (the export
runs from the measurement repository with every table's fit and check files, which this
session did not rebuild); for now the converter can add +4.13 dB to `ampLevel` itself.

*Ageing* (`m10d` with the sixth session's `m3a`/`m3e`; Ageing -100, 0, +100, two loads each).
Measured, not modelled: no effect on pitch (sixth session), amp decay times (0.431/0.836 s at
-100, 0.428/0.833 at 0, 0.428/0.831 at +100: within the repeat scatter), or level (within 0.2
dB); a fixed cutoff shift (resonant LPF24 at 512: harmonics 5-9 +0.3 dB at -100, -0.1 dB at
+100, about +0.16 and -0.05 semitones, as in the sixth session) and a fixed pulse width shift
(duty 0.2517, 0.2538, 0.2545 at -100, 0, +100); both loads identical, so no spread between
loads or notes. The converter can fold both shifts into the cutoff and the pulse width; no
engine hook.

*Sample rates* (`2cf5d30`, test only). The reference was rendered and every table checked at
44.1 kHz; the plugin mostly runs at 48 kHz. The standard plugin test build now also runs the
saturator checks and every fitted table except the cutoff tables at 48 and 96 kHz with the same
tolerances: amp level, oscillator level, amp attack/decay/release, pitch and oscillator EG
times, LFO speed, key-on delay and portamento all pass at both rates, and so do the saturators
(worst 0.43 dB, filter types 0.31/0.34 dB). The cutoff tables do not: at 48 kHz 18 checks fail,
all from index 944 up (up to 1400 cents at the top of LPF18/LPF12); at 96 kHz 93 fail, from
index 720-912 depending on the type and LPF6 from 400 (up to 1230 cents). Surge's filters respond
differently near the top of the range at another rate, so the tables, fitted to engine renders
at 44.1 kHz, are rate-specific there; how the reference behaves at other rates is unknown (the
recorder renders at 44.1 kHz). Not fixed: it needs either cutoff tables per rate or a
correction for the engine's rate dependence, and reference behaviour at other rates is a
question for Jacob (does the instrument run internally at a fixed rate?). Verified only at
44.1 kHz: the cutoff tables (above), and the plugin checks outside the fitted tables (unison,
drift, cutoff drift, pitch LFO phases, velocity, noise tone, portamento octave limit, first
block after a load). The sanitizer builds run the extra rates' checks not at all (they check
memory and threads; the TSan run would grow by most of an hour).

*Asked by Jacob, answered without a change.* Surge XT's own envelope slider keeps its range
(decision A2): Oscairn's editor uses the plugin's parameter ranges, which reach 2^8 s, and
widening Surge XT's GUI would add to the diff against upstream. Still open: the floor for
averaged short-envelope checks (question C), and Jacob's confirmation of the tolerance table.

*Wave folder above 96 and Hard* (time-boxed, no model). Static curves fitted per probe to
harmonics 2-12 (sine folder, triangle folder, rounded triangle tanh(b tri(g x)), |sin|^b):
best 0.9-2.1 dB RMS up to 96 (Soft), 3.7-5.4 dB above and 1.1-4.0 dB for Hard, with the best
family changing from one amount to the next; nothing committed. Resonance above 96 (a new
filter), self sync and the wave shaper were not attempted.

**Tests run in the tenth session.** GitHub Actions stayed disabled (no run since `cac0d8e`;
checked after the first push).

*A gate fault, found late.* The macOS gates ran on an exact export of each commit, built in
separate directories (`surge-gate/`, outside the repository) so the working tree could change
meanwhile. The export kept each file's commit time, older than the objects already built for
the previous commit, so ninja rebuilt nothing after the first commit: the macOS runs for
`6074120`, `86258b1`, `eb2a41c`, `5e0a27b`, `422f34c` and `134e4cf` tested `8ae52ad`'s
binaries (the testrunner reported 207 test cases throughout, where `5e0a27b` adds one, and the
plugin tests printed the old velocity line). Those six were pushed on those results and on the
Docker Linux jobs, which build from scratch and are valid. Only `8ae52ad`'s macOS run
(fresh build directories: testrunner 207 test cases passed, `[reference]` 635 compared, 6
skipped; plugin tests standard 85 s, ASan/UBSan 104 s, TSan 549 s, 0 sanitizer reports) counts.
The script now writes changed files with the current time, and the build directories were
cleaned before the runs below. The fault hid two test problems, both fixed before the second
push: the velocity check read a level before the start of a silent render (UBSan stops on it in
the macOS ASan build; GCC's UBSan in Docker doesn't check float-to-integer casts), fixed in
`d84c6d6`; and the key-on delay test (ninth session) failed in about 1 run in 10, depending on
the oscillator's random start phase, fixed in `454fc25`. A first ordering of the per-patch
commits also failed the Docker standard job (37 failures at the generic commit: the plugin
still set the options at init and the new load reset them); never pushed, reordered with
`ec14676` in front.

macOS arm64 gates (cleaned build directories, fixed script): `surge-testrunner` passed with
`[reference]` "Compared 635 patches, skipped 6" and `oscairn-analog-tests` standard, ASan/UBSan
and TSan exit 0 with 0 sanitizer reports on `d84c6d6` (208 test cases; plugin tests 83, 107,
540 s), `454fc25` (208; 81, 108, 547 s), `f2741c3` (208; 80, 105, 535 s), `ec14676` (208; 83,
106, 537 s), `95e6f97` (209; 79, 106, 537 s), `00f6869` (209; 82, 114, 597 s). Third push: `68f0e94` (210 test
cases; plugin tests 99, 247, 722 s), `4fdcec4` (210; 114, 136, 630 s), `2cf5d30` (test only,
testrunner skipped; 141 s with the 48 and 96 kHz checks, ASan 127 s, TSan 634 s), all passed
with 0 sanitizer reports. Re-run after the push on the six
pushed commits: (testrunner and `[reference]` 635/6 passed on each, the test
counts now right: 207, 207, 207, 208, 208, 208): `6074120`, `86258b1`, `eb2a41c`, `5e0a27b`
and `422f34c` passed all three plugin builds with 0 sanitizer reports (standard 76-81 s, ASan
96-99 s, TSan 494-510 s); `134e4cf` fails the macOS ASan/UBSan run (the velocity check's
silent-render read, fixed in `d84c6d6`; standard and TSan passed). So the first push contains
one commit whose macOS ASan gate fails.

Docker Linux (local substitute for CI, as in the eighth session; each job exit 0, "all
passed", no sanitizer reports): standard on `8ae52ad` (114 s of tests), `6074120` (98 s),
`86258b1` (91 s), `eb2a41c` (93 s), `5e0a27b` (95 s), `422f34c` (95 s), `134e4cf` (95 s);
ASan/UBSan (3173 s) and TSan (499 s) on `134e4cf`, the head of the first push, which holds the
engine changes of `8ae52ad`, `6074120` and `5e0a27b`; standard on `d84c6d6`, `454fc25`, `f2741c3`, `ec14676`, `95e6f97` (87 s of tests each) and
`00f6869` (88 s), ASan/UBSan (3350 s) and TSan (517 s) on `00f6869`, the head of the second
push; standard on `68f0e94` (92 s), `4fdcec4` (99 s) and `2cf5d30` (165 s, with the 48 and 96
kHz checks), ASan/UBSan and TSan on `4fdcec4` (3760 s, 798 s) and on `2cf5d30`, the head of the
third push (3730 s, 688 s). One Docker ASan build
was killed for memory (4 compile jobs while other work ran) and rerun with 3. This record
(`doc/` only) was not run separately. Measurement repository (`b26fff1`: `half_octave_bands`):
self-tests 26 passed.

**Eleventh session (2026-11-01): cutoff tables at 48 and 96 kHz, amp level without Part Volume.**

GitHub Actions stayed disabled; the Linux jobs ran in the local Docker substitute (see "Tests
run in the eleventh session"), which is not CI. No new reference batches (the recorder was not
used).

*The macOS gate script, checked first.* Before relying on it, the fixed script (changed files
written with the current time) was shown to rebuild: a build-only run on `2bd3779`, then the
full run on `ee45bc7`, whose only change is `FittedTables.h`: every test binary's hash changed
(standard `81ec6d2c` -> `fbcc9dea`, ASan `86db3400` -> `ae746070`, TSan `553604b8` ->
`8e574fb1`). The script now prints the binaries' hashes after every build, and builds the new
fitting tool (below) where the tree has it.

*Cutoff tables at 48 and 96 kHz* (`9f0a594` generic, `ec3de57` and `b353b7e` Oscairn). Why
they failed: Surge's voice filters run at twice the host rate and compute their coefficients
from the cutoff over that rate, and several clamp the cutoff at a fraction of it (the legacy
ladder at 0.187 x, i.e. 16.5 kHz at 44.1 kHz, the table's plateau at 62.75; the state-variable
filters at 0.22 x, 19.4 kHz; the OB-Xd at 22 kHz at any rate). So near the top the same cutoff
responds differently at another rate: above a clamp it keeps opening at 48 kHz where it stops
at 44.1 kHz, and the 6 dB slopes' high-frequency droop differs (LPF6 needed +0.5 semitone from
index 400 at 96 kHz). Nothing in the tables is per sample; they map to the engine's response at
44.1 kHz, which is what was matched to the reference. New storage option
`filterCutoffRateMatch` (saved in the patch like the other four; off when absent; reset on
load): at 48 and 96 kHz, for the ten filter configurations the cutoff tables use, the voice
moves the filter's final cutoff (after key tracking, envelope and drift) to the one whose
response best matches the 44.1 kHz response at the patch's cutoff (`dsp/FilterCutoffRateMatch.h`,
piecewise linear between knots every semitone from -40 to 75 and at each clamp, non-decreasing,
constant above the clamp; other types, rates and the option off: unchanged). The knots were
fitted engine against engine, not against the reference: a saw at note 36 through the plugin,
gains at the harmonics up to 16 kHz within 40 dB of the largest, shape error with the mean
removed, a quarter-semitone scan refined by a parabola (`oscairn-analog-rate-match`, which also
writes `FilterCutoffRateMatchTables.h`). Shape error left after matching (and with the cutoff
unmatched), worst over the grid, dB: 48 kHz LPF24 0.05 (0.25), LPF18 0.04 (0.27), LPF12 0.04
(0.18), LPF6 0.02 (0.09), HPF24 0.36 (0.62), HPF18 0.20 (0.20), HPF12 0.14 (0.14), HPF6 0.07
(0.07), BPF12 0.04 (0.34), BPF6 0.02 (0.17); 96 kHz LPF24 0.28 (1.43), LPF18 0.22 (1.56), LPF12
0.18 (1.04), LPF6 0.11 (0.52), HPF24 3.40 (5.28), HPF18 0.93, HPF12 0.66, HPF6 0.34 (the OB-Xd
types only above 60, their 22 kHz clamp, where no cutoff matches), BPF12 0.35 (1.95), BPF6 0.19
(1.01). The plugin's cutoff checks against the reference (unchanged code, 301 checks per rate),
like for like on the same build: without the rate match 0 / 18 / 93 failures at 44.1 / 48 /
96 kHz; with it 0 / 1 / 5, all HPF24 at the top: 48 kHz index 976 (-109 cents); 96 kHz 938 (+140),
942 (+454), 946 (+251), 956 and 976 (no minimum: the shape error falls monotonically). HPF24 is
the state-variable highpass at resonance 0.05, whose damping cap (2 - 1.52 F1) gives the 44.1 kHz
filter a resonance at the top that a higher rate doesn't, so no cutoff reproduces it (at 96 kHz,
at indices 956 and 976, the engine's best match to the reference itself is closer, 0.1-0.3 dB
against the 44.1 kHz engine's 1.6-2.5 dB, but far from the table). Tried and dropped: matching the state-variable filters'
damping at the higher rate through the resonance (the same Q1 at the matched cutoff), which made
the 96 kHz HPF24 shape error worse (2.1/3.5/5.2 dB at cutoffs 55/58/61 against 1.9/2.8/3.3).
The plugin test now runs every cutoff table at 48 and 96 kHz in the standard build, except HPF24
above index 937 at those two rates, whose best match it prints instead of checking (a new
exclusion, listed here; at 44.1 kHz every HPF24 check stays). Tests: `[signalpath]` "Filter
Cutoff Rate Match" (unchanged at 44.1 kHz and for types without a table, non-decreasing for
every table, one value above the ladder's clamp; in the voice at 48 kHz the ladder at cutoffs 64
and 70 differs with the option off and is identical with it on, at 44.1 kHz on and off are
identical; fails with the voice hook disabled, seen) and "Engine Options Are Saved In The Patch"
with the fifth option (fails without its load, seen). Plugin: the reference renders turn the
option on (`RenderSpec::referenceMatching`), the state test keeps it, and the stock comparison
still passes with it read from the patch; forced on for every patch it fails on 8 of 80 factory
patches at 48 kHz (seen). Not covered: rates other than 44.1, 48 and 96 kHz (88.2 and 192 kHz
are left unchanged; the tool can fit them); resonance other than each type's fitted one (the
same cutoff move is used); filter 2 is moved by the same rule but not checked separately.

*Amp level table without Part Volume* (`ee45bc7`, test only; measurement repository
`639f520`). The fit and check files behind `FittedTables.h` were found in the local fit
directory and the export rebuilt from them byte for byte first (LPF6 from `g_type4`'s table,
results and check manifest). `fit.py`'s level kind takes a new option `reference_offset_db`,
added to the reference side only (self-test; it fails without the option, seen). The amp level
was refitted from the same reference and engine sweeps with +4.13 dB (Part Volume 127 against
the probes' 100, measured in the tenth session) and checked by rendering the engine at the new
table values: 57 of 57 within 0.1 dB (worst 0.054 dB; before, 0.040 dB). Old/new: the table
moves by +4.127 to +4.130 dB over 56-511 (index 431, the init amp level: -7.996 -> -3.866 dB;
511: -5.034 -> -0.904 dB; 56: -43.433 -> -39.306 dB); every check reference moves by +4.13 dB
(index 60: -62.13 -> -58.00 dB). The oscillator level table is a ratio against the amp level
(fitted with the amp level curve as its base and checked at the amp table's index 431), so it
holds as it is: re-checked at the new amp level against the reference + 4.13 dB, 14 of 14
within 0.1 dB (worst 0.022 dB); only its check references change (index 80: -60.10 ->
-55.97 dB). Neither saturator check changes: both saturators sit before the amp gain. The
plugin tests pass at 44.1, 48 and 96 kHz with the new tables. **Converter**: with `ee45bc7`
(pushed), the converter must stop adding +4.13 dB to the amp level, and maps Part Volume and
Performance Volume to Surge's scene and global volume as 40 log10(v / 127) (0 dB at 127).
As before, the level is fitted from every 8th index and checked at the indices in between.

*Wave folder above 96 and Hard* (time-boxed, measured, no model). From the `m2` sine probes
(note 48): how memoryless the reference is, as the mismatch between the output at input phase
theta and pi - theta (equal for any static curve), relative to the signal: Soft 32-96 -29 to
-20 dB, 128-255 -13 dB; Hard 32 and 96 -21 and -27 dB, 128-255 -12 to -13 dB. The harmonic phases
above 96 depart from a static curve's by 6-7 degrees RMS, which a one-pole lowpass or first-order
allpass after the curve doesn't explain (best 6.2-6.5 degrees). Energy between the harmonics (the
period fit's residual): Soft 64 -63 dB, 96 -56, 128 -54, 192 -48, 255 -44 dB (peaks near 18-22 kHz,
aliasing); Hard 96 -44, 128 -39, 160 -34, 192 -23, 224 -11, 255 -4 dB (no dropouts): at Hard 224
and 255 the folder's aliased products are about as loud as the harmonics. So the reference folder
is not band-limited at high amounts (the engine's folder runs oversampled and won't alias like
it), and above 96 it isn't a static curve either; any fitted static curve is limited to roughly
the 3.7-5.4 dB already found, and Hard above 192 can't be matched on harmonics alone. Nothing
committed. Resonance above 96, self sync and the wave shaper were not attempted.

*Decisions for Jacob, as asked first.* (1) HPF24 above index 937 at 48 and 96 kHz; (2) rates
other than 44.1, 48 and 96 kHz; (3) whether the reference's 44.1 kHz behaviour is the target at
every rate. Answered for (2) and (3): the 44.1 kHz behaviour is the target at every rate, and
the rate match should cover every rate (below). (1) is still open (below and the register).

*Known differences register* (section 11, new; Jacob's request): one entry per known difference,
kept up to date.

*Note shift and pitch bend ranges* (`f633f96`, Oscairn; Jacob's decision: no artificial limit on
note shift or bend range). The reference's Part Note Shift and Performance Note Shift stop at
+-48 semitones each and its bend ranges at -48 to +24 (Upper and Lower; a negative Upper bends
down); the plugin exposed Surge's scene pitch (+-7 semitones) and bend ranges (0-24), though the
engine plays any value. The plugin's scene pitch now reaches +-127 semitones and the bend ranges
127 (up) and -127 to +127 (down; negative inverts the bend), set again after every load because
loading a patch resets Surge's bend ranges; ranges in cents (extended) keep Surge's. Documented
values map one to one (note shift n -> scene pitch n; Upper u -> bend up u; Lower l -> bend down
-l). A negative bend up range can't be stored: Surge's patch loader reads a bend up range outside
0-255 as a legacy float value and resets both bend ranges, so the reference's negative Upper
values have no equivalent (register). Stock Surge is unchanged (only the plugin's ranges; the
stock comparison still passes, 80 of 80). Plugin tests: the ranges (fail without the widening,
seen) and the extremes rendered: note shift +48 and -48 (the reference's largest), +127 from note
0 and -127 from note 127, bend up 127 from note 0, bend down 48 from note 108 and -127 from note 0,
each within 1 cent plus 0.05 % of the shift (worst 3.41 cents, at the 127-semitone bends: Surge's
smoothed bend settles 0.027 % short of its target). Not covered: note shift beyond the engine's
note range (Surge's pitch tables stop at note 255).

*Cutoff rate match at other rates* (`e3fe060` generic, `3a25680` Oscairn). The tool now fits
192 kHz too (shape error left after matching, worst over the grid: LPF24 0.41, LPF18 0.26, LPF12 0.22, LPF6 0.13, HPF24 3.51, HPF18 1.10, HPF12 0.78, HPF6
0.40 (the OB-Xd types 0.01 up to cutoff 60), BPF12 0.47, BPF6 0.25 dB); between
fitted rates (and between 44.1 kHz, unchanged, and 48 kHz) the matched cutoff is interpolated
linearly in log rate, above 192 kHz the 192 kHz table is used, below 44.1 kHz the cutoff is
unchanged. Plugin tests now run every fitted table and the saturator checks at 48, 88.2, 96,
176.4 and 192 kHz (88.2 and 176.4 interpolated): all pass but HPF24 above index 937 (printed,
register entry 1). At 64 kHz, tried and left out of the test, LPF24 fails at 948 (+58 cents) and
956 (+130) (interpolation too coarse near the state-variable clamp) and the amp EG decay at
index 116 is 4.6 % long (register entries 2 and 18). Testrunner: "Filter Cutoff Rate Match" also
checks the interpolation (88.2 kHz from 48 and 96, 46 kHz from 44.1 and 48, 384 kHz as 192, 32
kHz unchanged; fails without the interpolation, seen) and monotony at 64, 88.2 and 176.4 kHz.

*HPF24 at the top, tried again* (nothing committed). A search over resonance (0-0.6) and cutoff
at 96 kHz finds its best at resonance 0, 1.4 / 2.5 / 4.0 dB at cutoffs 55 / 58 / 61 (1.9 / 3.1 /
4.1 dB at 0.05), so the state-variable highpass can't reproduce its 44.1 kHz top at a higher
rate. HP 24 dB Driven (type 5, subtype 1) against the reference's HPF24 responses at 44.1 kHz
(best shape error per check): from index 880 0.08-0.28 dB (the state-variable filter 0.32-2.52
dB), at 48 and 96 kHz nearly the same, but below 848 0.17-0.51 dB (0.04-0.46). Refitted as a
table (sweep `surge/cut5d`, `gate.py type5d`, local): 22 of 23 checks pass, index 464 fails by
-111 cents. Not switched; a decision for Jacob (register entry 1).

**Tests run in the eleventh session.** GitHub Actions stayed disabled.

macOS arm64 gates (exported trees, build directories reused from the tenth session after the
rebuild proof above, then deleted at the end of the session): `ee45bc7` (test only, testrunner
skipped): plugin tests standard 133 s, ASan/UBSan 151 s, TSan 871 s; `9f0a594`:
`surge-testrunner` 211 test cases passed, `[reference]` "Compared 635 patches, skipped 6" (the
gate's log lacked that line this once; run again by hand on the same binary), plugin tests 155,
131, 683 s; `ec3de57`: testrunner 211 passed, `[reference]` 635/6, plugin tests 147, 175, 792 s
(the gate also built `oscairn-analog-rate-match`); `b353b7e`: testrunner 211 passed,
`[reference]` 635/6, plugin tests 375 s (with every cutoff table at three rates), 139, 772 s.
Every plugin run exit 0 with 0 sanitizer reports.

Docker Linux (local substitute for CI; exit 0, "all passed", no sanitizer reports): standard on
`ee45bc7` (177 s of tests), `9f0a594` (141 s), `ec3de57` (132 s), `b353b7e` (335 s);
ASan/UBSan (3996 s of tests) and TSan (554 s) on `b353b7e`, the head of the push, which holds
the engine change of `9f0a594`. Measurement repository (`639f520`): self-tests 27 passed.

Later in the session (a second push): macOS gates on fresh build directories (the first set was
deleted after the first push): `f633f96`: testrunner 211 passed, `[reference]` 635/6, plugin
tests standard 781 s, ASan/UBSan 178 s, TSan 966 s; `e3fe060`: testrunner 211 passed,
`[reference]` 635/6, plugin tests 277, 120, 621 s; `3a25680`: testrunner 211 passed,
`[reference]` 635/6, plugin tests 841 s (every fitted table at six rates), 131 s, 643 s; all
exit 0 with 0 sanitizer reports. Docker Linux standard on `f633f96` (374 s of tests), `e3fe060`
(314 s), `3a25680` (956 s); ASan/UBSan (3623 s of tests) and TSan (651 s) on `3a25680`, the head of the
second push, which holds the engine change of `e3fe060`. These records (`doc/` only) were not run separately; the
gate area was deleted at the end.

**Twelfth session (2026-11-02): HPF24 with HP 24 dB Driven tried and not applied; decisions.**

GitHub Actions stayed disabled. No commits to the engine, the plugin or the tests this session
(this record only); no new reference batches (the recorder was not used).

*Decisions by Jacob, recorded.* (1) Switch HPF24 to HP 24 dB Driven (type 5, subtype 1,
resonance 0.05), refit its cutoff table and rate match, and gate HPF24 above index 937 at every
rate: tried, below; **not applied**, because the Driven filter can't pass at 176.4 and 192 kHz
(a decision for Jacob again). (2) 64 kHz needn't be covered: decided by Jacob; it stays out of
the tests (register entries 2 and 18). (3) The tolerance table is accepted (no longer proposed):
amp level 0.1 dB, LFO speed 2 %, envelope times 2.5 % or 5 ms, cutoff 50 cents, aliasing floor
-70 dB, saturator harmonics 3 dB, unison offsets 0.1 cent and channel gains 0.02, drift start
values KS at 5 % plus 2 %, pitch LFO fade 2 % of full, key-on delay 2.5 % or 15 ms, portamento
5 % or 5 ms, velocity 0.3 dB, cutoff drift 20 %, pitch LFO phases 6 cents RMS. (4) Averaged
short-envelope checks use a 2 ms floor (not 5 ms; question C answered): short-envelope tables
are to be committed only where they pass with it, the rest listed in the register (entry 13;
not done this session). (5) The plugin's version is bumped whenever its sound or memory figures
change (Oscairn keys measurements by version and build); not applied yet, as nothing changed.

*HPF24 with HP 24 dB Driven.* Cutoff table (measurement side, local `gate.py type5d2`): the
engine sweep extended from -30 down to -45 (index 384 needs -32.8, 352 -40.9, so the trial
refit started at 416 and held -28.75 below it); the trial's failure at index 464 (-111 cents) is
a step in the shape measure, not noise: the fundamental falls out of the 40 dB range the shape
error uses between 464 (-36.7 dB under the largest harmonic) and 480 (-41.4 dB), and with it in
the Driven filter's best match sits about 1.2 semitones higher (its fundamental is 2.6 dB off
the reference's). A break at 475 (interpolated from the fitted renders 448 and 480) with the
base-plus-difference interpolation applied on each side, then the closest non-decreasing curve
(it otherwise fell 2.3 semitones at 475; it now holds -19.25 from 466 to 483 and misses the
fitted 480 by 48 cents), passes 25 of 25 checks (368-976; worst 560 at -42 cents) in the
measurement repository's check. Both `fit.py` changes have self-tests that fail without them;
not committed, since the switch wasn't applied (kept as a patch). In the plugin's own check at
44.1 kHz (same build, best match from the table in cents / shape error at the table, dB), state-variable filter
then Driven: 400 0/0.04, 7/0.18; 464 +20/0.09, -13/0.32; 560 +7/0.11, +44/0.42; 720 0/0.19,
-3/0.49; 848 +10/0.58, -4/0.27; 912 +2/0.32, +7/0.19; 942 +18/0.55, +3/0.11; 956 -18/1.62, 0/0.07;
976 -25/2.52, +2/0.07 (below index 848 worse: 0.18-0.51 dB against 0.04-0.46; the top better).

Filter saturator offset for the Driven filter (the rule of the tenth session: the offset moved
by the engine filter's pass-band level at the note): at its index 0 (-43.75) the Driven filter is
1.42 dB louder than the state-variable one at its index 0, so -42.52 -> -43.94 dB; harmonics at
raw 48 / 80 within 0.8 dB of the reference (third harmonic at raw 48: -34.4 against -35.2; the
Driven filter alone makes a -53.6 dB third with the drive off).

Rate match refitted for the Driven filter (`oscairn-analog-rate-match`, clamp 75, its frequency
bound). Engine against engine, shape error after matching (unmatched), dB: 48 kHz at most 0.03
up to cutoff 70 (0.27 at 75); 96 kHz 0.44-0.49 at 40-50 (0.45-0.66), 0.06-0.24 at 55-60; 192 kHz
1.1-2.0 from cutoff 40 to 65 (2.6-3.5). The plugin's check with the rate match, failures (over
50 cents) per rate: 44.1 none; 48 none; 88.2 560 (+57), 880 (+56), 912 (+54), 942 (+52); 96 560
(+56); 176.4 496 (+63), 560 (+80), 592 (+59), 752 (+63), 784 (+58), 816 (+360), 848 (+148), 880
(no minimum), 912-976 (+140 to +680); 192 similar (496 +339, 938 +1794, 956 +8463). Before
(state-variable filter, unchanged tables), only 938-976 failed above 48 kHz (register entry 1).
Cause: the Driven filter saturates inside its loop, so its response depends on the level and,
through the oversampled rate, on the sample rate: a saw at note 36 through it at cutoff 30
differs in shape between 44.1 and 192 kHz by 1.44 dB at the probes' level and 0.02 dB at 6 dB
lower; at 44.1 kHz the 6 dB lower level itself changes its shape by 0.20 dB. No cutoff moves
that back. Its best match to the reference at 192 kHz, at any cutoff, is 0.87-1.47 dB from index
880 (0.08-0.28 at 44.1). For comparison, HP 24 dB Clean (subtype 2, resonance 0.05, linear)
against the reference: 0.13-0.32 dB from index 880 at 44.1 kHz and the same at 192 kHz
(0.13-0.31), 0.19-0.68 dB below. Driven at resonance 0.15: 0.11-0.25 dB below index 784 but
0.14-0.51 from 880; 0.3: 0.06-0.18 below 592, 0.63-1.23 from 880.

*Silent converted AN-X Parts* (SQR Harmony "Harm", Pure Analog KIT "TOMS", Syn Bass + Sparkle
"Sparkle"; also KIT "KICK"). Not a plugin cause, and not the filter 2 cutoffs: the converter
writes Modern's third mix control (`a_oscN_param2`, triangle/square/sine) without a
`deactivated` attribute (`perfconv/anx.py`, the `param2` line, while `param0` and `param1` get
`deactivated=0`). Surge's patch loader, as in stock Surge, sets a deactivatable control with no
attribute to deactivated, so the triangle, square and sine waves (waves 3 and 4) and Saw2's sine
part (wave 1) are off. Through the plugin, TOMS and KICK render silent at notes 24-84 and Harm
and Sparkle at note 60 (the notes tried);
with `deactivated="0"` added to the three `param2` tags it plays (-26 to -36 dB RMS); minimal
cause found by reducing the differences from the plugin's default patch to one control.
**Converter**: write `deactivated=0` on `param2` too; every converted Part with Saw2 loses its
sine component the same way (from the code; not rendered). The converter must also map HPF24 to whatever filter is chosen
here (still type 5, subtype 0 with the existing table until Jacob decides).

*Not started this session:* the short-envelope tables at the 2 ms floor, the plugin version bump
rule (nothing changed), and CLAP preset discovery and `prepare_preset` for the plugin.

**Tests run in the twelfth session.** No code commit, so no gates. The measurement repository's
self-tests passed with the uncommitted `fit.py` changes (29 tests) and each new test failed
without its change (seen). The plugin checks quoted above ran on scratch builds of the working
tree (not committed).

**Mono and unison fixes (2026-11-02, after the twelfth session).** Two engine bugs with Voice
Unison above 1, in separate commits. (1) `84cac66`: on a key
release in Mono, Mono Fingered and Latch, `releaseNotePostHoldCheck` let the first voice on the key
set `do_switch`, so the group's other voices skipped the search for the held key and kept the
placeholder key 60; the last one played key 60, a stuck note at the wrong pitch. `do_switch` is now
reset for each voice. Test: "Unison Mono Returns To The Held Key" (mono, unison 4: 48, 55, release
55 returns all four voices to 48, release 48 leaves none gated; failed without the change, voices on
60). (2) `62bd6a2`: a new voice glides from `storage.last_key` and then sets it, so only the first
voice of a unison group glided (Poly, Mono restart, a new voice in Mono Single Trigger). Each
voice of the group now gets the prior last key and channel. Test: "Every Unison Voice Glides" (poly,
mono, mono single trigger with portamento: all four voices glide; without the change three of
four started on the new key in each mode). The plugin version is 0.1.1 (`0165817`, sound change).
The converter still writes 0.1.0 for the plugin's nodes, so the library packs the 0.1.1 binary as
`org.oscairn.analog@0.1.0` (converter not changed here).

**Tests run for these commits.** macOS arm64, each tree built from a fresh directory (exported with
submodules): `surge-testrunner` 211 passed, 1 skipped (`84cac66`) and 212 passed, 1 skipped
(`62bd6a2`, `0165817`); `[reference]` "Compared 635 patches, skipped 6" on all three;
`oscairn-analog-tests` all passed on all three, and on `0165817` in ASan/UBSan (130 s) and TSan
(675 s) builds with no reports. Linux in Docker (local substitute) on `0165817`: standard (1071 s of tests), ASan/UBSan (3794 s) and
TSan (459 s) all passed, exit 0, no sanitizer reports.

**Thirteenth session (2026-11-05): HPF24 with HP 24 dB Clean tried and not applied; the decay
table from index 72; CLAP presets; self sync and the wave shaper measured.**

GitHub Actions stayed disabled (CI on oscairn/surge is down); Linux checks ran in the local
Docker substitute. No new reference batches (the recorder was not used). The twelfth session's
Driven tooling and diffs were no longer in the scratch area; the tooling below was rebuilt.

*HPF24 with HP 24 dB Clean* (Jacob's decision: try type 5, subtype 2; nothing committed). Clean's
resonance does nothing below about 0.65 (its damping is capped at Q 0.5), so resonance 0.05 was
kept; fits at 0, 0.1 and 0.15 gave the same table within 0.3 cents. Cutoff table: engine sweep
`surge/cut5c` (note 36, -48 to 68 semitones in quarter steps; silent above 68), fitted like the
state-variable table (LPF24 shape plus a smooth difference) from -47 dB, with the twelfth
session's break at index 475 (where the reference's fundamental leaves the shape measure's 40 dB
range) and the closest non-decreasing curve (two new `fit.py` options, kept as a patch, not
committed since the switch wasn't applied): 24 of 25 measurement-side checks pass, 560 fails
(-57 cents; without the break 464 fails too, -135). Saturator offset by the tenth session's rule:
at its index 0 (-46.06) Clean passes a sine at note 60 1.50 dB louder than the state-variable
filter at its index 0, so -42.52 -> -44.02 dB; every filter saturator check passes with it at
every rate. Rate match refitted for Clean (`oscairn-analog-rate-match`, clamp 75, its frequency
bound): it is linear, so engine against engine it matches to within 0.015 dB up to cutoff 67 at 48
and 96 kHz and 0.10 dB at 192 kHz (unmatched 0.03-1.35 dB). The plugin's checks with the print-only
exclusion removed (every index gated at every rate), best match from the table in cents,
state-variable filter (as committed) / Clean; 938 and up at rates above 48 kHz were print-only
before:

| Index | 44.1 kHz | 48 kHz | 88.2 kHz | 96 kHz | 176.4 kHz | 192 kHz |
|---|---|---|---|---|---|---|
| 368 | - / +15 | - / +18 | - / +25 | - / +23 | - / -25 | - / -25 |
| 400 | +0 / +6 | +0 / +7 | +0 / +5 | +0 / +5 | +0 / +26 | +0 / +25 |
| 432 | +0 / +3 | +0 / +2 | +0 / +10 | +0 / +6 | +0 / +0 | +0 / +1 |
| 464 | +20 / -14 | +20 / -12 | +21 / -11 | +20 / -13 | +21 / -20 | +21 / -9 |
| 496 | +2 / +8 | +2 / +10 | +2 / +19 | +2 / +22 | +3 / +33 | +3 / +42 |
| 528 | +1 / +16 | +1 / +18 | +1 / +18 | +1 / +18 | +1 / +20 | +1 / +22 |
| 560 | +7 / +61 | +7 / +64 | +7 / +67 | +7 / +64 | +7 / +95 | +7 / +74 |
| 592 | +0 / -12 | +0 / -10 | +0 / -8 | +0 / -8 | +1 / -3 | +0 / -2 |
| 624 | -5 / -34 | -5 / -32 | -4 / -31 | -4 / -31 | -3 / -28 | -4 / -28 |
| 656 | +1 / +23 | +1 / +25 | +2 / +25 | +1 / +25 | +2 / +25 | +1 / +25 |
| 688 | +2 / +25 | +2 / +26 | +3 / +26 | +2 / +26 | +3 / +27 | +2 / +27 |
| 720 | +0 / -9 | +0 / -8 | +1 / -7 | +1 / -7 | +1 / -6 | +1 / -6 |
| 752 | +0 / +8 | +0 / +10 | +1 / +10 | +1 / +10 | +1 / +10 | +0 / +10 |
| 784 | +4 / +16 | +4 / +18 | +5 / +18 | +4 / +18 | +5 / +19 | +4 / +19 |
| 816 | +6 / +4 | +7 / +6 | +9 / +7 | +8 / +7 | +9 / +7 | +9 / +7 |
| 848 | +10 / -5 | +10 / -4 | +12 / -2 | +10 / -3 | +11 / -2 | +10 / -2 |
| 880 | +1 / +2 | +1 / +4 | +5 / +6 | +4 / +5 | +6 / +6 | +5 / +5 |
| 912 | +2 / +8 | +3 / +9 | +18 / +12 | +21 / +9 | +26 / +10 | +25 / +9 |
| 938 | -25 / -16 | -12 / -13 | +117 / -11 | +140 / -15 | +151 / -14 | +151 / -15 |
| 942 | +18 / +3 | +1 / +4 | +444 / +6 | +454 / +3 | +422 / +3 | +416 / +2 |
| 944 | +3 / +1 | -6 / +3 | +126 / +5 | +45 / +1 | +36 / +2 | +35 / +1 |
| 946 | -7 / +4 | -1 / +6 | +1174 / +8 | +251 / +4 | +125 / +4 | +108 / +3 |
| 950 | -15 / +0 | -27 / +2 | +500 / +4 | +25 / +0 | +25 / +1 | +25 / +0 |
| 956 | -18 / +2 | -49 / +4 | +284 / +6 | no min / +1 | no min / +3 | no min / +1 |
| 976 | -25 / +6 | -109 / +8 | +1066 / +14 | no min / +7 | no min / +7 | no min / +5 |

Shape error at the table's cutoff, dB: below index 848 the state-variable filter 0.04-0.46 (44.1
kHz), Clean 0.09-0.68; from 880 the state-variable filter 0.32-2.52 at 44.1 kHz (up to 3.61 at 192
kHz), Clean 0.13-0.31 at every rate. Clean passes everything from index 592 up at every rate,
but fails index 560 at every rate (44.1 kHz too), so per Jacob's rule HPF24 stays on the
state-variable filter, with its exclusion above 937 at other rates (register entry 1). The
failure at 560 is the fit, not noise: the reference's best Clean match there sits 0.6 semitone
above the curve through its neighbours (544 and 576), and the checks are the only renders
between them. Kept locally: `scripts/gate_type5c.py` (fit and check), `scripts/surge_cut5c.py`
(sweep), the trial's full diff and the `fit.py` patch (scratch).

*Short envelopes at the 2 ms floor* (`84a8b30`, test only). Jacob's decision: commit the tables
only where they pass with the 2 ms floor for averaged checks. The averaged probes (fourth
session: means of 3-8 repeats at note 96, indices 40-96) were refitted against a new engine sweep
with the reference-matching options on (`surge/egshort_ref`, scene highpass off; the earlier
sweep had them off, and the analog attack clamp shortens attacks by up to 1 ms there), from 40,
56, 72, 88 and 96, and checked at 48, 60, 64, 68, 76 and 80. Decay passes at 48, 76 and 80 and
fails at 60, 64 and 68 (2.4-2.6 ms short); attack passes 6 of 6 at 44.1 and 48 kHz but not at
88.2-192 kHz; release fails at 68, and its committed table only holds from 112. So only the
decay table is extended: from index 72 (72-95 from the averaged fit, 96 up unchanged; joins
monotonically), with averaged checks at 76 and 80 (reference 24.73 and 30.40 ms; engine 25.03
and 29.03 ms at 44.1 kHz, worst over the six rates 29.02 / 28.84 ms at 176.4 kHz). The test renders
them at note 96 with the scene highpass off and one-period windows (0.956 ms) and a 2 ms floor;
with the old table they fail (56 ms engine, seen). The rest is in register entry 13.

*CLAP presets* (`56ebc92`). The plugin has no native sound contexts, so RFC 0005's path for
pooled instruments: the entry offers `clap.preset-discovery-factory/2` (and its draft-2 ID) with
one provider, which declares the `.fxp` file type and Surge's factory patch folder (factory
content) and user patch folder (user content), found as Surge finds its data (a `SurgeStorage`
built once in `init`, so portable installs and `PIPELINE_OVERRIDE_DATA_HOME` work as in Surge);
a folder that doesn't exist isn't declared. `get_metadata` checks the file's `.fxp` header only
(fast, as CLAP asks) and reports one preset named after the file, for `org.oscairn.analog`; a
file that isn't a Surge patch is an error. `clap.preset-load/2` (and the draft ID) takes a file
location without a load key, reads the patch data after the `.fxp` header (its size checked
against the file's) and loads it as a state load does (same refusals, the patch kept on
refusal); on success it asks the host to rescan parameter values and calls `loaded`, otherwise
`on_error` with the reason. Surge's third-party patch folder (2920 patches) isn't declared (not
asked for; one line). Patches the plugin refuses (Formula modulators) are listed, and refused
when loaded. Tests (`presets`): the provider declares only `.fxp` and the factory folder; a
crawl of it finds all 641 factory patches and each is listed as one preset named after its file
for this plugin; a non-patch file is refused with an error; every 8th factory patch loaded with
preset-load gives the same saved state and the same render (a note, 1 s at 48 kHz) as the same
patch loaded as state (79 compared; the 2 refused are refused by both and reported, keeping the
patch); a missing file, a load key and a plugin location are refused. Each check failed with its
part undone (no discovery factory, no preset-load extension, a wrong preset name, no rescan:
seen). Oscairn's own scanner (`oscairn-plugin-scan --presets`) on the built plugin lists the 641
factory patches with `PIPELINE_OVERRIDE_DATA_HOME` pointing at the source tree's data, and none
on this machine without it (no Surge data installed). The version stays 0.1.1: no sound or
memory figure changed.

*Native sound contexts and snapshots prepared beside* (`3a8b0ee` vendored headers, `0e9f667`
contexts, `f063847` headers updated, `5bc3457` snapshots prepared beside, `2593f69` version 0.2.0,
`d4fc698` memory check as a median, `b66bf6c` headers updated, `d15a237` ring-out, `53a885f`
draft 3 and the choke of a retiring tail;
asked by Jacob after the presets, so Performance switches reuse one instance). The plugin
offers `oscairn.sound-contexts/draft-5` through the Oscairn SDK's MIT helper
(`sound-contexts-plugin.hpp`, vendored unchanged with `sound-contexts.h`, `assets.h` and
`memory.h` from oscairn/oscairn `72afff2`, then `eecdee4` for `oscairn.sound-snapshots`,
`2d1424a` for its draft 2 and `b9e90c7` for draft 3,
`src/oscairn-clap/third_party/oscairn-clap`, the commit in `upstream.json`), as the sfizz, Octet and setBfree forks do. Not the design in
section 4 (contexts as scene blocks sharing one voice pool and the storage's tables): each
sound is a whole Surge synthesizer, which leaves the engine untouched (`[reference]` and every
existing plugin check unchanged) but costs memory, so the capacity is 8 (the default sound and
seven prepared). What a sound costs, measured with the allocator's statistics (the heap's in-use
count around preparing one from a factory patch): about 32 MiB at any rate (macOS: single
preparations 28.7-34.3 MiB, the median of three 32.1-34.2 MiB at 44.1, 96 and 192 kHz; Linux in
Docker 31.3 MiB; `sizeof(SurgeSynthesizer)` is 19.6 MiB, and a `SurgeStorage` built alone allocated 8.8
MiB), so 8 sounds hold about 260 MiB; sharing the storage's tables between
the synthesizers (section 4's plan) is what would make more sounds sensible. `oscairn.memory` declares 36 MiB a sound (the measurement plus room for a patch's
wavetables): the default sound as the context, every other prepared sound as shared.

How it works. The helper keeps the slots, the selection queue (applied at the start of each
process call, before its events), the voices (a note held in a sound), the pins and the
snapshot. A note keeps the sound its channel selected when it started: it goes to that sound's
synthesizer under an ID of the plugin's own, and its helper voice ends (`NOTE_END` with the
host's note ID) when the synthesizer reports that ID ended, or when a released note's
synthesizer has no voice left. Every pinned synthesizer (selected on a channel, holding a note,
or still sounding) renders a block at a time from the block phase counted from activation, as
the single synthesizer did, so events still take effect at the first block boundary at or after
them; a sound that rings on after its last note stays pinned until a block finds it without
voices or waiting notes and with its output exactly zero for a block, or under -120 dB for a
second (Surge removes the last voice while the scene's filters still ring), and gives up the pin
at the next boundary, before its output stops being read; a block likewise pins what it renders until the next boundary (a selection applied
mid-block would otherwise unpin a sound whose output the block still reads). The audio thread
decides what it may touch from what it knows itself (the channels' selections, its voices, its
tails, the block), never from the pin counts, which the main thread's `select` also raises: the
first version went by the counts, and TSan found its read of a freshly prepared synthesizer
racing the main thread's write. MIDI controllers, aftertouch and pitch bend go to the channel's selected sound and
to the sounds holding the channel's notes; note expressions and polyphonic modulation to the
notes they name; parameter changes to the sounds selected on the event's channel (all selected
sounds for channel -1); the parameter values shown are channel 1's sound, refreshed (with
`PARAM_VALUE` events) after a selection for every channel. A key whose note its synthesizer
already ended (a mono mode moved the voice to another key) is still released there, so the mono
modes don't return to it. `prepare` takes parameter values (by ID, in the parameters' units,
checked against their ranges; no patch text); `prepare_preset` takes a Surge `.fxp` file as
preset-load does, refusing what a state load refuses. The CLAP state stays the default sound's
patch (what the converter writes and the library packs); the snapshot holds every sound's patch
data in hex, a line each, and is validated by building every sound's synthesizer before
anything changes. `oscairn.sound-snapshots/draft-3` (instant Performance changes, Oscairn
`347b2fc`, `2d1424a`, `b9e90c7`) is the helper's `prepareSnapshot`, `setRingOut` and
`chokeRetiring`: every sound of the next Performance's snapshot,
its Default too, becomes a new context beside the playing ones, all or nothing; reading a
snapshot's patch builds its synthesizer into the patch, touching nothing a playing sound uses,
and publishing installs it. Nothing is shared between sounds (each synthesizer has its own
tables), so a change needs as many free contexts as the snapshot has sounds: with the default
playing, up to 7. Measured: preparing a two-sound snapshot beside the playing sound takes 25 ms
(macOS arm64, release build, main thread); the switch itself is the selections, applied at the
start of the next process call. An outgoing sound refuses `discard` while it holds a voice, a
tail or the block being rendered (the default sound, ID 0, is never discarded). With `ring_out`,
while the host gives a ring-out buffer (four channels) for a process() call, every sound
selected by no channel that still renders (an outgoing Performance's) adds its output to the
channels the helper's `retiring()` gives (0-1) instead of to the main port. At a second change
while the first one's tail rings, `choke_retiring` makes the helper choke that sound's voices into
channels 2-3; the plugin fades the whole sound's output there itself over the choke time (3 ms),
because its tail outlives its voices (the scene's filters), which would otherwise move to
channels 0-1 once the last voice ends, and then cuts it (all sound off, its tail's pin given up). A snapshot load or activation ends every note without the synthesizers
reporting it.

Differences from before, for a single sound: MIDI note-offs on a key held twice release the
oldest note first (the helper's MIDI dialect; before, both); a choked note's `NOTE_END` comes
after the helper's 3 ms fade rather than when Surge's voice ends; every note now ends with a
`NOTE_END`, MIDI notes too (with note ID -1). Known limit: a key released while its sound is no
longer pinned (its notes ended, its channels moved on) isn't released in that synthesizer, so a
mono patch prepared again later could remember it; nothing renders it meanwhile.

Tests (`sound contexts`): eight contexts, the default selected on every channel; every 64th
factory patch prepared with `prepare_preset` and selected on every channel renders exactly as
the patch loaded as state (refused patches refused by both); a sound prepared from parameter
values (oscillator 1 pitch +7, amp level -6) renders exactly as those values set, saved and
loaded; on channel 0, a note held while the channel switches to a prepared sound and a note
after it render exactly as each alone, summed (the held note stays on the default sound); a
snapshot with a sound selected on channel 3 loads, round trips byte for byte and plays as the
saved one, and a damaged one is refused and changes nothing; a selected sound, and one still
releasing a note, can't be discarded, and can once its note has ended (its `NOTE_END` seen); a
discarded ID can't be selected; seven sounds beside the default, then a refusal with a reason;
a preset the plugin refuses (a Formula modulator) isn't prepared; a snapshot's two sounds
prepared beside the default and selected at the cue play exactly as the snapshot loaded in
another instance (sounds built from the defaults instead: fails, seen); a note held across a
snapshot switch keeps the outgoing sound and the next note takes the incoming one (the sum of
each alone, exactly but for the outgoing sound's last ring below 1e-9 when it stops rendering);
with a four-channel ring-out buffer before every process() call, the held note plays in channels
0-1 (as alone, to its last ring below 1e-9) and the main output holds only the incoming sound,
exactly (fails with the routing undone, seen); at a second change (`choke_retiring`, then the
selections) the first change's tail fades out in channels 2-3 and is exactly silent there from
the choke time and two blocks on, while the second change's outgoing sound takes channels 0-1
exactly as it played before (fails without the plugin's own fade, seen); a snapshot needing more contexts than are free is refused and
adds nothing; 24 rounds of preparing,
selecting, discarding and (every sixth) preparing a snapshot while another thread plays notes
on four channels end with every
prepared sound discarded (under TSan: the race above, seen with the audio thread going by the pin
counts); the declared memory covers
what preparing a sound allocates (the median of three preparations), within 25 %, at 44.1, 96
and 192 kHz (not in sanitizer
builds, whose allocator the statistics don't see). Each failed with its part undone (notes always
to the default synthesizer; releases to the channel's current sound instead of the note's;
parameter values not applied; snapshot patches not validated; 30 MiB declared: seen). The
existing plugin checks pass unchanged. `oscairn-plugin-check` (Oscairn `72afff2`) passes in
the engine and in a worker (`OSCAIRN_PLUGIN_WORKER`, the worker processes seen as its
children); it plays the plugin as a pooled instrument, not through its contexts.

*Self sync* (time-boxed, measured, not built). One master period of each `m3b` sync probe
(note 48, averaged over 0.3-0.9 s) against a model, harmonics 1-40, residual against the
signal: a slave wave whose phase advances at the nominal ratio (2^(25 raw / 1200)) times a
window g of the master phase, which rises over [0, a], is 1, falls over b and is 0 for a span z
before the next period, the output g^p times the slave wave, through a one-pole highpass. With
the ratio fixed at its nominal value: sine raw 24 -38.9 dB (a 0.20, b 0.22, z 0.05, p 1.0), 96
-25.8; triangle 24 -34.8, 96 -22.4; Saw1 24 -17.0, 48 -6.8, 96 -21.6, 144 -15.0, 192 -16.2; Saw2
24 -17.0; square 24 -10.1, 96 -7.6 dB. A first fit with the ratio free (amplitude window only, no
slowing; -3 to -9 dB) found the nominal ratio within 1 % for the triangle at 24, Saw1 at 24-192 and
the sine at 96 (the sine at 24: 1.53 for 1.41), so the pitch scale is 25 cents a step, and the
sixth session's odd ratios came from its restart model. The slave doesn't restart: it slows to a stop and restarts. Window
widths differ between probes (a and b near 0.2 for sines and triangles; the saw and square fits
are poorer and less certain); more probes (sync amounts between 24 and 96, two notes) are needed
before a fixed window can be fitted. Not built.

*Wave shaper* (time-boxed, measured, not built). On a sine (`m2b` `ws_sine_*`, note 48), a
static curve (polynomial of degree 31) of the input fits one period, harmonics 1-40, to -33.7
(48), -23.7 (80), -20.3 (112) and -19.6 dB (128 at oscillator level 256); with a one-pole
highpass after the curve at 0.1 x f0 (13 Hz at note 48) -38.0, -36.8, -31.8 and -30.1 dB (0.05
and 0.2 x f0 both worse). So the shaper is close to a static curve plus a low highpass; whether
the highpass is fixed in Hz needs probes at another note. Saw2 through the shaper doesn't fit
this way (-5 to -11 dB; its input isn't a sine). Not built.

*Wave folder above 96 and Hard* (time-boxed). The same static-curve fit with and without the
highpass on `m2` `wf0_*`/`wf1_*` (harmonics 1-120): Soft 128 improves from -19.1 to -30.7 dB, but
Soft 64 and 96 and Hard 96 get worse with it, and above 128 both stay -11 to -21 dB (aliasing,
eleventh session). Not conclusive; nothing changed. *Resonance above 96:* not attempted.

*Converter* (not this repository): HPF24 stays type 5, subtype 0 (HP 24 dB, Standard) at
resonance 0.05 with the committed table and saturator offset -42.52 dB; nothing to change for it.
Had Clean been applied: `a_filter1_subtype` 2 (and 2 for filter 2), the new table, and filter
saturator offset -44.02 dB. Short envelopes: use the committed `ampDecay` values from index 72 (the
table below 72 holds index 72's value, unchecked); attack and release below 100 and 112 as before.
Presets and sound contexts need nothing from the converter, but the plugin is now 0.2.0
(`2593f69`): the converter still writes 0.1.0 for the plugin's nodes.

**Tests run in the thirteenth session.** GitHub Actions stayed down (local substitutes). macOS
arm64, every commit's tree exported (submodules at their gitlinks) and built from a fresh
directory, each test binary's hash printed: `surge-testrunner` built with Lua as Surge builds it
and run from the source tree, 212 passed and 1 skipped, and `[reference]` "Compared 635 patches,
skipped 6", all passed, on every commit; `oscairn-analog-tests` (with `SURGE_SKIP_LUA`): `84a8b30`
911 s, `56ebc92` 945 s (ASan/UBSan 144 s, TSan 717 s), `3a8b0ee` 993 s, `0e9f667` 929 s,
`f063847` **1 failure** (the memory check: one preparation measured 28.71 MiB against 36 MiB
declared, over 25 %; fixed by `d4fc698`), `5bc3457` 1245 s, `2593f69` 1174 s, `d4fc698` 2329 s,
`b66bf6c` 1950 s, `d15a237` 2470 s, `53a885f` 3638 s (ASan/UBSan 238 s, TSan 1017 s); the times
grew with other sessions' builds running beside. Every sanitizer run had 0 reports. From
`3a8b0ee` on, `oscairn-plugin-check` (Oscairn's build at the time) passed on each fresh build in
the engine and in a worker. The `84a8b30` and `56ebc92` testrunner runs were first started
from the build directory without Lua (wrong; they failed to find their data and the
Lua-dependent Formula patches) and then run again as above, which is what is reported.
Docker Linux (standard; exit 0, "all passed", 0 sanitizer reports): `84a8b30` 968 s, `56ebc92`
1032 s, `3a8b0ee` 1176 s, `0e9f667` 1105 s, `f063847` 1396 s, `5bc3457` 1269 s, `2593f69` 1489
s, `d4fc698` 1249 s, `b66bf6c` 1690 s, `d15a237` 1513 s, `53a885f` 1699 s; ASan/UBSan and TSan on
`56ebc92` (4647 s, 605 s) and on `53a885f`, the head (5968 s, 838 s). On Linux a sound measured
31.3 MiB. Commits whose trees were later rewritten before any push (contexts and version
commits made and remade while the snapshots, the block pin and the memory comment were fixed)
were never gated and don't exist on the branch. The measurement repository was not changed.

**Voice budget (2026-11-06, after the thirteenth session).** `oscairn.voice-budget/draft-1`
(Oscairn RFC 0018, step 4), asked by Jacob: the host shares one CPU budget among every plugin
and tells a plugin with this extension how many voices it may keep; the plugin chooses which of
its own voices to give up. Commits: `346e6e4` vendored headers, `eb474bd` the voice budget,
then version 0.3.0 with this entry.

*Headers.* The vendored Oscairn SDK headers are at oscairn/oscairn `0d6a3e1` (unchanged, the
commit in `upstream.json`), which adds `voice-budget.h` and the SDK's MIT helper
`voice-budget-plugin.hpp` (the order, `voices::Limits`, `voices::Candidate`, `voices::victim()`,
`voices::Report`), now included by `sound-contexts-plugin.hpp`. One change comes with them
whatever the host does: the sound-contexts helper's pool of notes (256 notes held or sounding,
every sound together), when full, gives up a note in that header's order (released before held,
the quietest by velocity, then the oldest) instead of the oldest. The helper's own limits are
never set (the plugin keeps its own `Limits`, below), so they stay off.

*What a voice is.* One Surge voice, so each unison copy of a note is a voice, in any sound the
audio thread may touch (selected, holding a note, ringing, or in the block being rendered). The
helper's own voices are notes, so its Engine's built-in budget (which gives up whole notes) is
not used; the plugin uses the SDK's `Limits`, `Candidate`, `victim()` and `Report` directly. Each
Surge voice is a `Candidate`: its sound's context ID and whether a channel selects it; held while
its note's key is down (the helper's note, so a key held only by the pedal counts as released);
in a sound whose play mode is any but Poly (the Mono modes and Latch), held
while its gate is on and marked mono, so a held or sustained mono or legato note is never given
up (the host sends no MONO or LEGATO flags yet; Surge's play mode stands in for them); ending
while it fades or is in Surge's uber-release; loudness its amp's gain this block (the Amp gain
parameter, velocity, the amp EG and the fade) times its note's volume expression; age from
`SurgeVoice::age` (blocks since it started).

*Giving voices up.* `set_limits` (audio thread, between blocks) copies the limits. At the start
of each `process()` call, after the selections, each sound past its own entry's limit and then
the instance past its limit gives up voices in `victim()`'s order. Before a note starts in a
sound, the voices it will add (its unison count; none for a mono sound already sounding, or a
Poly key that reuses its voice) make room the same way, with the note's sound as the new voice's
context. A voice given up fades: `SurgeVoice::fadeOut` scales its amp gain down linearly to zero
over the choke time (3 ms) rounded up to whole 32-sample blocks (5 blocks, 3.33 ms at 48 kHz;
3.63 ms at 44.1 kHz; 3.0 ms at 192 kHz), starting at the next block boundary, and the voice then
ends. Surge reports a note's end only when no other voice holds its ID, so `NOTE_END` comes after
a note's last voice. A voice that a mono mode or a repeated Poly key reclaims for a new note stops
fading (`reclaimVoiceFor` clears the fade, as it clears Surge's uber-release).

*Surge's Polyphony Limit under a budget.* Once the host has set a limit or an entry, the voice a
sound's full Polyphony Limit releases for a new note is chosen in the same order within that sound
(`SurgeSynthesizer::VoiceChooser`, asked by `softkillVoice`); it is still released with Surge's
uber-release. With no limit and no entry (never set, or set back to 0 with no entries), the
chooser answers nothing and Surge chooses as stock (register entry 19).

*Reports.* `active_voices`: every Surge voice of those sounds, fading ones included; no cost
estimate (0). `notes`: each of the helper's notes by the host's ID, port 0, channel and key, with
its voices and its loudest voice's gain. `honors_volume`: true (every voice multiplies its amp by
its note's volume expression).

*Upstream files.* `SurgeVoice.h` (`fadeOut`, the fade counters, `ampGain`), `SurgeVoice.cpp` (the
fade in `SetQFB`, the end in `process_block`), `SurgeSynthesizer.h` (`VoiceChooser`) and
`SurgeSynthesizer.cpp` (the chooser in `softkillVoice`, the cleared fade in `reclaimVoiceFor`):
33 lines. Nothing changes without a budget: the fade counters stay 0 and the chooser is null in
Surge itself.

*Known limits* (not tested). A note waiting for its key-on delay gets room when it is played,
not when it starts; voices it starts later count from the next `process()` call. A fading voice
still counts toward Surge's Polyphony Limit (Surge counts every voice not in uber-release), so
under a budget a new note can release one voice more than it needs while others fade. Surge's
pool of 64 voices a sound is unchanged (a voice past it isn't started, as stock); the porting
guide's "cut only when the pool has no free slot" has no counterpart. The worker protocol
doesn't carry the extension yet, so it was tested in-process only.

Tests (`voice budget`): every voice follows the volume expression (a note at expression 0 is
silent); four held notes (a sine, velocity -24 dB at 0) with the limit lowered from none to 2: the
removed signal (the render without the limit minus the render with it) starts within a block
(sample 0 here, the change landing on a block boundary), the two quietest notes' `NOTE_END` come
2 to 5 ms later (by 144 samples at 48 kHz, reported at the start of the fade's last block), their
fading voices are still counted and reported a block in, the largest second difference of the
removed signal during the fade is 0.3 % of its level (the check: 2 %; by its curvature a sine of
these keys stays under 0.5 % and the 3.33 ms ramp's corners add about 0.6 %; a cut shows the
signal's value then: 7.2 % with the fade undone), and the two
loudest notes sound on; across two sounds (the default on channel 0, a prepared one on channel 1)
with the limit at 4: nothing is given up at the limit; a new note on channel 1 takes the default
sound's released note, though it is louder than a held note of its own sound (in the same
`process()` call: the room is made at the note); the next new note takes its own sound's quietest
held voice, not the quieter held voice of the default sound; unison 3: six voices, two notes of
three voices each by the host's address, a note's loudness its amp's gain (velocity 64 against
127 at -24 dB: within 2 %); two of a note's three voices given up leave it sounding without
`NOTE_END`, and its last one given up ends it only after the fade; a Polyphony Limit of 4 with a
fifth note: without a limit the plugin renders exactly as stock Surge (the oldest note goes),
under a budget with no limit the quietest note goes, and with the budget removed it renders as
stock again. `set_limits`, `active_voices` and `notes` run under the allocation guard, as every
`process()` call does. Each check failed with its part undone (seen): no fading at a block's start
(the lowered-limit and unison checks fail); no gain fade, the voice cut after the fade time
(the start and the click checks fail); a one-block fade (the 2 to 5 ms, counting and NOTE_END
checks fail); no room made at a note (both two-sound checks fail); every voice held (both);
every sound one context (own sound first fails); no loudness (quietest fails throughout); fading
voices not counted; one voice a note in `notes`; the voice ending at the fade's start (counting,
duration, click and NOTE_END checks fail); no Polyphony Limit chooser (the budget check fails);
the chooser without a budget (the stock comparison fails); an allocation in the limit path (the
guard aborts). The existing plugin checks pass unchanged, the stock comparison of plain patches
included.

The plugin is 0.3.0 (with this entry): a new extension, and the stealing order changes under a
budget. Without a budget it sounds as 0.2.0. The converter still writes 0.1.0 for the plugin's
nodes (not changed here).

**Tests run for the voice budget.** GitHub Actions stayed down (local substitutes). macOS arm64,
each commit's tree exported (submodules at their gitlinks) and built from a fresh directory:
`346e6e4` and `eb474bd`: `surge-testrunner` built with Lua and run from the source tree, 212
passed and 1 skipped, `[reference]` "Compared 635 patches, skipped 6", all passed;
`oscairn-analog-tests` all passed (858 s each); `oscairn-plugin-check` (Oscairn `0d6a3e1`, built
for this) passed in the engine (not in a worker). Docker Linux (standard; exit 0, "all passed", no
sanitizer reports): `346e6e4` 1013 s, `eb474bd` 1022 s of tests. The commit with this entry (the
version) was gated after it was written: its results are not recorded here.

**Reset at once (2026-11-06, after the thirteenth session).** Oscairn main `11ca03a` resets an
instance it takes over at a Performance change (CLAP `reset`, on the audio thread), then gives it
the incoming state; its porting guide now says a reset must take effect at once. The plugin's reset
called Surge's `allSoundOff()`, which only starts a fade of 8 blocks; at its end `stopSound()` ran
and ended every note struck meanwhile, so the notes played at a change were cut (in the converted
library, "Goofing" opened after "For the First Time" and "fpt 2", with silence before each change,
played a chord for 2 s at 0.0008 RMS, against 0.047 opened alone). Commits:
`c77e48c` the fix and its tests, then version 0.3.1 with this entry.

*What a reset does now.* Reset and activation clear each synthesizer at once (`clear()` in
`OscairnAnalog.cpp`), so that it then renders as a fresh one given its state, sample for sample:
`stopSound()` (voices, waiting notes, keys, pedals, hold buffers, the scene's filters) with no
fade left pending (also one MIDI's all sound off started); the MIDI channel state as constructed
(bend, pressure, RPN selection); MPE off (only RPN turns it on here); poly aftertouch; the held-key
order behind the lowest, highest and latest key sources; where portamento glides from (key 60);
the controller sources (bend, wheel, breath, expression, sustain, aftertouch, the key sources) at
0; the alternating sources' phase; the scene LFOs set up again as the constructor does (they run
every block, voices or not, and kept their phase into the next note); which modulators run (worked
out at each block; a voice started before the first block reads it as constructed); the output
gain's glide in from 0 over the first blocks; the scene data without the last block's modulation;
the song position (a free-running LFO started before the first block reads it); the random seed.
Reset clears at once the sounds the audio thread may touch (selected, holding a note, ringing,
or in the block being rendered); the others, which the main thread may be using, are marked and
cleared when next selected, at the start of a `process()` call before any event. Reset has no
output events: notes it ends get no `NOTE_END`, then or later. Nothing allocates or locks.

*What a reset keeps* (state, not processing): parameter values; the mod wheel's position, which a
Surge patch saves (it glides there from 0, as after a load, so a reset leaves the saved state as it
was); the prepared sounds and the channels' selections; the voice budget's limits; the host's tempo
and transport (given with each `process()` call).

*Upstream files.* `LFOModulationSource::assign` also clears the LFO's outputs and its first-attack
flag (3 lines and an include), so assigning again starts it afresh. Surge assigns each LFO once
(scene LFOs at construction, voice LFOs on a constructed voice), so Surge itself is unchanged.

*Without the reset* (a taken-over instance deactivated, loaded and activated, as before
`11ca03a`), activation left voices releasing (it called `allNotesOff()`), the scene LFOs' phase,
the controllers and the rest above; activation now clears the same way.

*Found, not changed.* (1) A patch loaded before activation can render differently from the same
patch loaded after it (factory "Asymmetry" and "Japanese Unison": 8.3 and 3.3 dB of residual
between the two orders on fresh instances; reloading or reactivating doesn't change either
order's result). The takeover (reset, deactivate, load, activate) renders exactly as a fresh
instance loaded before activation. Cause not looked for. (2) A sound that `chokeRetiring` fades out
ends with `allSoundOff()`, which then waits, unprocessed, until the sound is next rendered; if it is
selected again, notes in its first 8 blocks would be cut the same way. Not tested; a reset clears
it.

Tests (`reset`): notes struck in the block after a reset (after held notes, both pedals, bend,
pressure, controllers, MPE turned on by RPN and MIDI's all sound off just before it) sound at full
level, and no `NOTE_END` follows for the notes it ended; after a reset the instance renders
bit-identically to a fresh one with the same state, for every 8th factory patch and a patch that
routes every cleared controller, key and alternating source and a free-running tempo-synced voice
LFO to pitch (played on channel 1, where MPE left on would change the bend): with the same state
loaded again, with another state played before, with no load (the saved state unchanged, against
a fresh instance given it), and deactivated, loaded and activated (against a fresh instance loaded
before activation); with two prepared sounds, one taken off its channel while its notes ring, every
sound plays as in an instance that never played; a sound idle and unpinned at the reset, selected
again, plays as one that never played. Every reset runs under the allocation guard. Each failed
with its part undone (seen): the old reset (full level, and every comparison but the deactivated
one, as activation now clears); no pending fade cleared
(full level); no marking of idle sounds (idle sound); the channel state, MPE, poly aftertouch, the
key order, the portamento key, the controllers, the alternating sources, the LFO setup, either
upstream line, which modulators run, the output gain, the scene data, the song position, the seed
(comparisons); the wheel zeroed (the state check); an allocation in reset (the guard aborts). Not
shown by a test and kept as activation did: the host position (`ppqPos`) at 0.

The plugin is 0.3.1 (with this entry): a fix; patches sound as in 0.3.0 except after a reset or a
reactivation. The converter writes the plugin's own version for its nodes (0.3.1 in the rebuilt
library).

**Fourteenth session (2026-11-06): HPF24 on HP 24 dB Clean, refitted with probes between 528
and 592.**

Jacob's decision (after the thirteenth session): record reference probes between index 528 and
592 so that Clean (type 5, subtype 2) can be refitted. Two reference batches, the same probe as
the HPF24 sweeps (`cutall`, `kinks`: a saw at note 36, velocity 100, held 1 s with a 0.1 s tail,
filter 2 thru, resonance 0, voltage drift 0): `hp24gap`, every 4 from 532 to 588 but 544, 560
and 576 (12 renders), and `hp24rep`, 528, 544, 560, 576 and 592 again (5). The repeats give the
same best Clean match as the first recordings to 0.001 semitone, so the reference is repeatable
here and 560 was not a bad recording. Best Clean match (the `cut5c` sweep), semitones: 548
-8.33, 552 -7.69, 556 -7.17, 560 -6.65, 564 -7.34, 568 -6.81, 572 -6.28, 576 -5.85: it falls
by 0.7 semitone between 560 and 564, where the shape measure's harmonic 2 leaves its 40 dB range
(560: -38.6 dB below the largest harmonic, 564: -40.7; first harmonic used 2 up to 560, 3 from
564), as the fundamental does at the twelfth session's break at 475. The state-variable filter's
best match rises smoothly through the same probes. So the fit gets a second break at 562.

Fit (measurement side, the thirteenth session's `type5c3` with the new probes): fitted from the
earlier set plus 536, 552, 568 and 584 (every 8th index in the gap), breaks at 475 and 562,
non-decreasing; checked at every earlier check plus 532, 540, 548, 556, 564, 572, 580 and 588:
33 of 33 within 50 cents (560 -37, 564 +33, engine minus reference; the rest within 29). Without
the break at 562 (same points) 560 fails (-58), as before. The table changes only at indices 513-607 (by up
to 0.49 semitone); index 0 is unchanged, so the saturator offset stays -44.02 dB (thirteenth
session's rule). The rate match is the thirteenth session's Clean fit (regenerated at 96 kHz with
`oscairn-analog-rate-match fit HPF24 96000`: the same knots), with one fix: the trial's header held Clean's clamp, 75, twice
at the end of the grid, so the lookup's last slope was 0/0 and gave NaN above 75 (Surge's
`[signalpath]` "Filter Cutoff Rate Match" failed, seen); the tool now writes each cutoff once and
a knot at the clamp plus one when the clamp is the last, which changes only HPF24's last knot, to
76 at the same value). The plugin test checks HPF24 at
every index at every rate (the exclusion above 937 is removed); the new checks add the 8 gap
indices (their reference responses in `FittedTables.h`).

Best match from the table, cents (the plugin's checks, tolerance 50), state-variable filter
(before, with the gap indices added as checks; 938 and up at rates above 44.1 kHz were
printed, not checked) / Clean (now); "-": not a check of that table:

| Index | 44.1 kHz | 48 kHz | 88.2 kHz | 96 kHz | 176.4 kHz | 192 kHz |
|---|---|---|---|---|---|---|
| 368 | - / +15 | - / +18 | - / +25 | - / +23 | - / -25 | - / -25 |
| 400 | +0 / +6 | +0 / +7 | +0 / +5 | +0 / +4 | +0 / +26 | +0 / +25 |
| 432 | +0 / +2 | +0 / +2 | +0 / +10 | +0 / +6 | +0 / +0 | +0 / +1 |
| 464 | +20 / -14 | +20 / -12 | +21 / -11 | +20 / -12 | +21 / -20 | +20 / -9 |
| 496 | +2 / +8 | +2 / +10 | +2 / +19 | +2 / +22 | +3 / +33 | +3 / +42 |
| 528 | +1 / +8 | +1 / +9 | +1 / +9 | +1 / +9 | +1 / +9 | +1 / +14 |
| 532 | +2 / +5 | +2 / +6 | +2 / +6 | +2 / +6 | +2 / +8 | +2 / +11 |
| 536 | +3 / - | +3 / - | +4 / - | +4 / - | +4 / - | +4 / - |
| 540 | +1 / +1 | +1 / +3 | +2 / +3 | +1 / +2 | +2 / +5 | +1 / +6 |
| 548 | +1 / +0 | +1 / +1 | +1 / +1 | +1 / +1 | +1 / +4 | +1 / +0 |
| 552 | +3 / - | +4 / - | +4 / - | +4 / - | +4 / - | +4 / - |
| 556 | +4 / -3 | +5 / -2 | +5 / -2 | +5 / -2 | +5 / -1 | +5 / +0 |
| 560 | +7 / +40 | +7 / +42 | +7 / +42 | +7 / +42 | +7 / +42 | +7 / +38 |
| 564 | -8 / -35 | -7 / -33 | -6 / -31 | -7 / -30 | -6 / -23 | -6 / -26 |
| 568 | -4 / - | -4 / - | -3 / - | -3 / - | -3 / - | -3 / - |
| 572 | +0 / +2 | +0 / +3 | +1 / +5 | +1 / +5 | +2 / +11 | +2 / +11 |
| 580 | +1 / +3 | +1 / +4 | +2 / +6 | +2 / +7 | +2 / +11 | +2 / +12 |
| 584 | -2 / - | -2 / - | -1 / - | -1 / - | -1 / - | -1 / - |
| 588 | +1 / -1 | +1 / +0 | +2 / +2 | +2 / +2 | +2 / +7 | +2 / +8 |
| 592 | +0 / -8 | +0 / -6 | +0 / -4 | +0 / -4 | +0 / +2 | +0 / +1 |
| 624 | -5 / -34 | -4 / -32 | -4 / -31 | -4 / -31 | -4 / -28 | -4 / -28 |
| 656 | +1 / +23 | +1 / +24 | +2 / +25 | +1 / +25 | +2 / +26 | +1 / +25 |
| 688 | +2 / +25 | +2 / +26 | +3 / +26 | +2 / +26 | +2 / +26 | +2 / +27 |
| 720 | +0 / -9 | +0 / -8 | +1 / -7 | +1 / -7 | +1 / -6 | +1 / -6 |
| 752 | +0 / +8 | +0 / +10 | +1 / +10 | +1 / +10 | +1 / +10 | +0 / +10 |
| 784 | +4 / +16 | +4 / +18 | +5 / +18 | +4 / +18 | +5 / +18 | +4 / +18 |
| 816 | +6 / +4 | +7 / +6 | +9 / +7 | +8 / +7 | +9 / +7 | +8 / +7 |
| 848 | +10 / -5 | +10 / -4 | +12 / -2 | +10 / -3 | +11 / -2 | +10 / -2 |
| 880 | +1 / +2 | +1 / +4 | +5 / +6 | +4 / +5 | +6 / +6 | +6 / +5 |
| 912 | +2 / +8 | +4 / +9 | +18 / +12 | +21 / +10 | +26 / +10 | +25 / +9 |
| 938 | -25 / -16 | -12 / -13 | +117 / -11 | +140 / -15 | +152 / -14 | +151 / -15 |
| 942 | +18 / +3 | +2 / +4 | +444 / +6 | +454 / +3 | +422 / +3 | +416 / +2 |
| 944 | +3 / +0 | -6 / +3 | +126 / +4 | +45 / +1 | +36 / +2 | +35 / +1 |
| 946 | -7 / +4 | -1 / +6 | +1174 / +8 | +251 / +4 | +125 / +4 | +108 / +3 |
| 950 | -15 / +0 | -27 / +2 | +500 / +4 | +25 / +0 | +25 / +1 | +25 / +0 |
| 956 | -18 / +2 | -49 / +4 | +284 / +6 | no min / +1 | no min / +2 | no min / +1 |
| 976 | -25 / +6 | -109 / +8 | +1066 / +14 | no min / +6 | no min / +8 | no min / +5 |

Shape error at the table's cutoff over the 32 indices both check, dB: 44.1 kHz mean 0.387
(state-variable) / 0.387 (Clean), RMS 0.65 / 0.42, largest 2.52 / 0.68; 48 kHz mean 0.346 /
0.388, RMS 0.57 / 0.42; over all six rates mean 0.452 / 0.383, RMS 0.80 / 0.42, largest 3.61 /
0.68. Below index 848 Clean is worse (mean 0.45 against 0.15 at every rate, largest 0.68 against
0.73); from 880 it is better (0.13-0.31 at every rate against 0.25-3.61). Clean passes everywhere
and its shape error is not worse overall, so HPF24 is now Clean (register entry 1 holds what
differs). Kept locally, not in the measurement repository: the `fit.py` break and non-decreasing
options (the thirteenth session's patch) and the fit script.

*Converter* (not this repository): HPF24 is type 5, subtype 2 (HP 24 dB Clean) at resonance
0.05, with the new `cutoffHp24` table (its checked span 352-992); filter saturator offset -44.02
dB, for when the filter saturator drive is converted (the converter drops it for now).
The rate match is looked up by type and subtype, so with the option on a patch still on HP 24 dB
Standard (subtype 0) is no longer rate matched at other rates; a stock patch is unchanged (the
option is off by default).

The plugin is 0.3.2 (with this entry): HPF24 Parts sound different (converted with the new
mapping). The commits with this entry were gated after it was written: their results are not
recorded here.

**Sixteen sounds: one store of voices (2026-11-06, after the fourteenth session).** Asked by
Jacob: let one instance hold more than eight sounds by sharing what its synthesizers hold in
common. Commits: `3d6ee04` a choked-out sound cut at once, `b5e8db8` the shared voices, then version
0.3.3 with this entry.

*What a sound held* (macOS arm64, measured with the plugin's own configuration: `sizeof` and the
allocations of a second `SurgeSynthesizer` built beside a first). The `SurgeSynthesizer` object
was 19.55 MiB, of which its voices (`voices_array`, 2 scenes x 64 `SurgeVoice` of 145.4 KiB, mostly
each voice's three oscillator buffers) 18.17 MiB; its storage allocated 8.8 MiB more: the patch
(`SurgePatch`, 3.33 MiB), ten wavetables of 205 KiB each (two scenes' three oscillators, three
clipboard slots and the window oscillator's table; 2.0 MiB), the delay-line pool (0.5 MiB), the
hold buffer pools and smaller allocations; the `SurgeStorage` object itself is 0.97 MiB. A
prepared sound measured 32.1-34.2 MiB in all. The constant tables Surge builds per synthesizer
are small: the dB, envelope-rate, glide, pitch and 2^x tables are arrays inside `SurgeStorage`
(about 35 KiB together, and the pitch tables follow each synthesizer's tuning), the sinc tables
41 KiB, the window oscillator's wavetable 205 KiB. Sharing them would save under 0.5 MiB a sound,
so they stay each synthesizer's own; what is per synthesizer and not state is the voices, which
only sounding notes use.

*What changed.* The sounds' synthesizers share one store of Surge's voices (a `VoiceStore`: the
same 2 x 64 voices and their in-use marks), made once per instance and handed to each synthesizer
when it is built (`SurgeSynthesizer`'s new constructor argument; stock Surge builds its own store
and behaves as before). A synthesizer sharing a store takes any free voice of it, of either scene's
row, and at most 64 at once, as many as its own pool held (its filter state arrays hold 64 voices,
so more would overrun them). So the sounds together have 128 voices; a sound alone plays exactly
as before (it never reached past 64). A prepared sound now allocates 12.8-14.1 MiB (the median of
three preparations, at 44.1, 96 and 192 kHz, macOS arm64; single preparations 12.1-14.2, the heap's
in-use count moving between them), declared 15 MiB; the store, 18.2 MiB, is declared once in the
shared bytes. The cap is 16 sounds with the default: 16 x 15 MiB plus the store is about 260 MiB,
under the 288 MiB that 8 sounds declared before. The voice budget's candidates are now the store's 128
voices, by their place in it.

*A choked-out sound is cut at once.* A sound `choke_retiring` faded out ended with Surge's
`allSoundOff()`, which only starts an 8-block fade and waits for the synthesizer's next blocks; its
voices stayed allocated meanwhile (with a shared store they would be freed by whichever thread next
touched the synthesizer, perhaps the main thread at a discard), and if the sound was selected again
the notes in its first 8 blocks were cut (found, not changed, in the 0.3.1 entry). It now ends with
`stopSound()`, on the audio thread, at the end of the fade.

*Upstream files.* `SurgeSynthesizer.h` and `.cpp`: the store type, the constructor argument, the
pointers that replace the two arrays (`voices_array`, `voices_usedby`, same indexing) and the
shared branch of `getUnusedVoice`: 33 lines added, 17 removed. Without a store handed in, Surge is unchanged.

*Known limits.* Past 128 voices across the sounds (each within its own 64) a new voice isn't
started, as stock Surge does past its 64; the voice budget normally limits the sounds well before
(register entry 20).

Tests (`sound contexts`): sixteen sound contexts; fifteen sounds beside the default, then a
refusal; a snapshot needing more contexts than are free (fifteen prepared) refused; the declared
memory (15 MiB) covers what preparing a sound allocates, within 25 %, at 44.1, 96 and 192 kHz;
beside another sound holding 64 voices, a sound with a Polyphony Limit of 64 and the most unison
voices holds 64 notes, releases them and strikes 64 more, its output exactly stock Surge's (the
sum with the other sound alone, sample for sample); a sound choked out by `choke_retiring` and
selected again plays a note struck in its first block at full level (0.1012 RMS, as a fresh
instance). Each failed with its part undone (seen): without the store (32 MiB a sound against 15
declared; eight contexts); without the 64-voice limit of a shared synthesizer (the stock
comparison); with `allSoundOff()` at the choke's end (0.0000 RMS). A first declaration of 16 MiB
failed the memory check in the Release gate build at 96 kHz (median 12.8 MiB: 16 is 25.4 % over).

The plugin is 0.3.3 (with this entry): sixteen sounds where there were eight; a sound alone sounds
as in 0.3.2. The CLAP state and the snapshot are unchanged, so the converter needs nothing new. The
commits with this entry were gated after it was written: their results are not recorded here.

**Sound-snapshots draft 4 (2026-11-06).** Oscairn main replaced `oscairn.sound-snapshots/draft-3`
with draft 4 (`33206e5`, `0a5c14f`; RFC 0005, "One sound on its own"), so a plugin offering draft 3
no longer had instant Performance changes there. Commits: `b634e13` the vendored headers (Oscairn
`733baca`: `sound-contexts.h` and `sound-contexts-plugin.hpp` changed, the others unchanged), `7f757db`
the two new calls, then version 0.3.4 with this entry.

*What draft 4 adds.* `save_sound(id)` writes a snapshot holding only that sound, as its Default
selected on every channel and playing on the main output; Oscairn saves a sharing part as a Sound
with it. `sound_values(id)` gives a sound's parameter values while no channel selects it, for a
sharing part's view. The SDK's `saveSound` and `soundValues` implement them from two instrument
hooks. Here `saveSound` fills the sound's patch data from its synthesizer around the helper's call
(as the snapshot save does), `alone()` changes nothing (every sound plays on the main output; the
output bus comes with sharing), and `patchValues` reads every parameter's value from the sound's
synthesizer as the parameters show them (clamped as `get_value` is). The helper calls it only for a
sound no channel has selected since it was prepared, which the audio thread doesn't touch. The
snapshot and the CLAP state are unchanged.

Tests (`sound contexts`): the extension is found as draft 4 and not as draft 3; a prepared sound
and the default, each saved on its own, load into a fresh instance as its only sound, selected on
every channel, and render exactly as an instance with that patch as its state; an unknown sound
isn't saved; an unselected sound's values (all 447 parameters) equal what an instance with its
patch shows through `get_value`; a selected or unknown sound gives none. Seen failing with the old
headers (draft 3), with the two calls null, and with `save_sound` writing every sound.

The plugin is 0.3.4 (with this entry): Oscairn's instant Performance changes work again with
current Oscairn. Nothing changes for the converter. The commits with this entry were gated after
it was written: their results are not recorded here.

**Parts sharing one instance (2026-11-06).** Oscairn puts several parts on one instance of a
plugin that can share (RFC 0021, slice 3, and "Formed on their own"): native sound contexts and
their snapshot with draft 4's calls, no audio input and no note output, more than one output port,
and a stepped parameter tagged `OSCAIRN_ROLE_OUTPUT_BUS` (oscairn.ui), which the host sets on each
part's sound with a parameter event on the part's channel. Commits: `11fd5d5` the parameters' roles,
`755cf12` a test of per-part voice budget entries, `714705c` the output buses, then version 0.4.0
with this entry.

*Roles.* Declaring any oscairn.ui role turns Oscairn's guessing from parameter names off for the
plugin, so the plugin declares the roles the guessing gave: the scene's Volume (level) and Pan, filter
1's Cutoff and Resonance, the amp envelope's and the filter envelope's four stages. Not Hold: the
guessing gave the amp envelope bank's hold knob to Voice LFO 1's Hold (the first parameter named
"Hold"), which is not an amp envelope stage, so that knob is now blank. `ui.h` is vendored unchanged
from Oscairn `733baca`.

*Buses.* A stereo main output and eight stereo sound buses ("Sound bus 1" to "8"), the Basic Synth's
count: an instance holds as many parts as it has free sounds, ports and channels, so up to nine,
with seven sounds left for a Performance change's incoming sounds beside them.
The Output Bus parameter ("Global/Output Bus", stepped 0-8: Main, then a bus; a list, the role) is
per sound. A parameter event for it sets the bus of the sounds its channel selects (all sounds a
channel selects for channel -1), not of sounds that only hold the channel's notes, so an outgoing
sound's bus stays. The host's ports beyond Main are optional to the plugin: it writes as many as it
is given, and every port it is given each sample (unused ones silent).

*A bus change moves the whole sound.* Surge renders a sound's voices together and then through the
scene's stages (hardclip, the oversampling filter, the low cut, the scene's level), which keep
state, so a voice can't keep its own port without a second scene chain per bus. A bus change takes
effect at the block boundary its event lands on; a sound whose last block sounded crossfades from
the old port to the new one over the choke time (3 ms, linear), one that was silent moves at once.
Notes held across the change move with it (register entry 21). Oscairn sets a part's bus when the
part is made and at a Performance change's cue, before the part plays, so its parts don't hear this.

*Ring-out.* Only the main output rings out (RFC 0005's limit): a retiring sound on a bus keeps
playing on its port, and `choke_retiring` doesn't fade it (its voices are choked by the helper as
before). `save_sound` writes the sound on Main (the SDK's `alone()`), and `sound_values` includes its
bus.

*Formats.* The snapshot is `oscairn-analog-sounds-2`: each sound's bus, then its patch data in hex.
The CLAP state is a line `oscairn-analog-state-1 <bus>` (the default sound's bus) followed by the
default sound's patch data; patch data alone still loads, on Main (the converter's states, and
Surge's own). No reader of the old snapshot format is kept.

*Voice budget per part.* With a shared instance Oscairn sends an entry per part (its sound's context,
reserve and priority, limit 0) and the instance's limit. The voice budget (0.3.0) handled that
already; the test shows it.

Tests (`ports and parameters`, `per-part voice budgets`, `sharing`): exactly the twelve roles above
and the bus's; a main output and eight buses with their IDs, names and flags; the bus parameter's
info (stepped 0-8, default 0), hint (list) and texts ("Main", "Sound bus 3"; "Sound bus 9" refused);
two parts on one instance (the default sound, a factory patch, on channel 0 and bus 1; a preset on
channel 1 and bus 2, set with events on their channels), notes on both and a bend on channel 1 only,
render on their buses sample for sample what two instances render on Main, with Main and buses 3-8
silent (every port filled with 1 before each call, so an unwritten port shows); `get_value` shows
channel 1's sound's bus; the state carries the default sound's bus and a fresh instance given it
plays there; the snapshot restored plays each sound on its bus and saves the same text; prepared
beside in another instance, the sounds play on their buses; a sound on bus 4 saved alone plays on
Main in a fresh instance, and its values end with its bus; a bus change while a note is held: before
the change the old bus is the plain render exactly and the new one silent, from the fade's end the
old one is silent, the two add up to the plain render within 1e-6, and no step on either is larger
than the plain render's largest; a retiring sound on bus 3 plays on there and the ring-out buffer
stays empty; per part, a limit of 3 over four held notes on two parts: a part within its reserve
(2) keeps its notes and the other (reserve 0) gives up its oldest though its priority is the higher,
and with no reserves the lower-priority part's note goes, not the oldest. Seen failing (one at a
time): the sound's output always on Main (six checks), the snapshot's bus not written (restored,
prepared beside), the state's bus not written (the state check), no crossfade (the step check:
0.2201 against 0.2040), every retiring sound ringing out (the ring-out check), every voice's context
taken as 0 (the second budget check); the roles test without the extension.

*Oscairn host side* (Oscairn `733baca`, built from a `git archive` export outside both
repositories; the plugin a Release build of `714705c` from an export with its submodules).
`oscairn-plugin-check` passes in the engine and in a worker. Oscairn's own `control-shared-parts`
is written for the Basic Synth, so a copy adapted outside the repositories ran: this plugin's
parameter IDs (filter 1's cutoff and resonance and oscillator 1's pitch as the three parts'
values), fifteen sounds to fill an instance where the Basic Synth needs seven, its nine-part spill
and protocol cases left out (they count the Basic Synth's eight sounds), and two changes for this
plugin: every play starts after a second of silence, since a Surge synthesizer's output gain
glides in from 0 over its first blocks (rendered after 1, 30 and 40 idle blocks, a note differs
by 0.064 between 1 and 30 and not at all between 30 and 40); and in the worker run the separate
layout is made in the engine (below). Three Analog parts saved with an instance each open on one
instance and render what the three instances rendered, largest difference 0; the second part shows
its own sound's values; saved, the shared layout keeps one engine with three `sound`s and reopens
on one instance the same; three added one by one share one instance with a channel and bus each and
sound as three instances; a sharing part saved as a Sound joins a playing instance where it is
loaded, and alone keeps one of its own; changes from the separate layout to the shared one and back
load nothing and sound as each opened fresh. In-process and with every plugin in a worker, the same
three checks fail: "redone, as before" and twice "and sounds as before", each comparing a second
play on the same instance with the first, which Surge doesn't repeat (a note played again in one
instance after its release differs from the first by 0.37: its random sources and voice state move
on), not anything sharing does. With the plugin as at `755cf12` (no buses) the three parts open on
three instances and the sharing checks fail.

*Found, not changed (Oscairn).* In a worker, Oscairn sets the worker's memory limit from the
plugin's declaration when it activates (declared plus half and 64 MiB) and doesn't refit it when
sounds are prepared later (`prepareSound`, `prepareSnapshot`): filling one instance with fifteen
sounds stopped the worker at the ninth (its physical footprint past the limit; with the limit
measured on a wrapper process instead, the whole test ran). A shared instance that grows by many
sounds after it activates would be stopped the same way. Reported, not worked around here.

The plugin is 0.4.0 (with this entry): a sharing instance's ports and parameter, and new state and
snapshot formats. The converter keeps working; what it would need is below. The commits with this entry were gated after
it was written: their results are not recorded here.

*Converter* (not this repository; not changed). It writes this plugin's nodes as a plain state
(`state 1`, `state.clap` the patch data, `perfconv/surge.py` `Patch.state`) and no native snapshot.
(1) Those states still load, on Main, so nothing is needed for them; to write the current format it
would prefix the line `oscairn-analog-state-1 0` and a newline to the patch data. (2) Its
`parse_state` (tests and checks) refuses a state that doesn't start with "sub3", so it must skip
that first line to read a state the plugin saved (in a library re-saved by Oscairn). (3) Should it
ever write the plugin's sound snapshot, each slot's line is `1 "<name>" <bus> <hex patch data>` in
`oscairn-analog-sounds-2`. (4) The plugin's version for its nodes is 0.4.0. (5) Shared nodes:
Oscairn now forms them itself when it opens a Performance saved with an instance per part, so the
converter needn't write them.

**Factory patches from Oscairn's asset library (2026-11-07).** Jacob's decision: the plugin ships
Surge's factory patches as an Oscairn asset library, `org.oscairn.analog-factory` 1.0.0, holding
`patches_factory/` from upstream `5a4828a33` unmodified (with Surge's AUTHORS, LICENSE and a NOTICE),
and finds it through `oscairn.assets/draft-2` (Oscairn `8a8fcc6`), whose `library_root(owner,
library_id, min_version, path, capacity)` gives an installed library's read-only folder. Commits:
`872bf4a` the lookup, its tests and the vendored `assets.h`, then version 0.4.1 with this entry.

*Where the factory folder comes from* (the preset provider's `init`, which gets the indexer, not a
host): Surge's data folder when an environment variable overrides it (`SURGE_DATA_HOME`,
`PIPELINE_OVERRIDE_DATA_HOME`: Surge's own `datapathOverriden`), as before; else, when the indexer's
`oscairn.assets` table has `library_root` and it answers true for `org.oscairn.analog-factory`
at least 1.0.0, `<root>/patches_factory`; else Surge XT's own data folder, as before. The user patch
folder is unchanged. The plugin only reads the library's folder (installed libraries are read-only
hard links into Oscairn's hash store). At run time the plugin needs no factory folder: `preset-load`
and `prepare_preset` get each patch's full path from the host (the location discovery declared),
so nothing there calls `library_root`.

Tests (`presets`): an indexer whose `oscairn.assets` (draft 2, `resolve` null, as Oscairn's) gives a
temporary folder: the provider declares `<folder>/patches_factory` as its factory location, asking
for `org.oscairn.analog-factory 1.0.0` with the indexer as owner; the same folder written but the
answer false: it declares Surge XT's folder (as a storage built the same way finds it; none on this
machine, which has no Surge XT installed); `SURGE_DATA_HOME` set: the overridden folder, though the
library answers. Seen failing (one at a time): without the lookup (the first two), using the
library's answer when it is false (the second), the library before the override (the third).

*Oscairn side* (Oscairn `8a8fcc6`, built from a `git archive` export outside the repositories; the
plugin a dev build of this commit's tree): the library's files packed with `oscairn-sign
asset-package` (644 files: 641 patches and the three notices) and installed into a scratch data
folder through `Session::installArchive` (a scratch program; the scan helper doesn't install), as
`AssetLibraries/org.oscairn.analog-factory/1.0.0/` (read-only, hard links; every file's path,
SHA-256 and size as the manifest given with the files). `oscairn-plugin-scan --presets` then lists
641 factory patches, every one from the library's folder; with an empty data folder, none (no Surge
XT here); with the plugin as at 0.4.0, none.

The plugin is 0.4.1 (with this entry): it needs `org.oscairn.analog-factory` 1.0.0 or later for its
factory presets. Nothing changes for the converter (its nodes carry patch data, not preset paths).

**Rich views (2026-11-07).** Oscairn's `oscairn.ui` draft 4 (Oscairn `ed073e7` to `4961c35`) lets
a plugin say which of its parameters make an envelope, a key or velocity map or an FM operator
view, which Desktop and Panel draw above the parameters' tiles and edit by setting them. Commits:
`e00f087` the vendored headers at Oscairn `2885698` (`ui.h` draft 4, `memory.h` comments only; the
same headers as `4961c35`), `962d9cc` the views, then version 0.4.2 with this entry.

*What is declared,* as Oscairn's porting guide gives for this plugin ("Rich views: what each engine
should declare"): scene A's amp envelope as view 1 and its filter envelope as view 2, the
parameters that already carry their roles, each stage its named field (Attack, Decay, Sustain,
Release; index 0). The host draws each from zero: the attack to full, the decay to the sustain
level, held, the release to zero, positions by each parameter's range (Surge's times are log2
seconds, so the time axis is logarithmic). The stages' shapes and the envelope's mode (digital or
analog) stay tiles. No FM operator view, so `algorithm` is null. The roles are unchanged.
The plugin offers draft 4 only: a draft 3 host's hint is smaller than the one draft 4 fills.

*Decisions for Jacob* (the guide leaves these open): (1) no `DELAY` field: the scene's key-on
delay is a Part setting on the reference's screens, not an envelope stage, so it stays a tile;
(2) the voice LFOs' envelopes (delay, attack, hold, decay, sustain, release) are in no view,
though section 3 maps the reference's pitch EG to a voice LFO in envelope mode: no LFO is fixed
as the pitch EG (the converter doesn't map it yet), so a view on one would draw an LFO's
envelope as often as a pitch EG.

Tests (`ports and parameters`, `rich views`): the thirteen hints' roles, views, fields and
indexes; the views' parameter values follow a flush (a drag), automation in process (under the
allocation guard, with a note playing) and a state load; the extension is found as draft 4 and
not draft 3. Seen failing: with the old headers (draft 3), and without the views (no parameter in
a view).

*Oscairn side* (Oscairn `2885698`, built from a `git archive` export; the plugin a dev build of
`962d9cc`'s tree): Oscairn's plugin host keeps the eight fields, in the engine and in a worker (a
scratch program over `loadPlugin` and `loadPluginInWorker`), and a value set through the host
reaches the view's parameter in both; `oscairn-worker-host-test` with the plugin (its parameters
and hints, views included, cross the worker; identical audio and events); `oscairn-plugin-check`
in the engine and with the plugin in a worker. Oscairn's own view test (`desktop-gaps`) runs only
against its test plugin, so it wasn't run here.

The plugin is 0.4.2 (with this entry): current Oscairn draws its two envelopes. Nothing changes
for the converter. The commits with this entry were gated after it was written: their results are
not recorded here.

**Envelope times (2026-11-07).** Oscairn's `oscairn.ui` draft 5 (Oscairn `b1fcb88` to `cf63cd6`)
draws an envelope's stages by time, from an optional `seconds()` call or, without it, from value
texts ending in `ms` or `s`. The Oscairn session saw this plugin's tiles show raw values (Attack -8,
Release -5) and its curve spaced by parameter position. The plugin's value texts already carry
units through CLAP ("62.5 ms", "1.00 s", Surge's own `get_display`), and an Oscairn `cf63cd6` host
reads all six stage times from them in the engine and in a worker with 0.4.2's plugin; the raw
values and the spacing were the host's (drawing by time came with draft 5, and `cf63cd6` makes
the tiles show value texts that arrive after the page is drawn). The texts are unchanged. Commits:
`cebae97` the vendored headers at Oscairn `cf63cd6` (`ui.h` draft 5 only), `5a1ee2f` `seconds()`,
then version 0.4.3 with this entry.

*What it gives:* the amp and filter envelopes' attack, decay and release, 2^value seconds
(`ct_envtime`), the shortest value 0 as Surge displays it (the digital attack skips the stage
there; a decay or release at -8 runs about 3.9 ms, which the display also shows as 0). The sustains
are levels and give none. A tempo-synced stage still gets its unsynced time (a code shortcut).

Tests (`rich views`): at 33 values across each stage's range `seconds()` is the time its value
text shows, within the text's rounding, and the sustains give none; the extension is found as
draft 5 and not draft 3. Seen failing: with the draft 4 header, without `seconds()`, and with plain
2^value (3.9 ms where Surge shows 0).

The plugin is 0.4.3 (with this entry). Nothing changes for the converter or for native Surge. The
commits with this entry were gated after it was written: their results are not recorded here.

**Stepped value texts (2026-11-08).** Oscairn's Panel showed repeated choice labels (Attack Shape
"Convex | Linear | Linear", Envelope Mode "Digital | Digital"). A test of every step of every int
parameter against Surge's own display for that int found those texts already right: the envelope
shapes and modes, filter types and subtypes and oscillator types gave Surge's text at every step,
so the repeats come from the host side (by its code, not reproduced: an Oscairn editor that asks for each step's text in turn
and shows the parameter's last text for a step whose reply has not come). Two plugin faults it did
find, fixed in `db55981`: every step of Keytrack Root read as the current key, because
`get_display(true, value)` shows the current value for key names (the text now comes from a copy
of the parameter holding the int, as Surge's display has it); and `text_to_value` read nothing back
for the choice parameters (Surge has no parse for them) and truncated key names ("D-1" as C#-1). A
stepped int's own step texts now read back as those steps first, then Surge's parse.

Tests (`stepped value texts`): 93 int parameters, 1223 steps, each with Surge's text and read back as
a value with that text; the stepped ones cover Surge's ints, the others are 0..1. Seen failing
without the change (127 wrong Keytrack Root texts, 569 steps reading nothing back) and with only
the `value_to_text` half (some key names read back a semitone low).

The plugin is 0.4.4 (with this entry). Nothing changes for the converter, the sound or native Surge.

**Assets draft 3 (2026-11-09).** Oscairn's `oscairn.assets` draft 3 (oscairn-clap `7b5c4e0`) adds
`create()` to the host table: a plugin hands the host a finished file and gets back an asset
reference (its library, path and SHA-256) to keep in its state. The vendored headers moved to
`oscairn/oscairn-clap` at `7b5c4e0` (they left the Oscairn repository's `core/clap-ext`); `ui.h`
draft 5, `sound-contexts.h` draft 5, `sound-snapshots` draft 4 and `voice-budget` draft 1 are
unchanged in what they declare, so nothing else moves. The plugin asks the preset-discovery
indexer for `OSCAIRN_EXT_ASSETS`, which is now draft 3: the table's `resolve` and `library_root`
stay its first two members and current Oscairn answers drafts 1, 2 and 3, so the factory-patch
lookup works as before and `create()` (recording, edited wavetables) is unused for now. A host
answering only draft 2 returns nothing for the draft 3 ID and the provider falls back to Surge
XT's own data folder, as it does when the library is not installed. Tests (`preset discovery`):
the indexer's `library_root` still decides the declared factory folder.

The plugin is 0.4.5 (with this entry). Nothing changes for the converter, the sound or native Surge.

**No user folders (2026-11-11).** Opening a Performance with the plugin made macOS ask for access
to the Documents folder: every synthesizer the plugin made (and the preset provider's storage)
found Surge's user folder, `~/Documents/Surge XT` (`calculateStandardUserDataPath`), read Surge's
user defaults and MIDI defaults from it, scanned its FX presets and FX chains, read the user data
folder override in `~/Library/Application Support/Surge Synth Team/Surge XT`, looked for Surge XT's
data folder in `~/Library/Application Support/Surge XT`, and the provider declared the user patch
folder in Documents as a preset location. Under Oscairn a user's sounds are the plugin's state,
saved by the host, and factory content comes only from an asset library, so Surge's own folders
don't exist for the plugin. `SurgeStorageConfig::findFolders` (default true, so Surge XT is
unchanged) set false finds no data or user folder: `datapath` stays empty unless
`SURGE_DATA_HOME`/`PIPELINE_OVERRIDE_DATA_HOME` gives it, every user path stays empty, there is no
user defaults provider (each user default reads as Surge's built-in value, an update does
nothing), and the FX preset scans and the MIDI defaults file are skipped. The plugin's
synthesizers and its preset provider use it. The provider declares only the factory patches: the
environment override, else `org.oscairn.analog-factory` through `library_root`, else nothing (it no
longer falls back to Surge XT's data folder in `/Library/Application Support`). Surge's user
defaults that touch the sound (smoothing, pitch smoothing, MPE bend range) are now always Surge's
defaults in the plugin, whatever a user set in Surge XT.

Test: `tests/privacy-check.sh` runs `oscairn-analog-privacy-host` (`tests/privacy-host.cpp`, which
opens the bundle as Oscairn does: an instance made, initialized and activated, a note, preset
discovery with every declared location listed and its presets' metadata read, a preset loaded, a
note, the state saved and loaded into a fresh instance, a note) with HOME and
`CFFIXED_USER_HOME` (which `NSFileManager` reads) set to a fresh folder whose Documents, Desktop,
Downloads, Music, Movies, Pictures and `Library/Mobile Documents` are symbolic links. On macOS
it runs under `sandbox-exec`, killed at its first access to any of them or to the real home's
(resolving the link counts, so a look for a missing file is caught); everywhere it fails if
anything under HOME changed or appeared. In CI after the build. Seen failing without the change
(killed at instance creation).

The plugin is 0.4.6 (with this entry). Nothing changes for the converter or native Surge.

**Where this stopped** (thirteenth session). HPF24 stays on the state-variable HP 24 dB: Clean
passes everything but index 560 (at every rate), Driven fails above 48 kHz; a decision for Jacob
again (register entry 1). The amp EG decay table holds from index 72; attack and release below
100 and 112 don't pass with the 2 ms floor (entry 13). The plugin lists Surge's factory and user
patches through CLAP preset discovery and loads them with preset-load, and offers native sound
contexts (up to 8 sounds, a synthesizer each, 36 MiB declared a sound) with snapshots prepared
beside for instant Performance changes. Self sync and the wave
shaper have measured models (a slowing slave, a static curve plus a low highpass), not built.
Next: Jacob's HPF24 decision; self sync probes between raw 24 and 96 at two notes, and shaper
probes at another note, to fix the window and highpass; resonance above 96 (a new filter);
sharing the storage's tables between the sounds' synthesizers if more sounds are wanted.

**Where this stopped** (twelfth session). HPF24 stays on the state-variable HP 24 dB; HP 24 dB
Driven was refitted and fails at 88.2-192 kHz because it saturates; Clean is linear and doesn't
depend on the rate but matches less well at low cutoffs. Decision for Jacob: Driven with
88.2-192 kHz failures, Clean (refit and check), or the state-variable filter as before. Decided:
64 kHz out, tolerances accepted, 2 ms short-envelope floor (tables not done), version bumps.

**Where this stopped** (eleventh session). The cutoff tables hold above 44.1 kHz through the
filter cutoff rate match (off by default, per patch; fitted at 48, 96 and 192 kHz, interpolated
between, checked at 48, 88.2, 96, 176.4 and 192 kHz), except HPF24 at the top (a decision for
Jacob, register entry 1); the amp level table holds the engine's level only (the converter stops
adding +4.13 dB); note shift and bend ranges reach 127 semitones (a deliberate difference); the
folder's departures above 96 are measured (not memoryless, aliasing). Known differences are listed
in section 11. Next: the HPF24 decision; denser rate fits if interpolated rates matter (64 kHz
fails LPF24 at the top); resonance above 96 (a new filter); the folder above 96 and Hard, self sync
and the wave shaper; short envelopes; moving key-on delay, segment drift and unison out of the
large engine files.

**Where this stopped** (tenth session). The saturators now match the reference's curve
(algebraic, offsets for every filter type), the noise tone has a tilt filter and mapping, the
portamento "2" modes have an octave limit, amp velocity sensitivity is mapped to +255, the pitch
LFO delay at 2-6 uses measured values, and Part velocity depth and offset are measured. The
reference-matching engine options are per patch, off by default, and plain patches play in the
plugin exactly as in stock Surge; voice LFO releases reach 2^8 s per patch. A test merge of
upstream `main` is clean. Part and Performance Volume curves and ageing are measured. Next:
re-export the amp level table without the Part Volume (+4.13 dB); the converter mapping notes to
set the four options for converted Parts (the converter repository is not ours); folding Part velocity depth/offset into the sensitivities; resonance above 96 (a new
filter); the folder above 96 and Hard, self sync and the wave shaper (no model yet); short
envelopes (waiting for the decision on the floor for averaged checks); moving key-on delay,
segment drift and unison out of the large engine files to ease upstream merges.

**Where this stopped** (ninth session). The amp velocity law, key-on delay (engine, table,
note-end checks) and the portamento time table are in, and cutoff drift and the pitch LFO start
phases have plugin checks. Next: the portamento "2" modes' octave jump and Portamento Time
122-127 (an engine limit), negative amp velocity sensitivity, the noise tone's tilt filter,
averaged short-envelope checks once the floor is decided, the remaining milestone 1/2 curve
items (folder above 96 and Hard, saturator at high drive, filter saturator offsets for other
types, resonance above 96), and self sync and the wave shaper.

**Where this stopped** (eighth session). The Linux CI jobs run locally in Docker while Actions
is off; oscillator EG tables are in; a first-block bug in the plugin was found and fixed, which
exposed Surge's analog attack overshoot (now clamped in the plugin) and needed the amp EG decay
table refitted; unison and drift have plugin checks against the reference; pitch LFO start
phases are mapped. Next: a tilt filter for the noise tone; averaged short-envelope probes;
self sync and the wave shaper (still no model); the remaining milestone 1/2 items (folder above
96 and Hard, saturator curve at high drive, filter saturator offsets for other types,
resonance above 96); milestone 4's key-on delay (not started), the portamento time table against Surge and the
octave limit, and the amp velocity law (both measured, see above).

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

## 11. Known differences from the reference

One entry per known difference between the engine (through the plugin, with a converted
Part's options on) and the reference, kept up to date as work lands. Fields: what differs;
measured size and where; cause if known; status (**deliberate**: a decision, **unresolved**:
open); how to reproduce; where in the code. Sessions refer to section 10. Rule from Jacob
(eleventh session): no ceiling on pitch shift or transpose is added even where the reference
has one; any such reference limit found is recorded here as a deliberate difference. None has
been measured so far, and the fork adds no output pitch clamp (the portamento octave limit
only moves where a glide starts, below).

1. **HPF24 shape at low cutoffs, and near index 562.** HPF24 is HP 24 dB Clean (type 5, subtype 2,
   resonance 0.05) since the fourteenth session. Its shape error at the table's cutoff is larger
   than the state-variable HP 24 dB's (Standard, before) from index 352 to 848: 0.09-0.68 dB
   against 0.04-0.73 (mean 0.45 against 0.15 at 44.1 kHz); from 880 it is smaller, 0.13-0.31 dB at
   every rate against 0.32-2.52 at 44.1 kHz and up to 3.61 at 192 kHz. Its best match sits close
   to the tolerance on both sides of index 562: +38 to +42 cents at 560 and -23 to -35 at 564 over
   the six rates (tolerance 50). Cause: Clean's damping is capped at Q 0.5, so its knee is softer
   than the reference's and its shape matches less well where the knee is inside the measured
   harmonics; at 562 the shape measure's harmonic 2 leaves the 40 dB range (560: -38.6 dB below the
   largest, 564: -40.7), and the best Clean match falls by 0.7 semitone there (560 -6.65, 564
   -7.34), which a non-decreasing table can't follow, so the table sits between the two. Unresolved
   (no filter has been found that matches the whole range at every rate: the state-variable filter
   failed above index 937 at 48-192 kHz, by up to 1174 cents, because its damping cap gives the
   44.1 kHz filter a resonance near the top that higher rates don't; Driven failed at 88.2-192
   kHz because it saturates inside its loop; sessions eleven to thirteen). Reproduce: plugin tests
   (`oscairn-analog-tests`, "fitted tables" at each rate); measurement side, the fit with
   `fit.py`'s break and non-decreasing options (section 10, fourteenth session). Code:
   `FilterCutoffRateMatchTables.h` (HPF24's tables), `tests/FittedTables.h` (`cutoffHp24`),
   `testFittedTables`.
2. **Cutoff between and outside the fitted rates.** The rate match is fitted at 48, 96 and 192
   kHz and interpolated linearly in log rate between them (and between 44.1 kHz, unchanged, and
   48); above 192 kHz the 192 kHz table is used; below 44.1 kHz the cutoff is unchanged. Checked
   at the interpolated 88.2 and 176.4 kHz (all pass). At 64 kHz (not a checked
   rate) LPF24 fails at index 948 (+58 cents) and 956 (+130): the interpolation between 48 and 96
   kHz is too coarse near the state-variable filter's clamp. Below 44.1 kHz the filters clamp
   lower than at 44.1 kHz, so the top can't match. 64 kHz: **deliberate** (Jacob, twelfth session:
   it needn't be covered; not in the tests). Below 44.1 kHz: unresolved.
   Code: as 1.
3. **Cutoff tables beyond the OB-Xd clamp at 96 and 192 kHz** (HPF18/12/6 above cutoff 60,
   16 kHz): engine-against-engine shape error up to 0.93 dB (96 kHz) and 1.10 dB (192) after
   matching (the OB-Xd clamps at 22 kHz
   at every rate, so its top responds differently at 96 kHz); the reference checks pass.
   Unresolved, small. Reproduce: `oscairn-analog-rate-match fit HPF18 96000`.
4. **Resonance above index 96.** No Surge filter matches both probed cutoffs: LP 24 dB 0.9-1.3 dB
   RMS (cutoff 512) and 1.0-3.5 dB (768), the ladders 2.1-4.1 dB, Diode 4-10 dB. Cause: the
   reference behaves like a ladder (pass band -14 dB at 255, narrow peak +28 dB at 768) with LP 24
   dB's knee at low resonance; Surge has no such filter. Unresolved (needs a new filter). Reproduce:
   fourth-session LPF24 resonance 0-255 probes at cutoffs 512 and 768, `scripts/res_search.py`
   (local). Code: none (resonance maps to the chosen filter's resonance).
5. **Wave folder above amount 96, and Hard.** Best static curves 3.7-5.4 dB RMS (harmonics 2-12)
   above 96, Hard 1.1-4.0 dB. Cause (eleventh session): above 96 the reference isn't memoryless
   (output at input phase theta against pi - theta -12 to -13 dB of the signal, against -20 to
   -29 dB up to 96; harmonic phases 6-7 degrees RMS off any static curve) and isn't band-limited
   (energy between harmonics: Soft 255 -44 dB, Hard 192 -23, 224 -11, 255 -4 dB); the engine's
   pre-filter shaper runs oversampled. Unresolved. Reproduce: `m2` probes `wf0_*`, `wf1_*` (note
   48), `scripts/foldfit.py` (local); the memoryless and residual measures were scratch scripts
   (period fit with `shaper.period_fit`). Code: pre-filter shaper (`a1830c0`), sst waveshapers.
6. **"Analog" oscillator: self sync and the wave shaper.** Not built; Saw1, Saw2, square,
   triangle and sine map to Modern. Self sync as a restarted slave with fades fits harmonics 1-40
   5-8 dB RMS with ratios that don't follow the nominal ones; the shaper isn't one static curve
   on a sine (fifth harmonic 17 dB under the third at 32, against about 39 dB for sin(g x)).
   Thirteenth session (models found, not built): self sync fits a slave whose phase advances at
   the nominal ratio (25 cents a step holds) times a window that slows it to a stop and restarts
   it once per master period, its amplitude the window to a power: sine and triangle -35 to -39
   dB at raw 24, -22 to -26 dB at 96; saws and squares -7 to -22 dB. The shaper on a sine is a
   static curve followed by a highpass near 0.1 x f0 (13 Hz at note 48): -30 to -38 dB at 48-128
   against -20 to -34 dB without the highpass. Unresolved. Reproduce: `m3b` sync probes,
   fifth-session shaper probes (`scripts/m2b.py`), thirteenth-session fits (section 10). Code: none.
7. **Ageing.** The reference's ageing shifts the cutoff by about +0.16 to -0.05 semitone and the
   pulse width (duty 0.2517-0.2545) for the whole Part, no spread between loads or notes.
   Deliberate: folded into the converter's cutoff and pulse width, no engine hook. Reproduce:
   `m10d`, `m3a`/`m3e`.
8. **Saturators: low-level odd harmonics.** The reference has a -56 dB third harmonic at oscillator
   level 64 that no static curve gives; at high drive the algebraic curve is within 0.5 dB.
   Unresolved, small. Reproduce: `m2` sine probes, `scripts/satcheck.py` (local). Code:
   `saturatorAlgebraicCurve`, `QuadFilterChain.cpp` `saturate`.
9. **Drift: the reference's mean pitch offset.** At Voltage Drift 127 the reference's notes sit
   +11.9 cents high on average; the engine's segment drift has no mean offset (spread matches
   within 20 %). Unresolved (the converter could add it to the tuning). Reproduce: the sixth
   session's drift probes, plugin test "drift against the reference". Code: segment drift in
   `SurgeVoice.cpp`.
10. **Part velocity sensitivity depth and offset where they clamp.** v' = v x depth / 64 + 2 x
    (offset - 64), clamped to 0-127; where it clamps (for example depth 96: every velocity from
    64 up plays as 127) Surge has no equivalent. Unresolved (below the clamps the converter folds
    it into the sensitivities). Reproduce: `m10c`.
11. **Portamento Time 122-127.** The reference glides an octave in 13.1 s at 122, 18.5 s at 124
    and over 28 s at 127; the engine clamps portamento at 2^4 s (the plugin's range stops at
    2^3.99 s, 0.8 x 2^3.99 = 12.7 s an octave), so 122-127 all play at that. Unresolved (an engine
    limit). Reproduce: `m9a`/`m9c`/`m9g`/`m9h`, table `portamentoTime` (valid 1-121). Code: the
    plugin's portamento range in `OscairnAnalog.cpp`, the engine's portamento clamp. The portamento octave limit (glides over an octave start an octave from
    the target, the "2" modes) is a reference behaviour reproduced on purpose, off by default; it
    limits where a glide starts, not the pitch reached.
12. **Fits checked only against the renders they were fitted to.** The saturator curve and offsets
    (`m2`), the filter saturator offsets for the other filter types (`m10a`), the noise tilt
    knots (checks between knots at 24-104 only, none between 112 and 127), the pitch LFO delay at
    2-6, the amp velocity k above +192 (+192 to +224 not probed). Unresolved until independent
    probes exist.
13. **Short envelopes.** Jacob decided a 2 ms floor for averaged short-envelope checks (twelfth
    session). Thirteenth session: the amp EG decay table holds from index 72 (averaged checks 76
    and 80 pass at every checked rate, worst -1.56 ms). Not committed: decay 40-71 (60, 64, 68
    are 2.4-2.6 ms short: 14.15 / 16.33 / 18.78 ms against 16.78 / 18.70 / 21.19); attack 40-99
    (passes at 44.1 and 48 kHz, 6 of 6, but at 88.2-192 kHz the engine's short attacks run long:
    index 48 10.2 ms against 7.66 at 176.4 kHz, 80 29.0-29.3 against 26.76 from 88.2 kHz);
    release 40-111 (68: 19.95 against 17.16 ms; and the long table's 108 is 8.4 % off, so no
    contiguous range reaches 112). Below index 40 there are no averaged probes. Unresolved.
    Reproduce: `scripts/egshort_fit_ref_2ms.py` with the sweep `surge/egshort_ref`
    (`scripts/surge_eg_short_ref.py`, the reference-matching options on). Code: the amp EG.
14. **Surge 1.x patches in the plugin.** A patch of revision below 2 sets an LFO's sustain from
    whether its decay equals the parameter maximum; the plugin's voice LFO decay maximum is 8,
    not 5, so such a patch could load differently in the plugin than in Surge. Deliberate (no such
    patch to test). Code: `SurgePatch.cpp` loader, plugin ranges in `OscairnAnalog.cpp`.
15. **Sample rate of the reference.** Every reference render is at 44.1 kHz. Deliberate (Jacob,
    eleventh session): the 44.1 kHz behaviour is the target at every rate.
16. **Note shift and pitch bend range beyond the reference's.** The reference stops at +-48
    semitones of note shift (Part and Performance each) and bend ranges of -48 to +24; the plugin's
    scene pitch reaches +-127 semitones, its bend up range 127 and bend down range -127 to +127.
    Deliberate (Jacob's decision: no artificial pitch limits); documented values map one to one.
    Reproduce: plugin test "note shift and bend ranges". Code: `normalizePatch` in
    `OscairnAnalog.cpp`.
17. **Negative bend up range.** The reference's Upper bend range can be negative (bending up
    lowers the pitch, to -48); Surge's patch loader reads a bend up range outside 0-255 as a legacy
    float value and clears both ranges, so the plugin's bend up range stays 0-127 and negative
    Upper values have no equivalent (negative Lower values do: the bend down range goes to -127).
    Unresolved (needs a change to Surge's loader). Code: `SurgePatch.cpp` (`pbrange_up.val.i &
    0xffffff00`).
18. **Amp EG decay at 64 kHz.** At 64 kHz (not a checked rate) the amp EG decay at index 116 is
    4.6 % long (0.1283 s against 0.1226; tolerance 2.5 % or 5 ms); at 48, 88.2, 96, 176.4 and
    192 kHz every envelope table passes. Deliberate (Jacob, twelfth session: 64 kHz needn't be
    covered). Reproduce: the plugin
    tests' fitted tables at 64000 Hz.
19. **Voice stealing under a voice budget.** Once the host sets a voice budget
    (`oscairn.voice-budget`: a limit or an entry), voices go in the Oscairn SDK's order instead of
    Surge's: a sound's full Polyphony Limit releases the voice that order gives within that sound
    (released before held, the quietest, then the oldest), where Surge releases the voice released
    longest ago, else the oldest held; and a voice given up past the host's limits fades linearly
    over 3 ms rounded up to whole 32-sample blocks rather than by Surge's uber-release. Without a
    budget Surge's stealing is unchanged (checked against stock Surge in the plugin test).
    Deliberate (Jacob: use the SDK's order; accepted by Jacob 2026-11-07). Reproduce: plugin test "voice budget". Code: the
    voice budget section of `OscairnAnalog.cpp`, `SurgeSynthesizer::softkillVoice`,
    `SurgeVoice::fadeOut`.
20. **Voices shared between sounds.** The sounds of one instance share one store of 128 Surge
    voices, each taking at most 64 at once (stock Surge: 64 for its one patch). Past 128 voices
    across the sounds a new voice isn't started, as stock Surge does past its 64; one sound alone
    plays as stock. Deliberate, accepted by Jacob 2026-11-07 (he asked for more sounds per instance; the voice budget limits
    voices before the store does). Reproduce: plugin test "sound contexts" (the stock comparison
    beside another sound's 64 voices). Code: `SurgeSynthesizer::getUnusedVoice`, `SharedVoices` in
    `OscairnAnalog.cpp`.
21. **A bus change moves a sound's held notes.** Oscairn's porting guide says voices keep the output
    port they started on. Here a sound's Output Bus change moves the whole sound, notes held across
    it included, crossfading over 3 ms from the old port to the new; notes struck after it play on
    the new port as the guide says. Cause: Surge renders a sound's voices together through the
    scene's stateful stages, so a voice can't keep a port of its own without a scene chain per bus.
    Oscairn sets a part's bus before the part plays (when the part is made, and at a Performance
    change's cue), so its parts don't hear it. Deliberate, accepted by Jacob 2026-11-07. Reproduce: plugin test "sharing" (the
    bus change check). Code: the Output Bus in `renderBlock` and `render`, `OscairnAnalog.cpp`.
