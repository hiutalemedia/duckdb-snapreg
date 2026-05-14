#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

struct Rational {
    int64_t num;
    int64_t den;

    Rational() : num(0), den(1) {}
    Rational(int64_t n, int64_t d) : num(n), den(d) {}

    static int64_t gcd(int64_t a, int64_t b) {
        a = a < 0 ? -a : a;
        b = b < 0 ? -b : b;
        while (b) { a %= b; std::swap(a, b); }
        return a ? a : 1;
    }

    Rational reduce() const {
        int64_t g = gcd(num < 0 ? -num : num, den);
        return Rational(num / g, den / g);
    }

    double to_double() const { return (double)num / den; }
};

struct SnapResult {
    bool     valid;
    Rational r;
    SnapResult() : valid(false), r() {}
    SnapResult(bool v, Rational rat) : valid(v), r(rat) {}
};

inline SnapResult snap_to_rational(double v, int max_den, double tol = 1e-9) {
    if (!std::isfinite(v)) return SnapResult();
    bool   negative = v < 0.0;
    double av       = negative ? -v : v;
    SnapResult best;
    double     best_err = tol;
    for (int d = 1; d <= max_den; d++) {
        int64_t n   = (int64_t)std::llround(av * d);
        double  err = std::abs(av - (double)n / d);
        if (err < best_err) {
            best_err  = err;
            int64_t g = Rational::gcd(n, (int64_t)d);
            best = SnapResult(true, Rational((negative ? -n : n) / g, (int64_t)d / g));
        }
    }
    return best;
}

inline bool is_integer_result(int64_t feat_i, const Rational &slope,
                               const Rational &intercept) {
    typedef __int128 i128;
    i128 numer = (i128)slope.num * feat_i * intercept.den
               + (i128)intercept.num * slope.den;
    i128 denom = (i128)slope.den * intercept.den;
    return denom != 0 && (numer % denom == 0);
}

inline int64_t integer_result(int64_t feat_i, const Rational &slope,
                               const Rational &intercept) {
    typedef __int128 i128;
    i128 numer = (i128)slope.num * feat_i * intercept.den
               + (i128)intercept.num * slope.den;
    i128 denom = (i128)slope.den * intercept.den;
    return (int64_t)(numer / denom);
}
