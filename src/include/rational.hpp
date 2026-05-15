#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

struct Rational {
	int64_t num;
	int64_t den;

	Rational() : num(0), den(1) {
	}
	Rational(int64_t n, int64_t d) : num(n), den(d) {
	}

	static int64_t gcd(int64_t a, int64_t b) {
		a = a < 0 ? -a : a;
		b = b < 0 ? -b : b;
		while (b) {
			a %= b;
			std::swap(a, b);
		}
		return a ? a : 1;
	}

	Rational reduce() const {
		int64_t g = gcd(num < 0 ? -num : num, den);
		return Rational(num / g, den / g);
	}

	double to_double() const {
		return (double)num / den;
	}
};

struct SnapResult {
	bool valid;
	Rational r;
	SnapResult() : valid(false), r() {
	}
	SnapResult(bool v, Rational rat) : valid(v), r(rat) {
	}
};

inline SnapResult snap_to_rational(double v, int max_den, double tol = 1e-9) {
	if (!std::isfinite(v))
		return SnapResult();
	bool negative = v < 0.0;
	double av = negative ? -v : v;
	SnapResult best;
	double best_err = tol;
	for (int d = 1; d <= max_den; d++) {
		int64_t n = (int64_t)std::llround(av * d);
		double err = std::abs(av - (double)n / d);
		if (err < best_err) {
			best_err = err;
			int64_t g = Rational::gcd(n, (int64_t)d);
			best = SnapResult(true, Rational((negative ? -n : n) / g, (int64_t)d / g));
		}
	}
	return best;
}

// ─── Portable exact integer validation ───────────────────────────────────────
// max_den is at most 64 in practice, so:
//   slope.den, intercept.den <= 64
//   denom = slope.den * intercept.den <= 4096
//   feat_i is an ARC pixel coordinate or count, at most ~30
//   slope.num <= 64, intercept.num <= 64
//   numer = slope.num * feat_i * intercept.den + intercept.num * slope.den
//         <= 64 * 30 * 64 + 64 * 64 = 122880 + 4096 = 126976
// All values fit comfortably in int64_t. No __int128 needed.

inline bool is_integer_result(int64_t feat_i, const Rational &slope, const Rational &intercept) {
	int64_t denom = slope.den * intercept.den;
	if (denom == 0)
		return false;
	int64_t numer = slope.num * feat_i * intercept.den + intercept.num * slope.den;
	return (numer % denom) == 0;
}

inline int64_t integer_result(int64_t feat_i, const Rational &slope, const Rational &intercept) {
	int64_t denom = slope.den * intercept.den;
	int64_t numer = slope.num * feat_i * intercept.den + intercept.num * slope.den;
	return numer / denom;
}
