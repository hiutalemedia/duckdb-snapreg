#include "duckdb.hpp"
#include "rational.hpp"

namespace duckdb {

static void SnapRationalFunction(DataChunk &args, ExpressionState &, Vector &result) {
    idx_t count = args.size();

    auto &v_vec   = args.data[0];
    auto &den_vec = args.data[1];

    auto &entries = StructVector::GetEntries(result);
    auto *num_data = FlatVector::GetData<int64_t>(*entries[0]);
    auto *den_data = FlatVector::GetData<int64_t>(*entries[1]);
    auto &validity     = FlatVector::Validity(result);
    auto &num_validity = FlatVector::Validity(*entries[0]);
    auto &den_validity = FlatVector::Validity(*entries[1]);

    UnifiedVectorFormat v_fmt, d_fmt;
    v_vec.ToUnifiedFormat(count, v_fmt);
    den_vec.ToUnifiedFormat(count, d_fmt);

    auto vs   = UnifiedVectorFormat::GetData<double> (v_fmt);
    auto dens = UnifiedVectorFormat::GetData<int32_t>(d_fmt);

    for (idx_t i = 0; i < count; i++) {
        auto vi = v_fmt.sel->get_index(i);
        auto di = d_fmt.sel->get_index(i);

        if (!v_fmt.validity.RowIsValid(vi) || !d_fmt.validity.RowIsValid(di)) {
            validity.SetInvalid(i);
            num_validity.SetInvalid(i);
            den_validity.SetInvalid(i);
            continue;
        }

        SnapResult r = snap_to_rational(vs[vi], dens[di]);
        if (!r.valid) {
            validity.SetInvalid(i);
            num_validity.SetInvalid(i);
            den_validity.SetInvalid(i);
            continue;
        }

        num_data[i] = r.r.num;
        den_data[i] = r.r.den;
    }
}

void RegisterSnapRational(ExtensionLoader &loader) {
    child_list_t<LogicalType> struct_children;
    struct_children.emplace_back("num", LogicalType::BIGINT);
    struct_children.emplace_back("den", LogicalType::BIGINT);
    auto result_type = LogicalType::STRUCT(std::move(struct_children));

    ScalarFunction fn("snap_rational",
                      {LogicalType::DOUBLE, LogicalType::INTEGER},
                      result_type,
                      SnapRationalFunction);
    fn.null_handling = FunctionNullHandling::SPECIAL_HANDLING;

    loader.RegisterFunction(fn);
}

} // namespace duckdb
