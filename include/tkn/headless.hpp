#pragma once

#include <vpp/handles.hpp>
#include <vpp/debug.hpp>
#include <argagg.hpp>
#include <memory>
#include <variant>

namespace tkn {

enum class DevType {
	igpu,
	dgpu,
	choose
};

struct HeadlessArgs {
	static argagg::parser defaultParser();

	HeadlessArgs() = default;
	HeadlessArgs(const argagg::parser_results&);

	bool renderdoc {};
	bool layers {};
	std::variant<DevType, unsigned, const char*> phdev = DevType::dgpu;
	std::vector<const char*> iniExts {};
	std::vector<const char*> devExts {};
};

struct Headless {
	Headless(const HeadlessArgs& args = {});

	vpp::Instance instance;
	std::unique_ptr<vpp::DebugMessenger> messenger;
	std::unique_ptr<vpp::Device> device;
};

} // namespace tkn
