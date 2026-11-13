/*
 * Oscairn: a faster path for the Modern oscillator, chosen per voice when the patch turns on
 * the engine option SurgeStorage::modernOscillatorFast (saved as "modernOscillatorFast").
 *
 * Surge computes each sample of the oscillator from three evaluations of a generator
 * polynomial and a second difference over them, in double precision, one sample at a time. This
 * path computes the same second difference in closed form (the generator's second derivative
 * averaged under the difference's triangular kernel, which is exact for a piecewise
 * polynomial generator and has none of the cancellation the numerical difference has in single
 * precision) and does it for four samples at a time in SIMD lanes, in single precision. The
 * phase and its increment are carried and laid out per sample in double, as they are in
 * ModernOscillator.cpp, so the oscillator's pitch, unison, drift and pulse width behave as there.
 *
 * It plays the sawtooth, the pulse and a sine third component. Anything else (a triangle or
 * square third component that sounds, the sub-octave, oscillator FM either way round, sync, or a
 * phase increment past a quarter of the oversampled rate) is played by the code in
 * ModernOscillator.cpp, block by block: the state the two share (phases, increments, level
 * lags, the previous sample and the sync bookkeeping) is kept the same by both, so a block
 * played by either continues the other's. After sync has been on in a voice, the voice stays on
 * ModernOscillator.cpp, whose sync offsets the closed forms do not carry.
 *
 * Surge XT is released under the GNU General Public Licence v3 or later (GPL-3.0-or-later).
 */

#include "ModernOscillator.h"
#include "ModernOscillatorFastKernels.h"

namespace
{
// The lags' one-pole rates, as ModernOscillator sets them (the pulse width's in init(), the
// others the default), as the powers r^j the closed form of a lag needs.
constexpr double defaultLagRate = 0.004, pwidthLagRate = 0.001;

struct LagPowers
{
    double fast[BLOCK_SIZE_OS + 1], slow[BLOCK_SIZE_OS + 1];
    LagPowers()
    {
        for (int j = 0; j <= BLOCK_SIZE_OS; ++j)
        {
            fast[j] = std::pow(1. - defaultLagRate, j);
            slow[j] = std::pow(1. - pwidthLagRate, j);
        }
    }
};
const LagPowers lagPowers;

// The lag's value at each of the block's samples, scaled, from the closed form of a one-pole
// lag, v_j = t + (v_0 - t) r^j; leaves it at its value after the block as the sample-by-sample
// loop does. dst may be null when nothing reads the values.
template <typename Lag, typename T>
void rampLag(Lag &l, const double *r, T *dst, double scale)
{
    const double t = l.target_v, a = l.v - t;
    if (dst)
        for (int k = 0; k < BLOCK_SIZE_OS; ++k)
            dst[k] = (T)((t + a * r[k]) * scale);
    l.v = t + a * r[BLOCK_SIZE_OS];
}
} // namespace

void ModernOscillator::initFast(bool is_display)
{
    fast = false;
    syncSeen = false;
    fmScene = nullptr;
    fmOsc = -1;
    if (is_display || !storage->modernOscillatorFast)
        return;
    // Found in the patch's scenes, as a voice's oscillators are; one that is not stays on the
    // other path.
    auto &patch = storage->getPatch();
    for (int sc = 0; sc < n_scenes; ++sc)
        for (int k = 0; k < n_oscs; ++k)
            if (oscdata == &patch.scene[sc].osc[k])
            {
                fmScene = &patch.scene[sc];
                fmOsc = k;
                fast = true;
            }
}

