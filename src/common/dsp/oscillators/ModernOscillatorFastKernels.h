/*
 * Oscairn: the closed forms the fast Modern oscillator path (ModernOscillatorFast.cpp) plays,
 * four samples at a time. Each returns what ModernOscillator.cpp's numerical second difference
 * of the generator, (g(p) + g(p - 2d) - 2 g(p - d)) / (4 d^2) with the phases wrapped, gives for
 * a phase p in [0, 1) and increment d (rd is 1 / d), without the single precision cancellation
 * that difference has.
 *
 * With x = 2 p - 1 the generators are piecewise polynomials with corners where a derivative
 * jumps: the second difference is the generator's second derivative at the stencil's centre, plus
 * the h^2 / 12 times its fourth derivative (h = 2 d), plus, for each corner the stencil
 * reaches, the second difference of the truncated power the corner adds. The saw's generator
 * (x^3 - x) / 6 has a jump in its second derivative at the wrap; the parabola pair "sine" has
 * its fourth derivative jump (-8 to 8) at x = 0 and (8 to -8) at the wrap.
 *
 * Surge XT is released under the GNU General Public Licence v3 or later (GPL-3.0-or-later).
 */

#ifndef SURGE_COMMON_DSP_OSCILLATORS_MODERNOSCILLATORFASTKERNELS_H
#define SURGE_COMMON_DSP_OSCILLATORS_MODERNOSCILLATORFASTKERNELS_H

#include "globals.h"

namespace Surge::Oscillator::Fast
{
using V = SIMD_M128;

inline V vset(float f) { return SIMD_MM(set1_ps)(f); }
inline V vadd(V a, V b) { return SIMD_MM(add_ps)(a, b); }
inline V vsub(V a, V b) { return SIMD_MM(sub_ps)(a, b); }
inline V vmul(V a, V b) { return SIMD_MM(mul_ps)(a, b); }
inline V vmax(V a, V b) { return SIMD_MM(max_ps)(a, b); }
inline V vmin(V a, V b) { return SIMD_MM(min_ps)(a, b); }
// mask ? a : b
inline V vsel(V mask, V a, V b)
{
    return SIMD_MM(or_ps)(SIMD_MM(and_ps)(mask, a), SIMD_MM(andnot_ps)(mask, b));
}

/*
 * The saw: g(p) = (x^3 - x) / 6, g'' = x, with a step of 2 in g'' at the wrap (x = 1 back to
 * -1), so the second difference over the stencil p, p - d, p - 2d is the saw at the stencil's
 * centre, 2 (p - d) - 1, plus the step's share: the stencil's triangular kernel's mass before
 * the wrap, times 2.
 */
inline V sawClosed(V p, V d, V rd)
{
    const V one = vset(1.f), half = vset(0.5f), two = vset(2.f), zero = vset(0.f);
    V base = vsub(vmul(two, vsub(p, d)), one);
    V s = vsub(one, vmul(p, rd)); // where the wrap lies in the kernel, -1 to 1
    s = vmin(one, vmax(vset(-1.f), s));
    V q = vsub(one, vmax(s, vsub(zero, s)));
    V m = vmul(half, vmul(q, q));
    V pos = SIMD_MM(cmpgt_ps)(s, zero);
    V mass = vsel(pos, vsub(one, m), m);
    return vadd(base, vmul(two, mass));
}

/*
 * The "sine", a pair of parabolas: g(x) = -(m x^4 + 2 x^3 - x) / 3 with m = 1 for x < 0 and -1
 * above, so g'' = -4 (m x^2 + x) and g'''' = -8 m. Write y0 = x, y1 = y0 - h, y2 = y1 - h for
 * the stencil on the unwrapped line (h = 2 d; a stencil past the wrap is the same line, g
 * having period 2).
 *
 * Away from corners the second difference over h^2 is g''(y1) + h^2 g''''(y1) / 12 =
 * -4 (m y1^2 + y1) - (2/3) m h^2, with y1 and m taken at the wrapped centre. A corner adds
 * (2/3) u^4 for u past it (the jump in the polynomial piece), whose second difference over h^2
 * is evaluated only where the stencil reaches it, in the form u^2 (u^2 / h^2) so that nothing
 * cancels:
 *   centre at or right of x = 0:       - (2/3) (-y2)+^4 / h^2
 *   centre between the wrap and x = 0: + (2/3) ((y0)+^4 + (-1 - y2)+^4) / h^2
 *   centre left of the wrap (y1 < -1): - (2/3) (y0 + 1)^4 / h^2
 * Valid up to d = 1/4 (the stencil spans at most two corners).
 */
inline V sineClosed(V p, V d, V rd)
{
    const V one = vset(1.f), zero = vset(0.f);
    const V h = vadd(d, d), rh = vmul(vset(0.5f), rd);
    const V y0 = vsub(vadd(p, p), one), y1 = vsub(y0, h), y2 = vsub(y1, h);
    const V isC = SIMD_MM(cmplt_ps)(y1, vset(-1.f)), isA = SIMD_MM(cmpge_ps)(y1, zero);
    const V x1 = vadd(y1, SIMD_MM(and_ps)(isC, vset(2.f)));
    const V neg = SIMD_MM(cmplt_ps)(x1, zero);
    const V m = vsel(neg, one, vset(-1.f));
    const V smooth = vsub(vmul(vset(-4.f), vadd(vmul(m, vmul(x1, x1)), x1)),
                          vmul(vset(2.f / 3.f), vmul(m, vmul(h, h))));
    // u^4 / h^2 for u >= 0
    auto t4 = [&](V u) {
        V w = vmul(vmul(u, u), rh);
        return vmul(w, w);
    };
    const V a = t4(vmax(vsub(zero, y2), zero));
    const V c = t4(vadd(y0, one));
    const V b = vadd(t4(vmax(y0, zero)), t4(vmax(vsub(vset(-1.f), y2), zero)));
    V corner = vsel(isA, vsub(zero, a), vsel(isC, vsub(zero, c), b));
    return vadd(smooth, vmul(vset(2.f / 3.f), corner));
}
} // namespace Surge::Oscillator::Fast

#endif
