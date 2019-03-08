/// XXX: wip, mainly concepts. Mainly belonging to sed project
const uint childNone = 0xFFFFFFFF;
struct OctreeNode {
	// if chiildren[0] == childNone, this is a leaf node.
	// then the other 7 children can contains objects.
	// this means there can never be more than 7 objects per cell.
	uint children[8];

	// XXX: padding hurts...
	// any way to get around needing the parent?
	uint parent;
	float _1;
	float _2;
	float _3;
};

// TODO: maybe use grid of octrees?
layout(set = 0, binding = 0) uniform Octree {
	vec3 size; // total octree size
	OctreeNode nodes[];
} octree;

// e.g. for now: only able to use (transformed) boxes
layout(set = 0, binding = 1) uniform Objects {
	Box boxes[];
} objects;

// TODO: return more intersection information like normal, pos, t
Box first(Ray ray) {
	OctreeNode current = octree.nodes[0];
	vec3 center = vec3(0.0); // center of current octree node
	vec3 hsize = 0.5 * octree.size; // halfsize of current node

	while(current.children[0] != childNone) {
		uint child = 0;
		hsize *= 0.5;
		if(ray.origin.x > center.x) {
			child += 4;
			center.x += 2 * hsize.x;
		}
		if(ray.origin.y > center.y) {
			child += 2;
			center.y += 2 * hsize.y;
		}
		if(ray.origin.z > center.z) {
			child += 1;
			center.z += 2 * hsize.z;
		}
		center -= hsize;
		current = octree.nodes[current.children[child]];
	}

	// TODO: keep track of tested boxes?
	// last 8 or 16 or sth. are enough i guess

	// try all objects
	bool found = false;
	float mint = 1.f / 0.f;
	Box best;
	for(int i = 0; i < boxes.length(); ++i) {
		if(current.children[i] == childNone) {
			break;
		}

		vec3 bnormal;
		t = intersect(ray, boxes[i], bnormal);
		if(t > 0.0) {
			best = boxes[i];
		}
	}

	if(found) {
		return best;
	}

	// TODO: now traverse
	// roughly https://daeken.svbtle.com/a-stupidly-simple-fast-octree-traversal-for-ray-intersection
}

