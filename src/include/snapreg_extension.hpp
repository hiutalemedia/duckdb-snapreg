#pragma once
#include "duckdb.hpp"

namespace duckdb {

class SnapregExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override {
		return "snapreg";
	}
	std::string Version() const override;
};

} // namespace duckdb
