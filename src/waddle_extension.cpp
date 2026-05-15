#define DUCKDB_EXTENSION_MAIN

#include "snapreg_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void SnapregScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void SnapregOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Snapreg " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto snapreg_scalar_function =
	    ScalarFunction("snapreg", {LogicalType::VARCHAR}, LogicalType::VARCHAR, SnapregScalarFun);

	loader.RegisterFunction(snapreg_scalar_function);

	// Register another scalar function
	auto snapreg_openssl_version_scalar_function = ScalarFunction("snapreg_openssl_version", {LogicalType::VARCHAR},
	                                                              LogicalType::VARCHAR, SnapregOpenSSLVersionScalarFun);
	loader.RegisterFunction(snapreg_openssl_version_scalar_function);
}

void SnapregExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string SnapregExtension::Name() {
	return "snapreg";
}

std::string SnapregExtension::Version() const {
#ifdef EXT_VERSION_WADDLE
	return EXT_VERSION_WADDLE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(snapreg, loader) {
	duckdb::LoadInternal(loader);
}
}
