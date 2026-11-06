#include "lib/sentiment_core/js_math.hpp"

#include <bit>
#include <cstdint>
#include <limits>

// fdlibm (e_log.c / e_log10.c、V8 の base/ieee754.cc と同じ手順) の移植。定数と分岐は原典のまま。
namespace di::lib::sentiment_core {
namespace {
inline std::uint32_t hi(double x) { return static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(x) >> 32); }
inline std::uint32_t lo(double x) { return static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(x)); }
inline double with_hi(double x, std::uint32_t h) {
    return std::bit_cast<double>((static_cast<std::uint64_t>(h) << 32) | lo(x));
}

double fdlibm_log(double x) {
    constexpr double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
                     two54 = 1.80143985094819840000e+16, Lg1 = 6.666666666666735130e-01,
                     Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
                     Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;
    std::int32_t hx = static_cast<std::int32_t>(hi(x));
    const std::uint32_t lx = lo(x);
    std::int32_t k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | static_cast<std::int32_t>(lx)) == 0) return -std::numeric_limits<double>::infinity();
        if (hx < 0) return std::numeric_limits<double>::quiet_NaN();
        k -= 54;
        x *= two54;
        hx = static_cast<std::int32_t>(hi(x));
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    const std::int32_t i0 = (hx + 0x95f64) & 0x100000;
    x = with_hi(x, static_cast<std::uint32_t>(hx | (i0 ^ 0x3ff00000)));
    k += (i0 >> 20);
    const double f = x - 1.0;
    if ((0x000fffff & (2 + hx)) < 3) {
        if (f == 0.0) {
            if (k == 0) return 0.0;
            const double dk = k;
            return dk * ln2_hi + dk * ln2_lo;
        }
        const double R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        const double dk = k;
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    const double s = f / (2.0 + f);
    const double dk = k;
    const double z = s * s;
    std::int32_t i = hx - 0x6147a;
    const double w = z * z;
    const std::int32_t j = 0x6b851 - hx;
    const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    const double R = t2 + t1;
    if (i > 0) {
        const double hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

double fdlibm_log10(double x) {
    constexpr double two54 = 1.80143985094819840000e+16, ivln10 = 4.34294481903251816668e-01,
                     log10_2hi = 3.01029995663611771306e-01, log10_2lo = 3.69423907715893078616e-13;
    std::int32_t hx = static_cast<std::int32_t>(hi(x));
    const std::uint32_t lx = lo(x);
    std::int32_t k = 0;
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | static_cast<std::int32_t>(lx)) == 0) return -std::numeric_limits<double>::infinity();
        if (hx < 0) return std::numeric_limits<double>::quiet_NaN();
        k -= 54;
        x *= two54;
        hx = static_cast<std::int32_t>(hi(x));
    }
    if (hx >= 0x7ff00000) return x + x;
    if (hx == 0x3ff00000 && lo(x) == 0) return 0.0;
    k += (hx >> 20) - 1023;
    const std::int32_t i = static_cast<std::int32_t>((static_cast<std::uint32_t>(k) & 0x80000000u) >> 31);
    hx = (hx & 0x000fffff) | ((0x3ff - i) << 20);
    const double y = k + i;
    x = with_hi(x, static_cast<std::uint32_t>(hx));
    const double z = y * log10_2lo + ivln10 * fdlibm_log(x);
    return z + y * log10_2hi;
}
} // namespace

double js_log(double x) { return fdlibm_log(x); }
double js_log10(double x) { return fdlibm_log10(x); }

} // namespace di::lib::sentiment_core
