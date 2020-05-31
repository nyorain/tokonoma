#pragma once

#include <vpp/handles.hpp>
#include <vpp/debug.hpp>
#include <argagg.hpp>
#include <memory>
#include <variant>
#include <functional>

namespace tkn {

struct Features;
enum class DevType {
	igpu,
	dgpu,
	choose
};

using FeatureChecker = std::function<bool(Features& enable,
		const Features& supported)>;

struct HeadlessArgs {
	static argagg::parser defaultParser();

	HeadlessArgs() = default;
	HeadlessArgs(const argagg::parser_results&);

	bool renderdoc {};
	bool layers {};
	std::variant<DevType, unsigned, const char*> phdev = DevType::choose;
	std::vector<const char*> iniExts {};
	std::vector<const char*> devExts {};
	FeatureChecker featureChecker {};
};

struct Headless {
	Headless(const HeadlessArgs& args = {});

	vpp::Instance instance;
	std::unique_ptr<vpp::DebugMessenger> messenger;
	std::unique_ptr<vpp::Device> device;
};

} // namespace tkn
