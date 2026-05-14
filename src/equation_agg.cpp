#include "duckdb.hpp"
#include "rational.hpp"
#include <cmath>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

namespace duckdb {

// ─── Result type ──────────────────────────────────────────────────────────────
//
// equation_agg(target DOUBLE, feature DOUBLE, example_id INTEGER,
//              row_idx INTEGER, max_den INTEGER)
//   GROUP BY feat_name
// →
//   STRUCT(
//     slope_num       BIGINT,
//     slope_den       BIGINT,
//     intercept_num   BIGINT,
//     intercept_den   BIGINT,
//     mismatches      INTEGER,    -- total rows where equation fails
//     n_good          INTEGER,
//     n_bad           INTEGER,
//     consistent      BOOLEAN,    -- true iff every example has 0 mismatches
//     r2              DOUBLE,
//     per_example_mm  INTEGER[],  -- mismatch count per example (sorted by ex_id)
//     good_indices    INTEGER[],  -- row_idx values that passed
//     bad_indices     INTEGER[]   -- row_idx values that failed
//   )
//
// Returns NULL if fewer than 2 points, OLS denom ≈ 0 (constant feature),
// or no rational snap exists within tolerance.
//
// For constant-feature columns (all x equal), slope is forced to 0 and
// intercept = mean(target). If that snaps cleanly the result is still valid —
// useful for discovering pure constant holes.

// ─── Aggregate state ──────────────────────────────────────────────────────────

struct EquationAggState {
    int64_t n       = 0;
    int32_t max_den = 16;

    // Raw points — all fit logic happens in finalize.
    // ARC has ≤ ~20 data points per slot so heap cost is negligible.
    std::vector<double>  *xs          = nullptr;
    std::vector<double>  *ys          = nullptr;
    std::vector<int32_t> *example_ids = nullptr;
    std::vector<int32_t> *row_idxs    = nullptr;

    void Initialize() {
        xs          = new std::vector<double>();
        ys          = new std::vector<double>();
        example_ids = new std::vector<int32_t>();
        row_idxs    = new std::vector<int32_t>();
    }

    void AddPoint(double x, double y, int32_t ex_id, int32_t row_idx) {
        n++;
        xs->push_back(x);
        ys->push_back(y);
        example_ids->push_back(ex_id);
        row_idxs->push_back(row_idx);
    }

    void Merge(const EquationAggState &o) {
        if (!o.xs) return;
        n      += o.n;
        max_den = std::max(max_den, o.max_den);
        xs->insert(xs->end(), o.xs->begin(), o.xs->end());
        ys->insert(ys->end(), o.ys->begin(), o.ys->end());
        example_ids->insert(example_ids->end(), o.example_ids->begin(), o.example_ids->end());
        row_idxs->insert(row_idxs->end(), o.row_idxs->begin(), o.row_idxs->end());
    }

