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
template<typename Heuristic, typename NodeData>
bool BFPrio(const Path<NodeData>& a, const Path<NodeData>& b) {
	return Heuristic::get(last(a)) < Heuristic::get(last(b));
}

// Returns the f-value (cost + heuristic of last node) of the
// given path
template<typename Heuristic, typename NodeData>
float fvalue(const Path<NodeData>& a) {
	return cost(a) + Heuristic::get(last(a));
}

// A* uses a priority queue, handling the paths with the lowest
// f-value first. f-values are simply the addition of path cost
// and a heurstic (of a node to the nearest goal node) at the
// end of the path, approximating the total cost to the goal
// on the given path.
// Optimal, complete, O(b^m) for space and time.
template<typename Heuristic, typename NodeData>
bool AStarPrio(const Path<NodeData>& a, const Path<NodeData>& b) {
	return fvalue(a) <= fvalue(b);
}

// Iterative deepening, DFS variation
// Complete, Optimal (given that all arcs have equal cost)
// O(b^m) time complexity
// O(bm) space complexity (yeay! that's the advantage)
template<typename NodeData, typename GoalPred>
std::optional<Path<NodeData>> searchIDS(const Node<NodeData>& root, GoalPred isGoal) {
	using DFS = AdapterDFS<NodeData>;
	auto depth = 1;
	auto discarded = true; // whether there are too long paths

	// if we never discarded a path, we have fully explored
	// the tree. We didn't find a solution, so there is none
	while(discarded) {
		auto todo = DFS::create(Path{root, {}});
		discarded = false;
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
				discarded = true;
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

// Branch & Bound
// Uses f-value to discard paths that are worse than the best found solution.
// Will diverge when there are cycles.
// Even though it uses heuristics to discard paths, it is optimal, since
// the heuristic it admissable and dicarded paths can't lead to a
// goal node with less cost than the currently selected solution.
// Not complete though, same as DFS.
// Memory complexity: O(bm), since it is based of DFS
// Time complexity: O(b^m)
template<typename Heuristic, typename NodeData, typename GoalPred>
std::optional<Path<NodeData>> searchBnB(const Node<NodeData>& root, GoalPred isGoal) {
	using DFS = AdapterDFS<NodeData>;

	// the upper bound: the best solution we have found so far
	auto ub = std::numeric_limits<float>::infinity();
	auto todo = DFS::create(Path{root, {}});
	std::optional<Path<NodeData>> best = std::nullopt;

	while(!todo.empty()) {
		auto& path = DFS::pop(todo);
		if(fvalue<Heuristic>(path) > ub) {
			continue;
		}

		auto& node = last(path);
		if(isGoal(node)) {
			ub = cost(path);
			best = path;
			continue; // we don't have to check longer paths
		}

		for(auto& arc : node.arcs) {
			auto copy = path;
			copy.arcs.push_back(arc);
			DFS::push(todo, copy);
		}
	}

	return best;
}

// IDA*: iterative deepening A*
// Not really related to A* though, except for the fact that
// it uses f-values.
// Basically like IDS but uses f-values instead of depth.
// Solves the problem of non-completeness BnB has.
// Memory: O(bm)
// Time: O(b^m), but much worse factor than e.g. A*
template<typename Heuristic, typename NodeData, typename GoalPred>
std::optional<Path<NodeData>> searchIDAStar(const Node<NodeData>& root,
		GoalPred isGoal) {
	using DFS = AdapterDFS<NodeData>;
	auto thresh = fvalue<Heuristic>(root);
	while(true) {
		auto next = std::numeric_limits<float>::infinity();
		auto todo = DFS::create(Path{root, {}});
		while(!todo.empty()) {
			auto& path = DFS::pop(todo);
			auto fv = fvalue(path);
			if(fv > thresh) {
				next = std::min(next, fv);
				continue;
			}

			auto& node = last(path);
			if(isGoal(node)) {
				return {path};
			}

			for(auto& arc : node.arcs) {
				auto copy = path;
				copy.arcs.push_back(arc);
				DFS::push(todo, copy);
			}
		}

		// we haven't discarded any paths, i.e. fully
		// explored the graph and not found a solution
		if(thresh == next) {
			break;
		}

		thresh = next;
	}

	return std::nullopt;
}

// TODO: not finished, just a sketch
// Memory-bounded A* search data structure.
// Has a limit on the number of nodes to store in total.
// If that is exceeded at some points, discards the worst paths.
// Still O(b^m) memory complexity but less of a problem in reality
template<typename NodeData, typename Prio>
struct AdapterMBAStar {
	struct Queue {
		std::priority_queue<Path<NodeData>, Prio> queue;
		unsigned numNodes;

		bool empty() const {
			return queue.empty();
		}
	};

	using Path = Path<NodeData>;
	static constexpr auto nodeLimit = 1024 * 1024;

	Queue create(const Path& p) {
		return {{p}, 0};
	}

	Path pop(Queue& q) {
		auto front = q.queue.front();
		q.queue.pop();
		return front;
	}

	void push(Queue& q, Path p) {
		auto n = q.numNodes + p.arcs.size() + 1;
		q.push(p);
		if(n > nodeLimit) {
			// TODO: implement discarding of worse paths
			// we probably have to switch to std::deque
			// and the std::make_heap, std::push_heap etc
			// functions for iterating over all paths

			// TODO: add ability to update heuristic of common
			// ancestor we prune the paths to?
			// Add custom heuristic that uses an unordered_map?
			// but then we use additional space again, probably
			// not worth it like that...
			// Maybe add optional Heuristic::update(node, value)
			// function to Heuristic interface? Heuristics usually
			// don't store values though.
		}
	}
};

// implement path pruning
// optionally add cycle checking (and path pruning) where useful.
