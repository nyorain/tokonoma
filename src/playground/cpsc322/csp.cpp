#include <unordered_set>
#include <vector>

struct Variable {
	std::unordered_set<int> domain;
};

// Only unary/binary constraints supported
using BinaryPredicate = bool(*)(int, int);

struct Constraint {
	unsigned var1;
	unsigned var2;
	BinaryPredicate pred;
};

std::vector<int> csp(std::vector<Variable>& vars,
		std::vector<Constraint>& constraints) {
}
