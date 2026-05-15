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

extern "C" {

DUCKDB_EXTENSION_API void snapreg_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadStaticExtension<duckdb::SnapregExtension>();
}

// v1.5+ loadable extension entry point
DUCKDB_EXTENSION_API void snapreg_duckdb_cpp_init(duckdb::DatabaseInstance &db) {
	snapreg_init(db);
}

DUCKDB_EXTENSION_API const char *snapreg_version() {
	return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