bool ModernOscillator::processFast(float pitch, float drift, bool stereo, bool FM, float FMdepth)
{
    using namespace Surge::Oscillator::Fast;

    const bool sawActive = !oscdata->p[mo_saw_mix].deactivated;
    const bool pulseActive = !oscdata->p[mo_pulse_mix].deactivated;
    const bool thirdActive = !oscdata->p[mo_tri_mix].deactivated;
    const bool sineThird = thirdActive && multitype == momt_sine;

    // Everything below that returns false does so before this block changes any state.
    const float syncParam = std::max(0.f, localcopy[oscdata->p[mo_sync].param_id_in_scene].f);
    if (sync.v != 0 || sync.target_v != 0 || syncParam != 0)
        syncSeen = true;
    if (FM || syncSeen || (cachedDeform & mo_subone))
        return false;
    // An oscillator another takes as its FM source stays on the other path: the modulation
    // multiplies the error by up to 16 cycles per unit of the source.
    const int fmRouting = fmScene->fm_switch.val.i;
    if ((fmOsc == 1 && fmRouting != fm_off) || (fmOsc == 2 && fmRouting >= fm_3to2to1))
        return false;
    const float thirdParam = localcopy[oscdata->p[mo_tri_mix].param_id_in_scene].f;
    if (thirdActive && !sineThird &&
        (thirdParam != 0 || std::fabs(trimix.v) > 1e-9 || std::fabs(trimix.target_v) > 1e-9))
        return false;

    float ud = oscdata->p[mo_unison_detune].get_extended(
        localcopy[oscdata->p[mo_unison_detune].param_id_in_scene].f);
    float absOff = 0.f;
    if (oscdata->p[mo_unison_detune].absolute)
    {
        absOff = ud * 16;
        ud = 0;
    }

    // The sine's closed form covers a stencil that spans two of its corners at most: a phase
    // increment up to a quarter of the rate. Bound this block's by the pitch plus all the
    // detune and drift it can have (the drift is a small noise, under 4 here), and the last
    // block's.
    if (sineThird)
    {
        double bound = pitch_to_dphase_with_absolute_offset(
            pitch + std::fabs(ud) + 4 * std::fabs(drift), std::fabs(absOff));
        for (int u = 0; u < n_unison; ++u)
            bound = std::max(bound, dspbase[u].getTargetValue());
        if (bound > 0.24)
            return false;
    }

    // sync is off (its lag at zero), but the lag's first call must be made as it is there
    sync.newValue(0);
    const double basePitch = pitch;
    for (int u = 0; u < n_unison; ++u)
    {
        auto lfodetune = drift * driftLFO[u].next();
        const double target = std::min(
            0.5, pitch_to_dphase_with_absolute_offset(basePitch + lfodetune + ud * unisonOffsets[u],
                                                      absOff * unisonOffsets[u]));
        dpbase[u].newValue(target);
        dspbase[u].newValue(target);
    }
    // the sub-octave's increments are not played here but follow the pitch as they do there
    auto subdt = drift * driftLFO[0].val();
    subdpbase.newValue(std::min(0.5, pitch_to_dphase(basePitch + subdt)));
    subdspbase.newValue(std::min(0.5, pitch_to_dphase(basePitch + subdt + sync.v)));

    sawmix.newValue(sawActive * 0.5 *
                    limit_range(localcopy[oscdata->p[mo_saw_mix].param_id_in_scene].f, -2.f, 2.f));
    sqrmix.newValue(
        pulseActive * 0.5 *
        limit_range(localcopy[oscdata->p[mo_pulse_mix].param_id_in_scene].f, -2.f, 2.f));
    trimix.newValue(thirdActive * 0.5 * limit_range(thirdParam, -2.f, 2.f));
    pwidth.newValue(2 * limit_range(1.f - localcopy[oscdata->p[mo_pulse_width].param_id_in_scene].f,
                                    0.01f, 0.99f));
    fmdepth.newValue(16 * FMdepth * FMdepth * FMdepth);

    const bool sawOn = sawmix.v != 0 || sawmix.target_v != 0;
    const bool sqrOn = sqrmix.v != 0 || sqrmix.target_v != 0;
    const bool sinOn = sineThird && (trimix.v != 0 || trimix.target_v != 0);

    constexpr int n = BLOCK_SIZE_OS;
    static_assert(n % 4 == 0, "the oscillator's block is a whole number of SIMD registers");
    alignas(16) float aSaw[n], aSqr[n], aSin[n], aP[n], aPo[n], aD[n], aRd[n], yL[n], yR[n],
        last[4];
    double aPw[n];
    rampLag(sawmix, lagPowers.fast, sawOn ? aSaw : nullptr, 1.);
    rampLag(sqrmix, lagPowers.fast, sqrOn ? aSqr : nullptr, 1.);
    rampLag(pwidth, lagPowers.slow, sqrOn ? aPw : nullptr, 0.5);
    rampLag(trimix, lagPowers.fast, sinOn ? aSin : nullptr, 1.);
    rampLag(fmdepth, lagPowers.fast, (float *)nullptr, 1.);
    std::fill(yL, yL + n, 0.f);
    std::fill(yR, yR + n, 0.f);

    for (int u = 0; u < n_unison; ++u)
    {
        /*
         * Phase and increment at each sample: the increment ramps linearly over the block, so
         * sample j is at the block's start phase plus j d + j (j - 1) / 2 dd. In double, as
         * the accumulating loop is, and only its fraction goes to float.
         */
        const double p0 = sphase[u], d0 = dspbase[u].v, dd0 = dspbase[u].dv;
        for (int j = 0; j < n; ++j)
        {
            const double x = p0 + j * d0 + 0.5 * j * (j - 1) * dd0;
            aP[j] = (float)(x - std::floor(x));
            if (sqrOn)
            {
                // the pulse's second saw, in double so a phase near its wrap keeps its precision
                const double xo = x + aPw[j];
                aPo[j] = (float)(xo - std::floor(xo));
            }
            aD[j] = (float)(d0 + j * dd0);
            aRd[j] = 1.f / std::max(aD[j], 1e-10f);
        }
        const double total = n * d0 + 0.5 * n * (n - 1) * dd0, pe = p0 + total;
        sphase[u] = phase[u] = pe - std::floor(pe);
        dpbase[u].v = dspbase[u].v = d0 + n * dd0;
        // the wraps this block (each flips the sync reset), the output at its last sample
        if ((long long)std::floor(pe) & 1)
            sReset[u] = !sReset[u];
        sTurnFrac[u] = 0.0;

        const V gl = vset(mixL[u]), gr = vset(mixR[u]);
        for (int k = 0; k < n; k += 4)
        {
            const V P = SIMD_MM(load_ps)(aP + k), D = SIMD_MM(load_ps)(aD + k),
                    RD = SIMD_MM(load_ps)(aRd + k);
            V res = vset(0.f);
            if (sawOn || sqrOn)
            {
                const V saw = sawClosed(P, D, RD);
                if (sawOn)
                    res = SIMD_MM(mul_ps)(SIMD_MM(load_ps)(aSaw + k), saw);
                if (sqrOn)
                {
                    const V sqr = SIMD_MM(sub_ps)(sawClosed(SIMD_MM(load_ps)(aPo + k), D, RD), saw);
                    res = SIMD_MM(add_ps)(res, SIMD_MM(mul_ps)(SIMD_MM(load_ps)(aSqr + k), sqr));
                }
            }
            if (sinOn)
                res = SIMD_MM(add_ps)(
                    res, SIMD_MM(mul_ps)(SIMD_MM(load_ps)(aSin + k), sineClosed(P, D, RD)));
            if (k == n - 4)
                SIMD_MM(store_ps)(last, res);
            SIMD_MM(store_ps)(yL + k, SIMD_MM(add_ps)(SIMD_MM(load_ps)(yL + k),
                                                      SIMD_MM(mul_ps)(res, gl)));
            SIMD_MM(store_ps)(yR + k, SIMD_MM(add_ps)(SIMD_MM(load_ps)(yR + k),
                                                      SIMD_MM(mul_ps)(res, gr)));
        }
        sprior[u] = last[3];
    }

    std::copy(yL, yL + n, output);
    std::copy(yR, yR + n, outputR);
    if (!stereo)
        for (int s = 0; s < n; ++s)
            output[s] = 0.5 * (output[s] + outputR[s]);
    if (charFilt.doFilter)
    {
        if (stereo)
            charFilt.process_block_stereo(output, outputR, n);
        else
            charFilt.process_block(output, n);
    }
    starting = false;
    return true;
}
