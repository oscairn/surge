/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */
#include "QuadFilterChain.h"
#include "SurgeStorage.h"
#include <vembertech/basic_dsp.h>
#include <vembertech/portable_intrinsics.h>
#include "sst/basic-blocks/mechanics/simd-ops.h"
#include "sst/basic-blocks/dsp/Clippers.h"

namespace mech = sst::basic_blocks::mechanics;
namespace sdsp = sst::basic_blocks::dsp;

#define MWriteOutputs(x)                                                                           \
    d.OutL = SIMD_MM(add_ps)(d.OutL, d.dOutL);                                                     \
    d.OutR = SIMD_MM(add_ps)(d.OutR, d.dOutR);                                                     \
    auto outL = SIMD_MM(mul_ps)(x, d.OutL);                                                        \
    auto outR = SIMD_MM(mul_ps)(x, d.OutR);                                                        \
    SIMD_MM(store_ss)                                                                              \
    (&OutL[k], SIMD_MM(add_ss)(SIMD_MM(load_ss)(&OutL[k]), mech::sum_ps_to_ss(outL)));             \
    SIMD_MM(store_ss)                                                                              \
    (&OutR[k], SIMD_MM(add_ss)(SIMD_MM(load_ss)(&OutR[k]), mech::sum_ps_to_ss(outR)));

#define MWriteOutputsDual(x, y)                                                                    \
    d.OutL = SIMD_MM(add_ps)(d.OutL, d.dOutL);                                                     \
    d.OutR = SIMD_MM(add_ps)(d.OutR, d.dOutR);                                                     \
    d.Out2L = SIMD_MM(add_ps)(d.Out2L, d.dOut2L);                                                  \
    d.Out2R = SIMD_MM(add_ps)(d.Out2R, d.dOut2R);                                                  \
    auto outL = vMAdd(x, d.OutL, vMul(y, d.Out2L));                                                \
    auto outR = vMAdd(x, d.OutR, vMul(y, d.Out2R));                                                \
    SIMD_MM(store_ss)                                                                              \
    (&OutL[k], SIMD_MM(add_ss)(SIMD_MM(load_ss)(&OutL[k]), mech::sum_ps_to_ss(outL)));             \
    SIMD_MM(store_ss)                                                                              \
    (&OutR[k], SIMD_MM(add_ss)(SIMD_MM(load_ss)(&OutR[k]), mech::sum_ps_to_ss(outR)));

// Saturator: tanh plus a square term for asymmetry, scaled back by tanh(drive): the small
// signal gain is about 1 at low drive, and a full-scale input stays at full scale at high
// drive. Then a DC blocker for what the square term adds. With algebraic set, the curve is
// u / sqrt(1 + u^2) in place of tanh(u), scaled back by the same curve at the drive.
inline SIMD_M128 algebraicSigmoid(SIMD_M128 u)
{
    auto one = SIMD_MM(set1_ps)(1.f);
    return SIMD_MM(div_ps)(u, SIMD_MM(sqrt_ps)(SIMD_MM(add_ps)(one, SIMD_MM(mul_ps)(u, u))));
}

inline SIMD_M128 saturate(SIMD_M128 x, SIMD_M128 drive, SIMD_M128 asym, SIMD_M128 &dcX,
                          SIMD_M128 &dcY, SIMD_M128 dcR, bool algebraic)
{
    auto u = SIMD_MM(mul_ps)(x, drive);
    auto t = algebraic ? algebraicSigmoid(u) : sdsp::fasttanhSSEclamped(u);
    auto y = SIMD_MM(div_ps)(SIMD_MM(add_ps)(t, SIMD_MM(mul_ps)(asym, SIMD_MM(mul_ps)(t, t))),
                             algebraic ? algebraicSigmoid(drive)
                                       : sdsp::fasttanhSSEclamped(drive));
    auto out = SIMD_MM(add_ps)(SIMD_MM(sub_ps)(y, dcX), SIMD_MM(mul_ps)(dcR, dcY));
    dcX = y;
    dcY = out;
    return out;
}

