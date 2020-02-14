#include <vector>
#include <optional>

#include <queue>
#include <stack>

// Learning for the CPSC 322 midterm by just coding down the stuff.

// Directed graph data structure
template<typename NodeData>
struct Node;

template<typename NodeData>
struct Arc {
	float cost;
	Node<NodeData>* next;
};

template<typename NodeData>
struct Node {
	NodeData data;
	std::vector<Arc<NodeData>> arcs;
};

template<typename NodeData>
struct Path {
	Node<NodeData> root;
	std::vector<Arc<NodeData>> arcs;
};

// Returns the full cost of the given path so far
template<typename NodeData>
float cost(const Path<NodeData>& path) {
	float cost = 0.f;
	for(auto& arc : path.arcs) {
		cost += arc.cost;
	}
	return cost;
}

// Returns the last node of the given path
template<typename NodeData>
Node<NodeData>& last(const Path<NodeData>& path) {
	return path.arcs.empty() ? path.root : *path.arcs.back().next;
}

// Returns whether the given path has a cycle.
template<typename NodeData>
bool hasCycle(const Path<NodeData>& path) {
	for(auto i = 0u; i < path.arcs.length(); ++i) {
		if(path.arcs[i].next == &path.root) {
			return true;
		}

		for(auto j = 0u; j < i; ++j) {
			if(path.arcs[j].next == &path.arcs[i].next) {
				return true;
			}
		}
	}

	return false;
}

// Abstract searching algorithm
// Adapter: the used data structure. By varying this, DFS, BFS,
// A* or whatever can be implemented.
// Needs 3 static methods:
// - DataStructure Adapter::create(Path)
// - Path Adapter::pop(DataStructure&)
// - void Adapter::push(DataStructure&, Path)
// GoalPred: (const Node&) -> bool, returns whether given node is goal node
template<typename Adapter, typename NodeData, typename GoalPred>
std::optional<Path<NodeData>> search(const Node<NodeData>& root, GoalPred isGoal) {
	auto todo = Adapter::create(Path{root, {}});
	while(!todo.empty()) {
		auto& path = Adapter::pop(todo);
		auto& node = last(path);
		if(isGoal(node)) {
			return {path};
		}

		for(auto& arc : node.arcs) {
			auto copy = path;
			copy.arcs.push_back(arc);
			Adapter::push(todo, copy);
		}
	}

	return std::nullopt;
}

// Data structure variants
// b: maximal branching factor in tree (max number of arcs)
// m: maximal depth in tree (longest non-circular path in graph)
// space complexity always measured in number of paths.
// technically (for real space complexity) there should be
// a 'm' factor in the big-O notation i guess.
// Heuristics are assumed to be admissable. They are modeled stateless,
// i.e. can only access the nodes inherent data (NodeData).

// Depth-first search. Not complete, not optimal.
// Time complexity O(b^m)
// space complexity O(bm)
template<typename NodeData>
struct AdapterDFS {
	using Queue = std::queue<Path<NodeData>>;
	using Path = Path<NodeData>;

	Queue create(const Path& p) {
		return {p};
	}

	Path pop(Queue& q) {
		auto front = q.front();
		q.pop();
		return front;
	}

	void push(Queue& q, Path p) {
		q.push(p);
	}
};

// Breadth-first search. Complete, optimal (for constant arc cost).
// Time complexity O(b^m)
// Space complexity O(b^m)
// Can be seen as LCF with constant arc cost.
template<typename NodeData>
struct AdapterBFS {
	using Stack = std::stack<Path<NodeData>>;
	using Path = Path<NodeData>;

	Stack create(const Path& p) {
		return {p};
	}

	Path pop(Stack& q) {
		auto front = q.front();
		q.pop();
		return front;
	}

	void push(Stack& q, Path p) {
		q.push(p);
	}
};

template<typename NodeData, typename Prio>
struct AdapterPrio {
	using Queue = std::priority_queue<Path<NodeData>, Prio>;
	using Path = Path<NodeData>;

	Queue create(const Path& p) {
		return {p};
	}

	Path pop(Queue& q) {
		auto front = q.front();
		q.pop();
		return front;
	}

	void push(Queue& q, Path p) {
		q.push(p);
	}
};

// Lowest cost first search uses a priority queue, handling the
// paths with currently lowest cost first.
// Could be seen as AStar with trivial (constant) heuristic.
// Optimal, complete, O(b^m) for space and time.
template<typename NodeData>
bool LCFPrio(const Path<NodeData>& a, const Path<NodeData>& b) {
	return cost(a) <= cost(b);
}

// Best-first search: uses only heuristics of the end node of
// a path to evaluate which path should be chosen next.
// Not complete (since it can enter infinite cycles),
// not optimal (since heuristic is independent from real path cost).
// O(b^m) for space and time.
// Shittier version of A* i guess?
template<typename NodeData, typename Heuristic>
bool BFPrio(const Path<NodeData>& a, const Path<NodeData>& b) {
	return Heuristic::get(last(a)) < Heuristic::get(last(b));
}

// A* uses a priority queue, handling the paths with the lowest
// f-value first. f-values are simply the addition of path cost
// and a heurstic (of a node to the nearest goal node) at the
// end of the path, approximating the total cost to the goal
// on the given path.
// Optimal, complete, O(b^m) for space and time.
template<typename NodeData, typename Heuristic>
bool AStarPrio(const Path<NodeData>& a, const Path<NodeData>& b) {
	auto fa = cost(a) + Heuristic::get(last(a));
	auto fb = cost(b) + Heuristic::get(last(b));
	return fa <= fb;
}

// Iterative deepening, DFS variation
// Complete, Optimal (given that all arcs have equal cost)
// O(b^m) time complexity
// O(bm) space complexity (yeay! that's the advantage)
template<typename NodeData, typename GoalPred>
std::optional<Path<NodeData>> searchIDS(const Node<NodeData>& root, GoalPred isGoal) {
	using DFS = AdapterDFS<NodeData>;
	auto depth = 1;
	while(true) {
		auto todo = DFS::create(Path{root, {}});
		while(!todo.empty()) {
			auto& path = DFS::pop(todo);
			auto& node = last(path);
			if(isGoal(node)) {
				return {path};
			}

			// the inner search is basically exactly the same as the usual
			// search (with DFS), except for this part: discard
			// paths that are too long. We well check them in the
			// next step.
			if(path.arcs.length() >= depth) {
				continue;
			}

			for(auto& arc : node.arcs) {
				auto copy = path;
				copy.arcs.push_back(arc);
				DFS::push(todo, copy);
			}
		}

		++depth;
	}

	return std::nullopt;
}

// TODO: branch and bound
// TODO: IDA* (IDS with f-values)
// TODO: MBA* (memory bound A*)
//
// implement path pruning
// optionally add cycle checking (and path pruning) where useful.
