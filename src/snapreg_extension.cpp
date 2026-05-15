#define DUCKDB_EXTENSION_MAIN

#include "snapreg_extension.hpp"
#include "duckdb.hpp"

namespace duckdb {

void RegisterSnapRational(ExtensionLoader &loader);
void RegisterEquationAgg(ExtensionLoader &loader);
void RegisterResidualHint(ExtensionLoader &loader);

void SnapregExtension::Load(ExtensionLoader &loader) {
    RegisterSnapRational(loader);
    RegisterEquationAgg(loader);
    RegisterResidualHint(loader);
}

std::string SnapregExtension::Version() const {
#ifdef EXT_VERSION_SNAPREG
    return EXT_VERSION_SNAPREG;
#else
    return "dev";
#endif
}

} // namespace duckdb

// ─────────────────────────────────────────────────────────────
// Modern loadable extension entry point
// ─────────────────────────────────────────────────────────────
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(snapreg, loader) {
    duckdb::SnapregExtension ext;
    ext.Load(loader);
}

} // extern "C"