// Shaper on the filter inputs, before the filters (a folder, for example): inL and inR are
// the filter 1 and filter 2 inputs (left and right in Wide) from here on.
#define MPreShape                                                                                  \
    auto inL = d.DL[k], inR = d.DR[k];                                                             \
    if (g.PreWSptr)                                                                                \
    {                                                                                              \
        d.PreDrive = SIMD_MM(add_ps)(d.PreDrive, d.dPreDrive);                                     \
        inL = g.PreWSptr(&d.PreWSS[0], inL, d.PreDrive);                                           \
        inR = g.PreWSptr(&d.PreWSS[1], inR, d.PreDrive);                                           \
    }

// Per-sample steps of the saturator and output gain interpolators
#define MStepPost                                                                                  \
    for (int u = 0; u < 2; ++u)                                                                    \
    {                                                                                              \
        d.FDrive[u] = SIMD_MM(add_ps)(d.FDrive[u], d.dFDrive[u]);                                  \
        d.FAsym[u] = SIMD_MM(add_ps)(d.FAsym[u], d.dFAsym[u]);                                     \
        d.FGain[u] = SIMD_MM(add_ps)(d.FGain[u], d.dFGain[u]);                                     \
    }                                                                                              \
    d.ADrive = SIMD_MM(add_ps)(d.ADrive, d.dADrive);                                               \
    d.AAsym = SIMD_MM(add_ps)(d.AAsym, d.dAAsym);

// After filter unit u (state slot dc, the FU index): its saturator while the filter runs, then
// its output gain, which also applies with the filter off.
#define MFilterPost(x, u, dc, active)                                                              \
    if ((active) && g.filterDrive[u])                                                              \
        x = saturate(x, d.FDrive[u], d.FAsym[u], d.dcX[dc], d.dcY[dc], g.dcR, g.algebraicSat);    \
    x = SIMD_MM(mul_ps)(x, d.FGain[u]);

// The stage between the filters and the amp gain: mixer channels routed past the filters join
// the filtered signal, then the amp saturator. Feedback keeps tapping the filtered signal alone.
inline SIMD_M128 preAmpStage(QuadFilterChainState &d, const fbq_global &g, SIMD_M128 x,
                             SIMD_M128 post, int c)
{
    x = SIMD_MM(add_ps)(x, post);
    if (g.ampDrive)
        x = saturate(x, d.ADrive, d.AAsym, d.dcX[4 + c], d.dcY[4 + c], g.dcR, g.algebraicSat);
    return x;
}

#define MPreAmp(x, out, post, c)                                                                   \
    if (g.preAmp)                                                                                  \
        out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(preAmpStage(d, g, x, post, c), d.Gain));

#if 0 // DEBUG
#define AssertReasonableAudioFloat(x) assert(x<32.f && x> - 32.f);
#else
#define AssertReasonableAudioFloat(x)
#endif

