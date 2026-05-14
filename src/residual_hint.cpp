#include "duckdb.hpp"
#include "rational.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace duckdb {

// ─── residual_hint(vals DOUBLE[]) →
//   STRUCT(
//     range_tight     BOOLEAN,   -- max-min <= 2
//     nearly_constant BOOLEAN,   -- variance < 0.25
//     nearly_linear   BOOLEAN,   -- r² vs index > 0.90
//     binary_valued   BOOLEAN,   -- exactly 2 distinct (rounded) values
//     n_distinct      INTEGER,
//     min_val         DOUBLE,
//     max_val         DOUBLE,
//     mean_val        DOUBLE
//   )
// Returns NULL on empty input.

static void ResidualHintFunction(DataChunk &args, ExpressionState &, Vector &result) {
    idx_t count = args.size();
    auto &list_vec = args.data[0];

    auto &entries   = StructVector::GetEntries(result);
    auto &validity  = FlatVector::Validity(result);

    auto *range_tight_d     = FlatVector::GetData<bool>  (*entries[0]);
    auto *nearly_constant_d = FlatVector::GetData<bool>  (*entries[1]);
    auto *nearly_linear_d   = FlatVector::GetData<bool>  (*entries[2]);
    auto *binary_valued_d   = FlatVector::GetData<bool>  (*entries[3]);
    auto *n_distinct_d      = FlatVector::GetData<int32_t>(*entries[4]);
    auto *min_val_d         = FlatVector::GetData<double>(*entries[5]);
    auto *max_val_d         = FlatVector::GetData<double>(*entries[6]);
    auto *mean_val_d        = FlatVector::GetData<double>(*entries[7]);

    UnifiedVectorFormat list_fmt;
    list_vec.ToUnifiedFormat(count, list_fmt);
    auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_fmt);

    auto &child_vec = ListVector::GetEntry(list_vec);
    UnifiedVectorFormat child_fmt;
    child_vec.ToUnifiedFormat(ListVector::GetListSize(list_vec), child_fmt);
    auto child_vals = UnifiedVectorFormat::GetData<double>(child_fmt);

    for (idx_t i = 0; i < count; i++) {
        auto li = list_fmt.sel->get_index(i);
        if (!list_fmt.validity.RowIsValid(li)) { validity.SetInvalid(i); continue; }

        auto &le = list_entries[li];
        if (le.length == 0) { validity.SetInvalid(i); continue; }

        // Collect valid values
        std::vector<double> vals;
        vals.reserve(le.length);
        for (idx_t j = 0; j < le.length; j++) {
            auto ci = child_fmt.sel->get_index(le.offset + j);
            if (child_fmt.validity.RowIsValid(ci)) vals.push_back(child_vals[ci]);
        }
        if (vals.empty()) { validity.SetInvalid(i); continue; }

        // Basic stats
        double mn = *std::min_element(vals.begin(), vals.end());
        double mx = *std::max_element(vals.begin(), vals.end());
        double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / vals.size();

        double var = 0;
        for (double v : vals) var += (v - mean) * (v - mean);
        var /= vals.size();

        // Distinct count (rounded to nearest int)
        std::vector<int64_t> rounded;
        for (double v : vals) rounded.push_back((int64_t)std::llround(v));
        std::sort(rounded.begin(), rounded.end());
        auto n_dist = (int32_t)std::distance(rounded.begin(),
                          std::unique(rounded.begin(), rounded.end()));

        // R² vs index (1-based)
        double r2_linear = 0.0;
        if (vals.size() >= 3) {
            double n = (double)vals.size();
            double sum_x = n * (n + 1) / 2.0;
            double sum_xx = n * (n + 1) * (2 * n + 1) / 6.0;
            double sum_y = n * mean;
            double sum_xy = 0;
            for (idx_t j = 0; j < vals.size(); j++) sum_xy += (j + 1) * vals[j];
            double denom_ols = n * sum_xx - sum_x * sum_x;
            if (std::abs(denom_ols) > 1e-12) {
                double slope_ols = (n * sum_xy - sum_x * sum_y) / denom_ols;
                double intercept_ols = (sum_y - slope_ols * sum_x) / n;
                double ss_res = 0, ss_tot = var * n;
                for (idx_t j = 0; j < vals.size(); j++) {
                    double pred = slope_ols * (j + 1) + intercept_ols;
                    ss_res += (vals[j] - pred) * (vals[j] - pred);
                }
                r2_linear = ss_tot > 1e-12 ? 1.0 - ss_res / ss_tot : 1.0;
            }
        }

        range_tight_d[i]     = (mx - mn) <= 2.0;
        nearly_constant_d[i] = var < 0.25;
        nearly_linear_d[i]   = r2_linear > 0.90;
        binary_valued_d[i]   = (n_dist == 2);
        n_distinct_d[i]      = n_dist;
        min_val_d[i]         = mn;
        max_val_d[i]         = mx;
        mean_val_d[i]        = mean;
    }
}

void RegisterResidualHint(ExtensionLoader &loader) {
    child_list_t<LogicalType> children;
    children.emplace_back("range_tight",     LogicalType::BOOLEAN);
    children.emplace_back("nearly_constant", LogicalType::BOOLEAN);
    children.emplace_back("nearly_linear",   LogicalType::BOOLEAN);
    children.emplace_back("binary_valued",   LogicalType::BOOLEAN);
    children.emplace_back("n_distinct",      LogicalType::INTEGER);
    children.emplace_back("min_val",         LogicalType::DOUBLE);
    children.emplace_back("max_val",         LogicalType::DOUBLE);
    children.emplace_back("mean_val",        LogicalType::DOUBLE);
    auto result_type = LogicalType::STRUCT(std::move(children));

    ScalarFunction fn("residual_hint",
                      {LogicalType::LIST(LogicalType::DOUBLE)},
                      result_type,
                      ResidualHintFunction);
    fn.null_handling = FunctionNullHandling::SPECIAL_HANDLING;

    loader.RegisterFunction(fn);
}

} // namespace duckdb