    void Destroy() {
        delete xs; delete ys; delete example_ids; delete row_idxs;
        xs = ys = nullptr;
        example_ids = row_idxs = nullptr;
    }
};

// ─── Pairwise rational candidate search ──────────────────────────────────────

struct FitResult {
    bool     valid;
    Rational slope;
    Rational intercept;
    double   r2;
    FitResult() : valid(false), slope(), intercept(), r2(0) {}
    FitResult(Rational s, Rational i, double r) : valid(true), slope(s), intercept(i), r2(r) {}
};
// Generates slope candidates from all point pairs, intercept candidates from
// each point, then validates each (slope, intercept) pair with exact arithmetic.
// Returns the best fit (fewest mismatches). No regression — fully deterministic.

static bool validate_point(double feat, double target,
                            const Rational &slope, const Rational &intercept) {
    if (std::abs(feat - std::round(feat)) < 1e-9) {
        int64_t fi = (int64_t)std::llround(feat);
        if (!is_integer_result(fi, slope, intercept)) return false;
        return integer_result(fi, slope, intercept) == (int64_t)std::llround(target);
    }
    double pred = slope.to_double() * feat + intercept.to_double();
    return std::abs(pred - std::round(pred)) < 1e-9
        && std::abs(pred - target) < 0.5;
}

static FitResult fit(const EquationAggState &s) {
    if (s.n < 2) return FitResult();

    // ── Collect unique slope candidates ──────────────────────────────────────
    std::vector<Rational> slopes;

    // Always try slope=0 (constant equation)
    slopes.push_back(Rational(0, 1));

    for (int64_t i = 0; i < s.n; i++) {
        for (int64_t j = i + 1; j < s.n; j++) {
            double dx = (*s.xs)[j] - (*s.xs)[i];
            if (std::abs(dx) < 1e-12) continue;
            double dy = (*s.ys)[j] - (*s.ys)[i];
            SnapResult sr = snap_to_rational(dy / dx, s.max_den);
            if (!sr.valid) continue;
            // Deduplicate
            bool dup = false;
            for (size_t k = 0; k < slopes.size(); k++)
                if (slopes[k].num == sr.r.num && slopes[k].den == sr.r.den)
                    { dup = true; break; }
            if (!dup) slopes.push_back(sr.r);
        }
    }

    // ── For each slope, collect unique intercept candidates ──────────────────
    FitResult best;
    int32_t   best_mm = (int32_t)s.n + 1;

    for (size_t si = 0; si < slopes.size(); si++) {
        const Rational &slope = slopes[si];
        std::vector<Rational> intercepts;

        for (int64_t k = 0; k < s.n; k++) {
            double ir_raw = (*s.ys)[k] - slope.to_double() * (*s.xs)[k];
            SnapResult ir = snap_to_rational(ir_raw, s.max_den);
            if (!ir.valid) continue;
            bool dup = false;
            for (size_t m = 0; m < intercepts.size(); m++)
                if (intercepts[m].num == ir.r.num && intercepts[m].den == ir.r.den)
                    { dup = true; break; }
            if (!dup) intercepts.push_back(ir.r);
        }

        // ── Validate each (slope, intercept) pair ────────────────────────────
        for (size_t ii = 0; ii < intercepts.size(); ii++) {
            const Rational &intercept = intercepts[ii];
            int32_t mm = 0;
            for (int64_t k = 0; k < s.n; k++)
                if (!validate_point((*s.xs)[k], (*s.ys)[k], slope, intercept))
                    mm++;
            if (mm < best_mm) {
                best_mm = mm;
                best    = FitResult(slope, intercept, 0.0);
                if (mm == 0) return best; // perfect fit — done
            }
        }
    }

    return best;
}

// ─── Helpers to append to a LIST child inside a STRUCT ────────────────────────

template <typename T>
static void AppendList(Vector &list_vec, idx_t row, const std::vector<T> &items) {
    auto *list_data = FlatVector::GetData<list_entry_t>(list_vec);
    auto cur_size   = ListVector::GetListSize(list_vec);

    ListVector::Reserve(list_vec, cur_size + items.size());
    auto &child    = ListVector::GetEntry(list_vec);
    auto *cdata    = FlatVector::GetData<T>(child);

    for (idx_t k = 0; k < (idx_t)items.size(); k++) {
        cdata[cur_size + k] = items[k];
    }
    ListVector::SetListSize(list_vec, cur_size + items.size());

    list_data[row].offset = cur_size;
    list_data[row].length = items.size();
}

// ─── Aggregate callbacks ──────────────────────────────────────────────────────

static void EqAggInit(const AggregateFunction &, data_ptr_t state_ptr) {
    auto &s = *reinterpret_cast<EquationAggState *>(state_ptr);
    new (&s) EquationAggState();
    s.Initialize();
}

static void EqAggUpdate(Vector inputs[], AggregateInputData &, idx_t input_count,
                        Vector &state_vec, idx_t count) {
    // inputs: 0=target, 1=feature, 2=example_id, 3=row_idx, 4=max_den
    auto *states = FlatVector::GetData<data_ptr_t>(state_vec);

    UnifiedVectorFormat target_fmt, feat_fmt, ex_fmt, row_fmt, den_fmt;
    inputs[0].ToUnifiedFormat(count, target_fmt);
    inputs[1].ToUnifiedFormat(count, feat_fmt);
    inputs[2].ToUnifiedFormat(count, ex_fmt);
    inputs[3].ToUnifiedFormat(count, row_fmt);
    inputs[4].ToUnifiedFormat(count, den_fmt);

    auto *targets  = UnifiedVectorFormat::GetData<double> (target_fmt);
    auto *feats    = UnifiedVectorFormat::GetData<double> (feat_fmt);
    auto *ex_ids   = UnifiedVectorFormat::GetData<int32_t>(ex_fmt);
    auto *row_idxs = UnifiedVectorFormat::GetData<int32_t>(row_fmt);
    auto *max_dens = UnifiedVectorFormat::GetData<int32_t>(den_fmt);

    for (idx_t i = 0; i < count; i++) {
        auto ti = target_fmt.sel->get_index(i);
        auto fi = feat_fmt.sel->get_index(i);
        auto ei = ex_fmt.sel->get_index(i);
        auto ri = row_fmt.sel->get_index(i);
        auto di = den_fmt.sel->get_index(i);

        if (!target_fmt.validity.RowIsValid(ti) ||
            !feat_fmt.validity.RowIsValid(fi))    continue;

        auto &s = *reinterpret_cast<EquationAggState *>(states[i]);
        if (den_fmt.validity.RowIsValid(di)) s.max_den = max_dens[di];
        s.AddPoint(feats[fi], targets[ti],
                   ex_fmt.validity.RowIsValid(ei)  ? ex_ids[ei]   : 0,
                   row_fmt.validity.RowIsValid(ri)  ? row_idxs[ri] : (int32_t)i);
    }
}

static void EqAggCombine(Vector &src_vec, Vector &tgt_vec,
                          AggregateInputData &, idx_t count) {
    auto *srcs = FlatVector::GetData<data_ptr_t>(src_vec);
    auto *tgts = FlatVector::GetData<data_ptr_t>(tgt_vec);
    for (idx_t i = 0; i < count; i++) {
        auto &src = *reinterpret_cast<EquationAggState *>(srcs[i]);
        auto &tgt = *reinterpret_cast<EquationAggState *>(tgts[i]);
        tgt.Merge(src);
    }
}

static void EqAggFinalize(Vector &state_vec, AggregateInputData &,
                           Vector &result, idx_t count, idx_t offset) {
    auto *states = FlatVector::GetData<data_ptr_t>(state_vec);
    auto &entries = StructVector::GetEntries(result);

    auto *slope_num_d     = FlatVector::GetData<int64_t>(*entries[0]);
    auto *slope_den_d     = FlatVector::GetData<int64_t>(*entries[1]);
    auto *intercept_num_d = FlatVector::GetData<int64_t>(*entries[2]);
    auto *intercept_den_d = FlatVector::GetData<int64_t>(*entries[3]);
    auto *mismatches_d    = FlatVector::GetData<int32_t>(*entries[4]);
    auto *n_good_d        = FlatVector::GetData<int32_t>(*entries[5]);
    auto *n_bad_d         = FlatVector::GetData<int32_t>(*entries[6]);
    auto *consistent_d    = FlatVector::GetData<bool>   (*entries[7]);
    auto *r2_d            = FlatVector::GetData<double> (*entries[8]);
    // entries[9]  = per_example_mm  INTEGER[]
    // entries[10] = good_indices    INTEGER[]
    // entries[11] = bad_indices     INTEGER[]

    auto &result_validity = FlatVector::Validity(result);

    for (idx_t i = 0; i < count; i++) {
        auto &s = *reinterpret_cast<EquationAggState *>(states[i + offset]);

        if (s.n < 2) { result_validity.SetInvalid(i); continue; }

        auto fit_opt = fit(s);
        if (!fit_opt.valid) { result_validity.SetInvalid(i); continue; }
        auto &fr = fit_opt;

        // ── Per-point validation ──────────────────────────────────────────────
        int32_t total_mm = 0;
        bool consistent  = true;
        std::map<int32_t, int32_t> per_ex_mm;
        std::set<int32_t> seen_ex;
        for (auto eid : *s.example_ids) seen_ex.insert(eid);

        std::vector<int32_t> good_idx, bad_idx;

        for (int64_t j = 0; j < s.n; j++) {
            double feat = (*s.xs)[j];
            bool ok;

            if (std::abs(feat - std::round(feat)) < 1e-9) {
                // Integer-valued feature: exact arithmetic
                int64_t fi = (int64_t)std::llround(feat);
                ok = is_integer_result(fi, fr.slope, fr.intercept);
                if (ok) {
                    // Also verify against stored target
                    int64_t pred = integer_result(fi, fr.slope, fr.intercept);
                    ok = (std::abs((*s.ys)[j] - (double)pred) < 0.5);
                }
            } else {
                // Non-integer feature: float check (slope * feat + intercept ≈ integer ≈ target)
                double pred = fr.slope.to_double() * feat + fr.intercept.to_double();
                ok = (std::abs(pred - std::round(pred)) < 1e-9) &&
                     (std::abs(pred - (*s.ys)[j]) < 0.5);
            }

            int32_t ex  = (*s.example_ids)[j];
            int32_t row = (*s.row_idxs)[j];
            if (ok) {
                good_idx.push_back(row);
            } else {
                bad_idx.push_back(row);
                total_mm++;
                per_ex_mm[ex]++;
                consistent = false;
            }
        }

        // ── Fill scalar outputs ───────────────────────────────────────────────
        slope_num_d[i]     = fr.slope.num;
        slope_den_d[i]     = fr.slope.den;
        intercept_num_d[i] = fr.intercept.num;
        intercept_den_d[i] = fr.intercept.den;
        mismatches_d[i]    = total_mm;
        n_good_d[i]        = (int32_t)good_idx.size();
        n_bad_d[i]         = (int32_t)bad_idx.size();
        consistent_d[i]    = consistent;
        r2_d[i]            = fr.r2;

        // ── per_example_mm: sorted by example_id ─────────────────────────────
        std::vector<int32_t> sorted_ex(seen_ex.begin(), seen_ex.end());
        std::vector<int32_t> per_mm_vals;
        per_mm_vals.reserve(sorted_ex.size());
        for (auto eid : sorted_ex) {
            auto it = per_ex_mm.find(eid);
            per_mm_vals.push_back(it != per_ex_mm.end() ? it->second : 0);
        }

        AppendList<int32_t>(*entries[9],  i, per_mm_vals);
        AppendList<int32_t>(*entries[10], i, good_idx);
        AppendList<int32_t>(*entries[11], i, bad_idx);
    }
}

static void EqAggDestroy(Vector &state_vec, AggregateInputData &, idx_t count) {
    auto *states = FlatVector::GetData<data_ptr_t>(state_vec);
    for (idx_t i = 0; i < count; i++) {
        reinterpret_cast<EquationAggState *>(states[i])->Destroy();
    }
}

// ─── Registration ─────────────────────────────────────────────────────────────

void RegisterEquationAgg(ExtensionLoader &loader) {
    child_list_t<LogicalType> result_children;
    result_children.emplace_back("slope_num",     LogicalType::BIGINT);
    result_children.emplace_back("slope_den",     LogicalType::BIGINT);
    result_children.emplace_back("intercept_num", LogicalType::BIGINT);
    result_children.emplace_back("intercept_den", LogicalType::BIGINT);
    result_children.emplace_back("mismatches",    LogicalType::INTEGER);
    result_children.emplace_back("n_good",        LogicalType::INTEGER);
    result_children.emplace_back("n_bad",         LogicalType::INTEGER);
    result_children.emplace_back("consistent",    LogicalType::BOOLEAN);
    result_children.emplace_back("r2",            LogicalType::DOUBLE);
    result_children.emplace_back("per_example_mm", LogicalType::LIST(LogicalType::INTEGER));
    result_children.emplace_back("good_indices",   LogicalType::LIST(LogicalType::INTEGER));
    result_children.emplace_back("bad_indices",    LogicalType::LIST(LogicalType::INTEGER));
    auto result_type = LogicalType::STRUCT(std::move(result_children));

    AggregateFunction agg(
        "equation_agg",
        /* args */        {LogicalType::DOUBLE, LogicalType::DOUBLE,
                           LogicalType::INTEGER, LogicalType::INTEGER,
                           LogicalType::INTEGER},
        /* return_type */ result_type,
        /* state_size */  AggregateFunction::StateSize<EquationAggState>,
        /* init */        EqAggInit,
        /* update */      EqAggUpdate,
        /* combine */     EqAggCombine,
        /* finalize */    EqAggFinalize,
        /* simple_upd */  nullptr,
        /* bind */        nullptr,
        /* destructor */  EqAggDestroy
    );

    loader.RegisterFunction(agg);
}

} // namespace duckdb
