#include <functional>
#include <deque>
#include <vector>
#include <array>
#include <cstdio>
#include <algorithm>
#include <cassert>
#include <unordered_set>

using Domain = int;
using Predicate = bool(*)(Domain, Domain);

struct Constraint {
	unsigned var1;
	unsigned var2;
	Predicate pred;
};

struct Variable {
	std::vector<unsigned> constraints; // ids into constraint vector
	std::unordered_set<Domain> domain;
};

void arcConsistency(std::vector<Variable> vars,
		const std::vector<Constraint>& constraints) {
	struct Arc {
		unsigned var;
		unsigned constraint;
	};

	// we use a queue as data structure
	std::deque<Arc> todo;

	// insert arcs to be initially checked: all arcs
	for(auto i = 0u; i < vars.size(); ++i) {
		for(auto& c : vars[i].constraints) {
			todo.push_back({i, c});
		}
	}

	while(!todo.empty()) {
		auto arc = todo.front();
		todo.pop_front();

		auto& var = vars[arc.var];
		auto& constraint = constraints[arc.constraint];
		assert(constraint.var1 == arc.var || constraint.var2 == arc.var);

		auto oid = constraint.var1 == arc.var ? constraint.var2 : constraint.var1;
		auto& other = vars[oid];

		for(auto& val1 : var.domain) {
			auto found = false;
			for(auto& val2 : other.domain) {
				if(constraint.pred(val1, val2)) {
					found = true;
					break;
				}
			}
			if(found) {
				continue;
			}

			// erase the value that can never be used to fulfill the
			// constraint from the variables domain
			var.domain.erase(val1);

			// add all arcs from *other* constraints connected
			// to the current variable to the todo list, they have
			// to be checked again.
			for(auto& cid : var.constraints) {
				// no need to re-check this arc. If it previously
				// was valid, it is still valid because we only removed
				// a value that didn't fulfill the constraint with any
				// value from the other variables domain anyways.
				if(cid == arc.constraint) {
					continue;
				}

				auto& constraint = constraints[cid];
				auto oid = constraint.var1 == arc.var ?
					constraint.var2 : constraint.var1;

				// check whether the arc is already in the
				// todo list.
				// TODO: seems inefficient, there is probably a better way
				auto present = false;
				for(auto& t : todo) {
					if(t.var == oid && t.constraint == cid) {
						present = true;
						break;
					}
				}
				if(!present) {
					todo.push_back({oid, cid});
				}
			}
		}
	}
}