template <int config, bool A, bool WS, bool B>
void ProcessFBQuad(QuadFilterChainState &d, fbq_global &g, float *OutL, float *OutR)
{
    const auto hb_c = SIMD_MM(set1_ps)(0.5f); // If this is changed from 0.5, make sure to change
                                              // this in the code because it is assumed to be half
    const auto one = SIMD_MM(set1_ps)(1.0f);

    switch (config)
    {
    case fc_serial1: // no feedback at all  (saves CPU)
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            auto input = inL;
            auto x = input, y = inR;
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, SIMD_MM(and_ps)(mask, x)));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], d.wsLPF, d.Drive);
            }

            if (A || WS)
            {
                d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
                x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(input, SIMD_MM(sub_ps)(one, d.Mix1)),
                                    SIMD_MM(mul_ps)(x, d.Mix1));
            }

            y = SIMD_MM(add_ps)(x, y);

            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, SIMD_MM(sub_ps)(one, d.Mix2)),
                                SIMD_MM(mul_ps)(y, d.Mix2));
            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            MPreAmp(x, out, d.DPL[k], 0)

            // output stage
            MWriteOutputs(out)
        }
        break;
    case fc_serial2:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto input = vMul(d.FB, d.FBlineL);
            input = vAdd(inL, sdsp::softclip_ps(input));
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);
            auto x = input, y = inR;

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, SIMD_MM(and_ps)(mask, x)));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], d.wsLPF, d.Drive);
            }

            if (A || WS)
            {
                d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
                x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(input, SIMD_MM(sub_ps)(one, d.Mix1)),
                                    SIMD_MM(mul_ps)(x, d.Mix1));
            }

            y = SIMD_MM(add_ps)(x, y);

            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, SIMD_MM(sub_ps)(one, d.Mix2)),
                                SIMD_MM(mul_ps)(y, d.Mix2));
            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            auto fbOut = out;
            MPreAmp(x, out, d.DPL[k], 0)
            d.FBlineL = fbOut;

            // output stage
            MWriteOutputs(out)
        }
        break;
    case fc_serial3: // filter 2 is only heard in the feedback path, good for physical modelling
                     // with comb as f2
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto input = vMul(d.FB, d.FBlineL);
            input = vAdd(inL, sdsp::softclip_ps(input));
            auto x = input, y = inR;
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, SIMD_MM(and_ps)(mask, x)));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], d.wsLPF, d.Drive);
            }

            if (A || WS)
            {
                d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
                x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(input, SIMD_MM(sub_ps)(one, d.Mix1)),
                                    SIMD_MM(mul_ps)(x, d.Mix1));
            }

            // output stage
            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            MPreAmp(x, out, d.DPL[k], 0)
            x = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));

            MWriteOutputs(out)

                y = SIMD_MM(add_ps)(x, y);

            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, SIMD_MM(sub_ps)(one, d.Mix2)),
                                SIMD_MM(mul_ps)(y, d.Mix2));

            d.FBlineL = y;
        }
        break;
    case fc_dual1:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto fb = SIMD_MM(mul_ps)(d.FB, d.FBlineL);
            fb = sdsp::softclip_ps(fb);
            auto x = SIMD_MM(add_ps)(inL, fb);
            auto y = SIMD_MM(add_ps)(inR, fb);
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, d.Mix1), SIMD_MM(mul_ps)(y, d.Mix2));

            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, SIMD_MM(and_ps)(mask, x)));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], d.wsLPF, d.Drive);
            }

            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            auto fbOut = out;
            MPreAmp(x, out, d.DPL[k], 0)
            d.FBlineL = fbOut;
            // output stage
            MWriteOutputs(out)
        }
        break;
    case fc_dual2:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto fb = SIMD_MM(mul_ps)(d.FB, d.FBlineL);
            fb = sdsp::softclip_ps(fb);
            auto x = SIMD_MM(add_ps)(inL, fb);
            auto y = SIMD_MM(add_ps)(inR, fb);
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, SIMD_MM(and_ps)(mask, x)));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], d.wsLPF, d.Drive);
            }

            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, d.Mix1), SIMD_MM(mul_ps)(y, d.Mix2));

            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            auto fbOut = out;
            MPreAmp(x, out, d.DPL[k], 0)
            d.FBlineL = fbOut;
            // output stage
            MWriteOutputs(out)
        }
        break;
    case fc_ring:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto fb = SIMD_MM(mul_ps)(d.FB, d.FBlineL);
            fb = sdsp::softclip_ps(fb);
            auto x = SIMD_MM(add_ps)(inL, fb);
            auto y = SIMD_MM(add_ps)(inR, fb);
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);

            x = SIMD_MM(mul_ps)(SIMD_MM(add_ps)(SIMD_MM(mul_ps)(SIMD_MM(sub_ps)(one, d.Mix1), y),
                                                SIMD_MM(mul_ps)(x, d.Mix1)),
                                SIMD_MM(add_ps)(SIMD_MM(mul_ps)(SIMD_MM(sub_ps)(one, d.Mix2), x),
                                                SIMD_MM(mul_ps)(y, d.Mix2)));

            if (WS)
            {
                d.wsLPF = SIMD_MM(mul_ps)(hb_c, SIMD_MM(add_ps)(d.wsLPF, x));
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], SIMD_MM(and_ps)(mask, d.wsLPF), d.Drive);
            }

            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto out = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            auto fbOut = out;
            MPreAmp(x, out, d.DPL[k], 0)
            d.FBlineL = fbOut;
            // output stage
            MWriteOutputs(out)
        }
        break;
    case fc_stereo:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto fb = SIMD_MM(mul_ps)(d.FB, d.FBlineL);
            fb = sdsp::softclip_ps(fb);
            auto x = SIMD_MM(add_ps)(inL, fb);
            auto y = SIMD_MM(add_ps)(inR, fb);
            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
                x = g.FU1ptr(&d.FU[0], x);
            MFilterPost(x, 0, 0, A)
            if (B)
                y = g.FU2ptr(&d.FU[1], y);
            MFilterPost(y, 1, 1, B)

            if (WS)
            {
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], SIMD_MM(and_ps)(mask, x), d.Drive);
                y = g.WSptr(&d.WSS[1], SIMD_MM(and_ps)(mask, y), d.Drive);
            }

            d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
            d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
            x = SIMD_MM(mul_ps)(x, d.Mix1);
            y = SIMD_MM(mul_ps)(y, d.Mix2);

            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto px = x, py = y;
            x = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            y = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(y, d.Gain));
            auto ox = x, oy = y;
            // half to each filter's pan position
            MPreAmp(px, ox, SIMD_MM(mul_ps)(hb_c, d.DPL[k]), 0)
            MPreAmp(py, oy, SIMD_MM(mul_ps)(hb_c, d.DPR[k]), 1)
            d.FBlineL = SIMD_MM(add_ps)(x, y);

            // output stage
            MWriteOutputsDual(ox, oy) AssertReasonableAudioFloat(OutL[k]);
            AssertReasonableAudioFloat(OutR[k]);
        }
        break;
    case fc_wide:
        for (int k = 0; k < BLOCK_SIZE_OS; k++)
        {
            MStepPost
            MPreShape
            d.FB = SIMD_MM(add_ps)(d.FB, d.dFB);
            auto fbL = SIMD_MM(mul_ps)(d.FB, d.FBlineL);
            auto fbR = SIMD_MM(mul_ps)(d.FB, d.FBlineR);
            auto xin = SIMD_MM(add_ps)(inL, sdsp::softclip_ps(fbL));
            auto yin = SIMD_MM(add_ps)(inR, sdsp::softclip_ps(fbR));
            auto x = xin;
            auto y = yin;

            auto mask = SIMD_MM(load_ps)((float *)&d.FU[0].active);

            if (A)
            {
                x = g.FU1ptr(&d.FU[0], x);
                y = g.FU1ptr(&d.FU[2], y);
            }
            MFilterPost(x, 0, 0, A)
            MFilterPost(y, 0, 2, A)

            if (WS)
            {
                d.Drive = SIMD_MM(add_ps)(d.Drive, d.dDrive);
                x = g.WSptr(&d.WSS[0], SIMD_MM(and_ps)(mask, x), d.Drive);
                y = g.WSptr(&d.WSS[1], SIMD_MM(and_ps)(mask, y), d.Drive);
            }

            if (A || WS)
            {
                d.Mix1 = SIMD_MM(add_ps)(d.Mix1, d.dMix1);
                auto t = SIMD_MM(sub_ps)(one, d.Mix1);
                x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(xin, t), SIMD_MM(mul_ps)(x, d.Mix1));
                y = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(yin, t), SIMD_MM(mul_ps)(y, d.Mix1));
            }

            if (B)
            {
                auto z = g.FU2ptr(&d.FU[1], x);
                auto w = g.FU2ptr(&d.FU[3], y);
                MFilterPost(z, 1, 1, true)
                MFilterPost(w, 1, 3, true)

                d.Mix2 = SIMD_MM(add_ps)(d.Mix2, d.dMix2);
                auto t = SIMD_MM(sub_ps)(one, d.Mix2);
                x = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(x, t), SIMD_MM(mul_ps)(z, d.Mix2));
                y = SIMD_MM(add_ps)(SIMD_MM(mul_ps)(y, t), SIMD_MM(mul_ps)(w, d.Mix2));
            }
            else
            {
                x = SIMD_MM(mul_ps)(x, d.FGain[1]);
                y = SIMD_MM(mul_ps)(y, d.FGain[1]);
            }

            d.Gain = SIMD_MM(add_ps)(d.Gain, d.dGain);
            auto px = x, py = y;
            x = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(x, d.Gain));
            y = SIMD_MM(and_ps)(mask, SIMD_MM(mul_ps)(y, d.Gain));
            auto ox = x, oy = y;
            MPreAmp(px, ox, d.DPL[k], 0)
            MPreAmp(py, oy, d.DPR[k], 1)
            d.FBlineL = x;
            d.FBlineR = y;

            // output stage
            MWriteOutputsDual(ox, oy) AssertReasonableAudioFloat(OutL[k]);
            AssertReasonableAudioFloat(OutR[k]);
        }
        break;
    }
}

