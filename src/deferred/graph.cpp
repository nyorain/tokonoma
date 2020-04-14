#include "graph.hpp"
#include <dlg/dlg.hpp>
#include <iomanip>

std::string graphDot(const FrameGraph& graph) {
	std::string ret = "strict digraph {\n";
	for(auto& pass : graph.passes()) {
		auto it = std::find_if(graph.order().begin(), graph.order().end(),
			[&](auto& o) { return o.pass == &pass; });
		dlg_assert(it != graph.order().end());
		auto num = it - graph.order().begin();
		auto nb = it->barriers.size();
		ret += dlg::format("{} [label = {}];", std::quoted(pass.name),
			std::quoted(dlg::format("{} ({}, {} barriers)", pass.name, num, nb)));

		for(auto& in : pass.in()) {
			ret += dlg::format("{} -> {};\n",
				std::quoted(in.target->producer.pass->name),
				std::quoted(pass.name));
		}
	}

	ret += "\n}";
	return ret;
}