template <int config> FBQFPtr GetFBQPointer2(bool A, bool WS, bool B)
{
    if (A)
    {
        if (B)
        {
            if (WS)
                return ProcessFBQuad<config, 1, 1, 1>;
            else
                return ProcessFBQuad<config, 1, 0, 1>;
        }
        else
        {
            if (WS)
                return ProcessFBQuad<config, 1, 1, 0>;
            else
                return ProcessFBQuad<config, 1, 0, 0>;
        }
    }
    else
    {
        if (B)
        {
            if (WS)
                return ProcessFBQuad<config, 0, 1, 1>;
            else
                return ProcessFBQuad<config, 0, 0, 1>;
        }
        else
        {
            if (WS)
                return ProcessFBQuad<config, 0, 1, 0>;
            else
                return ProcessFBQuad<config, 0, 0, 0>;
        }
    }
    return 0;
}

FBQFPtr GetFBQPointer(int config, bool A, bool WS, bool B)
{
    switch (config)
    {
    case fc_serial1:
        return GetFBQPointer2<fc_serial1>(A, WS, B);
    case fc_serial2:
        return GetFBQPointer2<fc_serial2>(A, WS, B);
    case fc_serial3:
        return GetFBQPointer2<fc_serial3>(A, WS, B);
    case fc_dual1:
        return GetFBQPointer2<fc_dual1>(A, WS, B);
    case fc_dual2:
        return GetFBQPointer2<fc_dual2>(A, WS, B);
    case fc_ring:
        return GetFBQPointer2<fc_ring>(A, WS, B);
    case fc_stereo:
        return GetFBQPointer2<fc_stereo>(A, WS, B);
    case fc_wide:
        return GetFBQPointer2<fc_wide>(A, WS, B);
    }
    return 0;
}

void InitQuadFilterChainStateToZero(QuadFilterChainState *Q)
{
    Q->Gain = SIMD_MM(setzero_ps)();
    Q->FB = SIMD_MM(setzero_ps)();
    Q->Mix1 = SIMD_MM(setzero_ps)();
    Q->Mix2 = SIMD_MM(setzero_ps)();
    Q->Drive = SIMD_MM(setzero_ps)();
    Q->dGain = SIMD_MM(setzero_ps)();
    Q->dFB = SIMD_MM(setzero_ps)();
    Q->dMix1 = SIMD_MM(setzero_ps)();
    Q->dMix2 = SIMD_MM(setzero_ps)();
    Q->dDrive = SIMD_MM(setzero_ps)();

    Q->wsLPF = SIMD_MM(setzero_ps)();
    for (int u = 0; u < 2; ++u)
    {
        Q->FDrive[u] = Q->FAsym[u] = Q->dFDrive[u] = Q->dFAsym[u] = Q->dFGain[u] =
            SIMD_MM(setzero_ps)();
        Q->FGain[u] = SIMD_MM(set1_ps)(1.f);
    }
    Q->ADrive = Q->AAsym = Q->dADrive = Q->dAAsym = SIMD_MM(setzero_ps)();
    Q->PreDrive = Q->dPreDrive = SIMD_MM(setzero_ps)();
    for (int i = 0; i < 6; ++i)
        Q->dcX[i] = Q->dcY[i] = SIMD_MM(setzero_ps)();
    Q->FBlineL = SIMD_MM(setzero_ps)();
    Q->FBlineR = SIMD_MM(setzero_ps)();

    for (auto i = 0; i < BLOCK_SIZE_OS; ++i)
    {
        Q->DL[i] = SIMD_MM(setzero_ps)();
        Q->DR[i] = SIMD_MM(setzero_ps)();
        Q->DPL[i] = SIMD_MM(setzero_ps)();
        Q->DPR[i] = SIMD_MM(setzero_ps)();
    }

    Q->OutL = SIMD_MM(setzero_ps)();
    Q->OutR = SIMD_MM(setzero_ps)();
    Q->dOutL = SIMD_MM(setzero_ps)();
    Q->dOutR = SIMD_MM(setzero_ps)();
    Q->Out2L = SIMD_MM(setzero_ps)();
    Q->Out2R = SIMD_MM(setzero_ps)();
    Q->dOut2L = SIMD_MM(setzero_ps)();
    Q->dOut2R = SIMD_MM(setzero_ps)();
}
