#include <nytl/flags.hpp>

// Indexing of corners
//
//      ^ Y
//      |
//      4-------------------5
//     /|                  /|
//    / |                 / |
//   /  |                /  |
//  7-------------------6   |
//  |   |               |   |
//  |   |               |   |
//  |   |               |   |
//  |   |               |   |
//  |   0---------------|---1--> X
//  |  /                |  /
//  | /                 | /
//  |/                  |/
//  3-------------------2
// /
//  Z

// Indexing of the edges
//
//      o--------4----------o
//     /|                  /|
//    7 |                 5 |
//   /  |                /  |
//  o--------6----------o   |
//  |   8               |   9
//  |   |               |   |
//  |   |               |   |
//  11  |               10  |
//  |   o--------0------|---o
//  |  /                |  /
//  | 3                 | 1
//  |/                  |/
//  o--------2----------o

enum class Edge {
	none = 0,
	e0 = 1 << 0,
	e1 = 1 << 1,
	e2 = 1 << 2,
	e3 = 1 << 3,
	e4 = 1 << 4,
	e5 = 1 << 5,
	e6 = 1 << 6,
	e7 = 1 << 7,
	e8 = 1 << 8,
	e9 = 1 << 9,
	e10 = 1 << 10,
	e11 = 1 << 11,
};

using EdgeFlags = nytl::Flags<Edge>;
NYTL_FLAG_OPS(Edge)

// indexed by edge
constexpr struct {
	unsigned a;
	unsigned b;
} edgePoints[] = {
	{0, 1}, // edge0
	{1, 2}, // edge1
	{2, 3}, // edge2
	{3, 0}, // edge3
	{4, 5}, // edge4
	{5, 6}, // edge5
	{6, 7}, // edge6
	{7, 4}, // edge7
	{0, 4}, // edge8
	{1, 5}, // edge9
	{2, 6}, // edge10
	{3, 7}, // edge11
};

// indexed by cube bitmask
// contains the values from Edge as integer bitmask
constexpr int edgeTable[256] = {
	0x0  , 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
	0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
	0x190, 0x99 , 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
	0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
	0x230, 0x339, 0x33 , 0x13a, 0x636, 0x73f, 0x435, 0x53c,
	0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
	0x3a0, 0x2a9, 0x1a3, 0xaa , 0x7a6, 0x6af, 0x5a5, 0x4ac,
	0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
	0x460, 0x569, 0x663, 0x76a, 0x66 , 0x16f, 0x265, 0x36c,
	0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
	0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff , 0x3f5, 0x2fc,
	0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
	0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55 , 0x15c,
	0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
	0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc ,
	0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
	0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
	0xcc , 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
	0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
	0x15c, 0x55 , 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
	0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
	0x2fc, 0x3f5, 0xff , 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
	0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
	0x36c, 0x265, 0x16f, 0x66 , 0x76a, 0x663, 0x569, 0x460,
	0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
	0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa , 0x1a3, 0x2a9, 0x3a0,
	0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
	0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33 , 0x339, 0x230,
	0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
	0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99 , 0x190,
	0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
	0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0   };

// indexed by cube bitmask
constexpr int triTable[256][16] = {
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 1, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 8, 3, 9, 8, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 2, 10, 0, 2, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{2, 8, 3, 2, 10, 8, 10, 9, 8, -1, -1, -1, -1, -1, -1, -1},
	{3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 11, 2, 8, 11, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 9, 0, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 11, 2, 1, 9, 11, 9, 8, 11, -1, -1, -1, -1, -1, -1, -1},
	{3, 10, 1, 11, 10, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 10, 1, 0, 8, 10, 8, 11, 10, -1, -1, -1, -1, -1, -1, -1},
	{3, 9, 0, 3, 11, 9, 11, 10, 9, -1, -1, -1, -1, -1, -1, -1},
	{9, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 3, 0, 7, 3, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 1, 9, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 1, 9, 4, 7, 1, 7, 3, 1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 4, 7, 3, 0, 4, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1},
	{9, 2, 10, 9, 0, 2, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
	{2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1, -1, -1, -1},
	{8, 4, 7, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{11, 4, 7, 11, 2, 4, 2, 0, 4, -1, -1, -1, -1, -1, -1, -1},
	{9, 0, 1, 8, 4, 7, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
	{4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1, -1, -1, -1},
	{3, 10, 1, 3, 11, 10, 7, 8, 4, -1, -1, -1, -1, -1, -1, -1},
	{1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1, -1, -1, -1},
	{4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1, -1, -1, -1},
	{4, 7, 11, 4, 11, 9, 9, 11, 10, -1, -1, -1, -1, -1, -1, -1},
	{9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 5, 4, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 5, 4, 1, 5, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{8, 5, 4, 8, 3, 5, 3, 1, 5, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 0, 8, 1, 2, 10, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
	{5, 2, 10, 5, 4, 2, 4, 0, 2, -1, -1, -1, -1, -1, -1, -1},
	{2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1, -1, -1, -1},
	{9, 5, 4, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 11, 2, 0, 8, 11, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
	{0, 5, 4, 0, 1, 5, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
	{2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1, -1, -1, -1},
	{10, 3, 11, 10, 1, 3, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1},
	{4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1, -1, -1, -1},
	{5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1, -1, -1, -1},
	{5, 4, 8, 5, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1},
	{9, 7, 8, 5, 7, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 3, 0, 9, 5, 3, 5, 7, 3, -1, -1, -1, -1, -1, -1, -1},
	{0, 7, 8, 0, 1, 7, 1, 5, 7, -1, -1, -1, -1, -1, -1, -1},
	{1, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 7, 8, 9, 5, 7, 10, 1, 2, -1, -1, -1, -1, -1, -1, -1},
	{10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1, -1, -1, -1},
	{8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1, -1, -1, -1},
	{2, 10, 5, 2, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1},
	{7, 9, 5, 7, 8, 9, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1},
	{9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1, -1, -1, -1},
	{2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1, -1, -1, -1},
	{11, 2, 1, 11, 1, 7, 7, 1, 5, -1, -1, -1, -1, -1, -1, -1},
	{9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1, -1, -1, -1},
	{5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
	{11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1},
	{11, 10, 5, 7, 11, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 0, 1, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 8, 3, 1, 9, 8, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
	{1, 6, 5, 2, 6, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 6, 5, 1, 2, 6, 3, 0, 8, -1, -1, -1, -1, -1, -1, -1},
	{9, 6, 5, 9, 0, 6, 0, 2, 6, -1, -1, -1, -1, -1, -1, -1},
	{5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1, -1, -1, -1},
	{2, 3, 11, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{11, 0, 8, 11, 2, 0, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
	{0, 1, 9, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
	{5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1, -1, -1, -1},
	{6, 3, 11, 6, 5, 3, 5, 1, 3, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1, -1, -1, -1},
	{3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1, -1, -1, -1},
	{6, 5, 9, 6, 9, 11, 11, 9, 8, -1, -1, -1, -1, -1, -1, -1},
	{5, 10, 6, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 3, 0, 4, 7, 3, 6, 5, 10, -1, -1, -1, -1, -1, -1, -1},
	{1, 9, 0, 5, 10, 6, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
	{10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1, -1, -1, -1},
	{6, 1, 2, 6, 5, 1, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1, -1, -1, -1},
	{8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1, -1, -1, -1},
	{7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1},
	{3, 11, 2, 7, 8, 4, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
	{5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1, -1, -1, -1},
	{0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1},
	{9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
	{8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1, -1, -1, -1},
	{5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
	{0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1},
	{6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1, -1, -1, -1},
	{10, 4, 9, 6, 4, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 10, 6, 4, 9, 10, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1},
	{10, 0, 1, 10, 6, 0, 6, 4, 0, -1, -1, -1, -1, -1, -1, -1},
	{8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1, -1, -1, -1},
	{1, 4, 9, 1, 2, 4, 2, 6, 4, -1, -1, -1, -1, -1, -1, -1},
	{3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1, -1, -1, -1},
	{0, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{8, 3, 2, 8, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1},
	{10, 4, 9, 10, 6, 4, 11, 2, 3, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1, -1, -1, -1},
	{3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1, -1, -1, -1},
	{6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1},
	{9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1, -1, -1, -1},
	{8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1},
	{3, 11, 6, 3, 6, 0, 0, 6, 4, -1, -1, -1, -1, -1, -1, -1},
	{6, 4, 8, 11, 6, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{7, 10, 6, 7, 8, 10, 8, 9, 10, -1, -1, -1, -1, -1, -1, -1},
	{0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1, -1, -1, -1},
	{10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1, -1, -1, -1},
	{10, 6, 7, 10, 7, 1, 1, 7, 3, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1, -1, -1, -1},
	{2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
	{7, 8, 0, 7, 0, 6, 6, 0, 2, -1, -1, -1, -1, -1, -1, -1},
	{7, 3, 2, 6, 7, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1, -1, -1, -1},
	{2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1},
	{1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
	{11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1, -1, -1, -1},
	{8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
	{0, 9, 1, 11, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1, -1, -1, -1},
	{7, 11, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 0, 8, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 1, 9, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{8, 1, 9, 8, 3, 1, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
	{10, 1, 2, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, 3, 0, 8, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
	{2, 9, 0, 2, 10, 9, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
	{6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1, -1, -1, -1},
	{7, 2, 3, 6, 2, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{7, 0, 8, 7, 6, 0, 6, 2, 0, -1, -1, -1, -1, -1, -1, -1},
	{2, 7, 6, 2, 3, 7, 0, 1, 9, -1, -1, -1, -1, -1, -1, -1},
	{1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1, -1, -1, -1},
	{10, 7, 6, 10, 1, 7, 1, 3, 7, -1, -1, -1, -1, -1, -1, -1},
	{10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1, -1, -1, -1},
	{0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1, -1, -1, -1},
	{7, 6, 10, 7, 10, 8, 8, 10, 9, -1, -1, -1, -1, -1, -1, -1},
	{6, 8, 4, 11, 8, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 6, 11, 3, 0, 6, 0, 4, 6, -1, -1, -1, -1, -1, -1, -1},
	{8, 6, 11, 8, 4, 6, 9, 0, 1, -1, -1, -1, -1, -1, -1, -1},
	{9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1, -1, -1, -1},
	{6, 8, 4, 6, 11, 8, 2, 10, 1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1, -1, -1, -1},
	{4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1, -1, -1, -1},
	{10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
	{8, 2, 3, 8, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1},
	{0, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1, -1, -1, -1},
	{1, 9, 4, 1, 4, 2, 2, 4, 6, -1, -1, -1, -1, -1, -1, -1},
	{8, 1, 3, 8, 6, 1, 8, 4, 6, 6, 10, 1, -1, -1, -1, -1},
	{10, 1, 0, 10, 0, 6, 6, 0, 4, -1, -1, -1, -1, -1, -1, -1},
	{4, 6, 3, 4, 3, 8, 6, 10, 3, 0, 3, 9, 10, 9, 3, -1},
	{10, 9, 4, 6, 10, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 9, 5, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, 4, 9, 5, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
	{5, 0, 1, 5, 4, 0, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
	{11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1, -1, -1, -1},
	{9, 5, 4, 10, 1, 2, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
	{6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1, -1, -1, -1},
	{7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1, -1, -1, -1},
	{3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1},
	{7, 2, 3, 7, 6, 2, 5, 4, 9, -1, -1, -1, -1, -1, -1, -1},
	{9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1, -1, -1, -1},
	{3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1, -1, -1, -1},
	{6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
	{9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1, -1, -1, -1},
	{1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
	{4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1},
	{7, 6, 10, 7, 10, 8, 5, 4, 10, 4, 8, 10, -1, -1, -1, -1},
	{6, 9, 5, 6, 11, 9, 11, 8, 9, -1, -1, -1, -1, -1, -1, -1},
	{3, 6, 11, 0, 6, 3, 0, 5, 6, 0, 9, 5, -1, -1, -1, -1},
	{0, 11, 8, 0, 5, 11, 0, 1, 5, 5, 6, 11, -1, -1, -1, -1},
	{6, 11, 3, 6, 3, 5, 5, 3, 1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 10, 9, 5, 11, 9, 11, 8, 11, 5, 6, -1, -1, -1, -1},
	{0, 11, 3, 0, 6, 11, 0, 9, 6, 5, 6, 9, 1, 2, 10, -1},
	{11, 8, 5, 11, 5, 6, 8, 0, 5, 10, 5, 2, 0, 2, 5, -1},
	{6, 11, 3, 6, 3, 5, 2, 10, 3, 10, 5, 3, -1, -1, -1, -1},
	{5, 8, 9, 5, 2, 8, 5, 6, 2, 3, 8, 2, -1, -1, -1, -1},
	{9, 5, 6, 9, 6, 0, 0, 6, 2, -1, -1, -1, -1, -1, -1, -1},
	{1, 5, 8, 1, 8, 0, 5, 6, 8, 3, 8, 2, 6, 2, 8, -1},
	{1, 5, 6, 2, 1, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1},
	{10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1, -1, -1, -1},
	{0, 3, 8, 5, 6, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{10, 5, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{11, 5, 10, 7, 5, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{11, 5, 10, 11, 7, 5, 8, 3, 0, -1, -1, -1, -1, -1, -1, -1},
	{5, 11, 7, 5, 10, 11, 1, 9, 0, -1, -1, -1, -1, -1, -1, -1},
	{10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1, -1, -1, -1},
	{11, 1, 2, 11, 7, 1, 7, 5, 1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1, -1, -1, -1},
	{9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1, -1, -1, -1},
	{7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1},
	{2, 5, 10, 2, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1},
	{8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1, -1, -1, -1},
	{9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1, -1, -1, -1},
	{9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
	{1, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 7, 0, 7, 1, 1, 7, 5, -1, -1, -1, -1, -1, -1, -1},
	{9, 0, 3, 9, 3, 5, 5, 3, 7, -1, -1, -1, -1, -1, -1, -1},
	{9, 8, 7, 5, 9, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{5, 8, 4, 5, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1},
	{5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1, -1, -1, -1},
	{0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1, -1, -1, -1},
	{10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1},
	{2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1, -1, -1, -1},
	{0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1},
	{0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
	{9, 4, 5, 2, 11, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1, -1, -1, -1},
	{5, 10, 2, 5, 2, 4, 4, 2, 0, -1, -1, -1, -1, -1, -1, -1},
	{3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1},
	{5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1, -1, -1, -1},
	{8, 4, 5, 8, 5, 3, 3, 5, 1, -1, -1, -1, -1, -1, -1, -1},
	{0, 4, 5, 1, 0, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1, -1, -1, -1},
	{9, 4, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 11, 7, 4, 9, 11, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1},
	{0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1, -1, -1, -1},
	{1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1, -1, -1, -1},
	{3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
	{4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1, -1, -1, -1},
	{9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
	{11, 7, 4, 11, 4, 2, 2, 4, 0, -1, -1, -1, -1, -1, -1, -1},
	{11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1, -1, -1, -1},
	{2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1, -1, -1, -1},
	{9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1},
	{3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
	{1, 10, 2, 8, 7, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 9, 1, 4, 1, 7, 7, 1, 3, -1, -1, -1, -1, -1, -1, -1},
	{4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1, -1, -1, -1},
	{4, 0, 3, 7, 4, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{4, 8, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{9, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 0, 9, 3, 9, 11, 11, 9, 10, -1, -1, -1, -1, -1, -1, -1},
	{0, 1, 10, 0, 10, 8, 8, 10, 11, -1, -1, -1, -1, -1, -1, -1},
	{3, 1, 10, 11, 3, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 2, 11, 1, 11, 9, 9, 11, 8, -1, -1, -1, -1, -1, -1, -1},
	{3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1, -1, -1, -1},
	{0, 2, 11, 8, 0, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{3, 2, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{2, 3, 8, 2, 8, 10, 10, 8, 9, -1, -1, -1, -1, -1, -1, -1},
	{9, 10, 2, 0, 9, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1, -1, -1, -1},
	{1, 10, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{1, 3, 8, 9, 1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 9, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{0, 3, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
	{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}
};

// the first table index is the cube configuration,
// the second index is for up to four dual points.
// Each value of a table entry encodes the edges with an associated dual point.
// A value of 0 indicates a dummy point.
EdgeFlags dualPointsList[256][4] = {
	{Edge::none, Edge::none, Edge::none, Edge::none}, // Edge::none
	{Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none, Edge::none}, // 1
	{Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 2
	{Edge::e1|Edge::e3|Edge::e8|Edge::e9, Edge::none, Edge::none, Edge::none}, // 3
	{Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none, Edge::none}, // 4
	{Edge::e1|Edge::e2|Edge::e10, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 5
	{Edge::e0|Edge::e2|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 6
	{Edge::e2|Edge::e3|Edge::e8|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 7
	{Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none, Edge::none}, // 8
	{Edge::e0|Edge::e2|Edge::e8|Edge::e11, Edge::none, Edge::none, Edge::none}, // 9
	{Edge::e0|Edge::e1|Edge::e9, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 10
	{Edge::e1|Edge::e2|Edge::e8|Edge::e9|Edge::e11, Edge::none, Edge::none, Edge::none}, // 11
	{Edge::e1|Edge::e3|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 12
	{Edge::e0|Edge::e1|Edge::e8|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 13
	{Edge::e0|Edge::e3|Edge::e9|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 14
	{Edge::e8|Edge::e9|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 15
	{Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none, Edge::none}, // 16
	{Edge::e0|Edge::e3|Edge::e4|Edge::e7, Edge::none, Edge::none, Edge::none}, // 17
	{Edge::e4|Edge::e7|Edge::e8, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 18
	{Edge::e1|Edge::e3|Edge::e4|Edge::e7|Edge::e9, Edge::none, Edge::none, Edge::none}, // 19
	{Edge::e4|Edge::e7|Edge::e8, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 20
	{Edge::e0|Edge::e3|Edge::e4|Edge::e7, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 21
	{Edge::e0|Edge::e2|Edge::e9|Edge::e10, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 22
	{Edge::e2|Edge::e3|Edge::e4|Edge::e7|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 23
	{Edge::e4|Edge::e7|Edge::e8, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 24
	{Edge::e2|Edge::e4|Edge::e7|Edge::e11|Edge::e0, Edge::none, Edge::none, Edge::none}, // 25
	{Edge::e4|Edge::e7|Edge::e8, Edge::e2|Edge::e3|Edge::e11, Edge::e0|Edge::e1|Edge::e9, Edge::none}, // 26
	{Edge::e1|Edge::e2|Edge::e4|Edge::e7|Edge::e9|Edge::e11, Edge::none, Edge::none, Edge::none}, // 27
	{Edge::e1|Edge::e3|Edge::e10|Edge::e11, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 28
	{Edge::e0|Edge::e1|Edge::e4|Edge::e7|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 29
	{Edge::e0|Edge::e3|Edge::e9|Edge::e10|Edge::e11, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 30
	{Edge::e4|Edge::e7|Edge::e9|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 31
	{Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none, Edge::none}, // 32
	{Edge::e4|Edge::e5|Edge::e9, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 33
	{Edge::e0|Edge::e1|Edge::e4|Edge::e5, Edge::none, Edge::none, Edge::none}, // 34
	{Edge::e1|Edge::e3|Edge::e4|Edge::e5|Edge::e8, Edge::none, Edge::none, Edge::none}, // 35
	{Edge::e4|Edge::e5|Edge::e9, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 36
	{Edge::e4|Edge::e5|Edge::e9, Edge::e1|Edge::e2|Edge::e10, Edge::e0|Edge::e3|Edge::e8, Edge::none}, // 37
	{Edge::e0|Edge::e2|Edge::e4|Edge::e5|Edge::e10, Edge::none, Edge::none, Edge::none}, // 38
	{Edge::e2|Edge::e3|Edge::e4|Edge::e5|Edge::e8|Edge::e10, Edge::none, Edge::none, Edge::none}, // 39
	{Edge::e4|Edge::e5|Edge::e9,Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 40
	{Edge::e0|Edge::e2|Edge::e8|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 41
	{Edge::e0|Edge::e1|Edge::e4|Edge::e5, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 42
	{Edge::e1|Edge::e2|Edge::e4|Edge::e5|Edge::e8|Edge::e11, Edge::none, Edge::none, Edge::none}, // 43
	{Edge::e1|Edge::e3|Edge::e10|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 44
	{Edge::e0|Edge::e1|Edge::e8|Edge::e10|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 45
	{Edge::e0|Edge::e4|Edge::e5|Edge::e3|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 46
	{Edge::e4|Edge::e5|Edge::e8|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 47
	{Edge::e5|Edge::e7|Edge::e8|Edge::e9, Edge::none, Edge::none, Edge::none}, // 48
	{Edge::e0|Edge::e3|Edge::e5|Edge::e7|Edge::e9, Edge::none, Edge::none, Edge::none}, // 49
	{Edge::e0|Edge::e1|Edge::e5|Edge::e7|Edge::e8, Edge::none, Edge::none, Edge::none}, // 50
	{Edge::e1|Edge::e3|Edge::e5|Edge::e7, Edge::none, Edge::none, Edge::none}, // 51
	{Edge::e5|Edge::e7|Edge::e8|Edge::e9, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 52
	{Edge::e0|Edge::e3|Edge::e5|Edge::e7|Edge::e9, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 53
	{Edge::e0|Edge::e2|Edge::e5|Edge::e7|Edge::e8|Edge::e10, Edge::none, Edge::none, Edge::none}, // 54
	{Edge::e2|Edge::e3|Edge::e5|Edge::e7|Edge::e10, Edge::none, Edge::none, Edge::none}, // 55
	{Edge::e5|Edge::e7|Edge::e8|Edge::e9, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 56
	{Edge::e0|Edge::e2|Edge::e11|Edge::e7|Edge::e5|Edge::e9, Edge::none, Edge::none, Edge::none}, // 57
	{Edge::e7|Edge::e8|Edge::e5|Edge::e0|Edge::e1, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 58
	{Edge::e1|Edge::e2|Edge::e5|Edge::e7|Edge::e11, Edge::none, Edge::none, Edge::none}, // 59
	{Edge::e5|Edge::e7|Edge::e8|Edge::e9, Edge::e1|Edge::e3|Edge::e10|Edge::e11, Edge::none, Edge::none}, // 60
	{Edge::e1|Edge::e10|Edge::e5|Edge::e9|Edge::e0|Edge::e11|Edge::e7, Edge::none, Edge::none, Edge::none}, // 61
	{Edge::e11|Edge::e3|Edge::e7|Edge::e8|Edge::e5|Edge::e0|Edge::e10, Edge::none, Edge::none, Edge::none}, // 62
	{Edge::e5|Edge::e7|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 63
	{Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none, Edge::none}, // 64
	{Edge::e5|Edge::e6|Edge::e10, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 65
	{Edge::e5|Edge::e6|Edge::e10, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 66
	{Edge::e1|Edge::e3|Edge::e8|Edge::e9, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 67
	{Edge::e1|Edge::e2|Edge::e5|Edge::e6, Edge::none, Edge::none, Edge::none}, // 68
	{Edge::e1|Edge::e2|Edge::e5|Edge::e6, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 69
	{Edge::e0|Edge::e2|Edge::e5|Edge::e6|Edge::e9, Edge::none, Edge::none, Edge::none}, // 70
	{Edge::e3|Edge::e8|Edge::e9|Edge::e2|Edge::e6|Edge::e5, Edge::none, Edge::none, Edge::none}, // 71
	{Edge::e5|Edge::e6|Edge::e10, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 72
	{Edge::e0|Edge::e2|Edge::e8|Edge::e11, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 73
	{Edge::e0|Edge::e1|Edge::e9, Edge::e5|Edge::e6|Edge::e10, Edge::e2|Edge::e3|Edge::e11, Edge::none}, // 74
	{Edge::e1|Edge::e2|Edge::e8|Edge::e9|Edge::e11, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 75
	{Edge::e1|Edge::e5|Edge::e6|Edge::e3|Edge::e11, Edge::none, Edge::none, Edge::none}, // 76
	{Edge::e0|Edge::e8|Edge::e11|Edge::e6|Edge::e5|Edge::e1|Edge::e0, Edge::none, Edge::none, Edge::none}, // 77
	{Edge::e5|Edge::e6|Edge::e3|Edge::e11|Edge::e0|Edge::e9, Edge::none, Edge::none, Edge::none}, // 78
	{Edge::e8|Edge::e9|Edge::e11|Edge::e5|Edge::e6, Edge::none, Edge::none, Edge::none}, // 79
	{Edge::e4|Edge::e7|Edge::e8, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 80
	{Edge::e0|Edge::e3|Edge::e4|Edge::e7, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 81
	{Edge::e4|Edge::e7|Edge::e8, Edge::e0|Edge::e1|Edge::e9, Edge::e5|Edge::e6|Edge::e10, Edge::none}, // 82
	{Edge::e4|Edge::e7|Edge::e3|Edge::e1|Edge::e9, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 83
	{Edge::e1|Edge::e2|Edge::e5|Edge::e6, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 84
	{Edge::e0|Edge::e3|Edge::e4|Edge::e7, Edge::e1|Edge::e2|Edge::e5|Edge::e6, Edge::none, Edge::none}, // 85
	{Edge::e6|Edge::e5|Edge::e0|Edge::e2|Edge::e9, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 86
	{Edge::e7|Edge::e3|Edge::e4|Edge::e6|Edge::e5|Edge::e2|Edge::e9, Edge::none, Edge::none, Edge::none}, // 87
	{Edge::e4|Edge::e7|Edge::e8, Edge::e5|Edge::e6|Edge::e10, Edge::e2|Edge::e3|Edge::e11, Edge::none}, // 88
	{Edge::e4|Edge::e7|Edge::e11|Edge::e2|Edge::e0, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 89
	{Edge::e4|Edge::e7|Edge::e8, Edge::e2|Edge::e3|Edge::e11, Edge::e5|Edge::e6|Edge::e10, Edge::e0|Edge::e1|Edge::e9}, // 90
	{Edge::e2|Edge::e11|Edge::e7|Edge::e4|Edge::e1|Edge::e9, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 91
	{Edge::e11|Edge::e3|Edge::e6|Edge::e5|Edge::e1, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 92
	{Edge::e7|Edge::e4|Edge::e11|Edge::e6|Edge::e5|Edge::e0|Edge::e1, Edge::none, Edge::none, Edge::none}, // 93
	{Edge::e11|Edge::e3|Edge::e5|Edge::e6|Edge::e0|Edge::e9, Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none}, // 94
	{Edge::e4|Edge::e7|Edge::e11|Edge::e5|Edge::e6|Edge::e9, Edge::none, Edge::none, Edge::none}, // 95
	{Edge::e4|Edge::e6|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 96
	{Edge::e4|Edge::e6|Edge::e9|Edge::e10, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 97
	{Edge::e0|Edge::e1|Edge::e4|Edge::e6|Edge::e10, Edge::none, Edge::none, Edge::none}, // 98
	{Edge::e3|Edge::e8|Edge::e1|Edge::e4|Edge::e6|Edge::e10, Edge::none, Edge::none, Edge::none}, // 99
	{Edge::e6|Edge::e2|Edge::e1|Edge::e4|Edge::e9, Edge::none, Edge::none, Edge::none}, // 100
	{Edge::e6|Edge::e2|Edge::e1|Edge::e4|Edge::e9, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 101
	{Edge::e0|Edge::e2|Edge::e4|Edge::e6, Edge::none, Edge::none, Edge::none}, // 102
	{Edge::e3|Edge::e8|Edge::e4|Edge::e6|Edge::e2, Edge::none, Edge::none, Edge::none}, // 103
	{Edge::e4|Edge::e9|Edge::e6|Edge::e10, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 104
	{Edge::e4|Edge::e9|Edge::e6|Edge::e10, Edge::e0|Edge::e2|Edge::e8|Edge::e11, Edge::none, Edge::none}, // 105
	{Edge::e6|Edge::e10|Edge::e4|Edge::e0|Edge::e1, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 106
	{Edge::e2|Edge::e11|Edge::e8|Edge::e1|Edge::e4|Edge::e10|Edge::e6, Edge::none, Edge::none, Edge::none}, // 107
	{Edge::e3|Edge::e11|Edge::e6|Edge::e4|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 108
	{Edge::e8|Edge::e0|Edge::e11|Edge::e6|Edge::e4|Edge::e9|Edge::e1, Edge::none, Edge::none, Edge::none}, // 109
	{Edge::e11|Edge::e3|Edge::e6|Edge::e4|Edge::e0, Edge::none, Edge::none, Edge::none}, // 110
	{Edge::e4|Edge::e6|Edge::e8|Edge::e11, Edge::none, Edge::none, Edge::none}, // 111
	{Edge::e7|Edge::e6|Edge::e8|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 112
	{Edge::e3|Edge::e7|Edge::e0|Edge::e6|Edge::e10|Edge::e9, Edge::none, Edge::none, Edge::none}, // 113
	{Edge::e7|Edge::e8|Edge::e0|Edge::e1|Edge::e10|Edge::e6, Edge::none, Edge::none, Edge::none}, // 114
	{Edge::e7|Edge::e3|Edge::e6|Edge::e10|Edge::e1, Edge::none, Edge::none, Edge::none}, // 115
	{Edge::e7|Edge::e8|Edge::e6|Edge::e9|Edge::e2|Edge::e1, Edge::none, Edge::none, Edge::none}, // 116
	{Edge::e7|Edge::e3|Edge::e0|Edge::e9|Edge::e1|Edge::e2|Edge::e6, Edge::none, Edge::none, Edge::none}, // 117
	{Edge::e7|Edge::e8|Edge::e0|Edge::e2|Edge::e6, Edge::none, Edge::none, Edge::none}, // 118
	{Edge::e7|Edge::e6|Edge::e3|Edge::e2, Edge::none, Edge::none, Edge::none}, // 119
	{Edge::e7|Edge::e8|Edge::e9|Edge::e6|Edge::e10, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 120
	{Edge::e7|Edge::e11|Edge::e2|Edge::e0|Edge::e6|Edge::e10|Edge::e9, Edge::none, Edge::none, Edge::none}, // 121
	{Edge::e7|Edge::e8|Edge::e6|Edge::e10|Edge::e0|Edge::e1, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 122
	{Edge::e7|Edge::e6|Edge::e10|Edge::e1|Edge::e2|Edge::e11, Edge::none, Edge::none, Edge::none}, // 123
	{Edge::e7|Edge::e8|Edge::e11|Edge::e6|Edge::e3|Edge::e9|Edge::e1, Edge::none, Edge::none, Edge::none}, // 124
	{Edge::e11|Edge::e6|Edge::e7, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 125
	{Edge::e7|Edge::e8|Edge::e11|Edge::e3|Edge::e6|Edge::e0, Edge::none, Edge::none, Edge::none}, // 126
	{Edge::e7|Edge::e6|Edge::e11, Edge::none, Edge::none, Edge::none}, // 127
	{Edge::e7|Edge::e6|Edge::e11, Edge::none, Edge::none, Edge::none}, // 128
	{Edge::e7|Edge::e6|Edge::e11, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 129
	{Edge::e7|Edge::e6|Edge::e11, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 130
	{Edge::e7|Edge::e6|Edge::e11, Edge::e1|Edge::e3|Edge::e8|Edge::e9, Edge::none, Edge::none}, // 131
	{Edge::e7|Edge::e6|Edge::e11, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 132
	{Edge::e7|Edge::e6|Edge::e11, Edge::e1|Edge::e2|Edge::e10, Edge::e0|Edge::e3|Edge::e8, Edge::none}, // 133
	{Edge::e7|Edge::e6|Edge::e11, Edge::e0|Edge::e2|Edge::e9|Edge::e10, Edge::none, Edge::none}, // 134
	{Edge::e7|Edge::e6|Edge::e11, Edge::e2|Edge::e3|Edge::e8|Edge::e9|Edge::e10, Edge::none, Edge::none}, // 135
	{Edge::e2|Edge::e3|Edge::e6|Edge::e7, Edge::none, Edge::none, Edge::none}, // 136
	{Edge::e0|Edge::e2|Edge::e6|Edge::e7|Edge::e8, Edge::none, Edge::none, Edge::none}, // 137
	{Edge::e2|Edge::e3|Edge::e6|Edge::e7, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 138
	{Edge::e1|Edge::e2|Edge::e6|Edge::e7|Edge::e8|Edge::e9, Edge::none, Edge::none, Edge::none}, // 139
	{Edge::e7|Edge::e6|Edge::e10|Edge::e1|Edge::e3, Edge::none, Edge::none, Edge::none}, // 140
	{Edge::e7|Edge::e6|Edge::e8|Edge::e0|Edge::e10|Edge::e1, Edge::none, Edge::none, Edge::none}, // 141
	{Edge::e3|Edge::e6|Edge::e7|Edge::e0|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 142
	{Edge::e7|Edge::e6|Edge::e8|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 143
	{Edge::e4|Edge::e6|Edge::e8|Edge::e11, Edge::none, Edge::none, Edge::none}, // 144
	{Edge::e0|Edge::e3|Edge::e4|Edge::e6|Edge::e11, Edge::none, Edge::none, Edge::none}, // 145
	{Edge::e4|Edge::e6|Edge::e8|Edge::e11, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 146
	{Edge::e6|Edge::e11|Edge::e3|Edge::e4|Edge::e9|Edge::e1, Edge::none, Edge::none, Edge::none}, // 147
	{Edge::e4|Edge::e6|Edge::e8|Edge::e11, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 148
	{Edge::e0|Edge::e3|Edge::e4|Edge::e6|Edge::e11, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 149
	{Edge::e4|Edge::e6|Edge::e8|Edge::e11, Edge::e0|Edge::e2|Edge::e9|Edge::e10, Edge::none, Edge::none}, // 150
	{Edge::e11|Edge::e6|Edge::e3|Edge::e4|Edge::e9|Edge::e10|Edge::e2, Edge::none, Edge::none, Edge::none}, // 151
	{Edge::e4|Edge::e6|Edge::e2|Edge::e3|Edge::e8, Edge::none, Edge::none, Edge::none}, // 152
	{Edge::e0|Edge::e2|Edge::e4|Edge::e6, Edge::none, Edge::none, Edge::none}, // 153
	{Edge::e4|Edge::e6|Edge::e2|Edge::e3|Edge::e8, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 154
	{Edge::e4|Edge::e6|Edge::e2|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 155
	{Edge::e4|Edge::e6|Edge::e10|Edge::e1|Edge::e8|Edge::e3, Edge::none, Edge::none, Edge::none}, // 156
	{Edge::e6|Edge::e4|Edge::e0|Edge::e1|Edge::e10, Edge::none, Edge::none, Edge::none}, // 157
	{Edge::e9|Edge::e0|Edge::e10|Edge::e6|Edge::e4|Edge::e8|Edge::e3, Edge::none, Edge::none, Edge::none}, // 158
	{Edge::e4|Edge::e6|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 159
	{Edge::e7|Edge::e6|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 160
	{Edge::e7|Edge::e6|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::e0|Edge::e3|Edge::e8, Edge::none}, // 161
	{Edge::e7|Edge::e6|Edge::e11, Edge::e0|Edge::e1|Edge::e4|Edge::e5, Edge::none, Edge::none}, // 162
	{Edge::e7|Edge::e6|Edge::e11, Edge::e8|Edge::e3|Edge::e1|Edge::e5|Edge::e4, Edge::none, Edge::none}, // 163
	{Edge::e6|Edge::e7|Edge::e11, Edge::e4|Edge::e5|Edge::e9, Edge::e1|Edge::e2|Edge::e10, Edge::none}, // 164
	{Edge::e6|Edge::e7|Edge::e11, Edge::e0|Edge::e3|Edge::e8, Edge::e1|Edge::e2|Edge::e10, Edge::e4|Edge::e5|Edge::e9}, // 165
	{Edge::e7|Edge::e6|Edge::e11, Edge::e4|Edge::e5|Edge::e0|Edge::e10|Edge::e2, Edge::none, Edge::none}, // 166
	{Edge::e7|Edge::e6|Edge::e11, Edge::e3|Edge::e8|Edge::e4|Edge::e5|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 167
	{Edge::e2|Edge::e3|Edge::e6|Edge::e7, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 168
	{Edge::e0|Edge::e2|Edge::e6|Edge::e7|Edge::e8, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 169
	{Edge::e2|Edge::e3|Edge::e6|Edge::e7, Edge::e0|Edge::e1|Edge::e4|Edge::e5, Edge::none, Edge::none}, // 170
	{Edge::e7|Edge::e6|Edge::e2|Edge::e8|Edge::e4|Edge::e5|Edge::e1, Edge::none, Edge::none, Edge::none}, // 171
	{Edge::e6|Edge::e7|Edge::e3|Edge::e10|Edge::e1, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 172
	{Edge::e0|Edge::e8|Edge::e6|Edge::e7|Edge::e1|Edge::e10, Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none}, // 173
	{Edge::e7|Edge::e6|Edge::e3|Edge::e10|Edge::e0|Edge::e4|Edge::e5, Edge::none, Edge::none, Edge::none}, // 174
	{Edge::e8|Edge::e7|Edge::e6|Edge::e10|Edge::e4|Edge::e5, Edge::none, Edge::none, Edge::none}, // 175
	{Edge::e11|Edge::e6|Edge::e5|Edge::e9|Edge::e8, Edge::none, Edge::none, Edge::none}, // 176
	{Edge::e11|Edge::e6|Edge::e5|Edge::e9|Edge::e0|Edge::e3, Edge::none, Edge::none, Edge::none}, // 177
	{Edge::e11|Edge::e6|Edge::e5|Edge::e8|Edge::e0|Edge::e1, Edge::none, Edge::none, Edge::none}, // 178
	{Edge::e11|Edge::e6|Edge::e5|Edge::e1|Edge::e3, Edge::none, Edge::none, Edge::none}, // 179
	{Edge::e11|Edge::e6|Edge::e5|Edge::e9|Edge::e8, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 180
	{Edge::e6|Edge::e11|Edge::e0|Edge::e3|Edge::e5|Edge::e9, Edge::e1|Edge::e10|Edge::e2, Edge::none, Edge::none}, // 181
	{Edge::e11|Edge::e6|Edge::e5|Edge::e10|Edge::e2|Edge::e8|Edge::e0, Edge::none, Edge::none, Edge::none}, // 182
	{Edge::e6|Edge::e11|Edge::e3|Edge::e2|Edge::e10|Edge::e5, Edge::none, Edge::none, Edge::none}, // 183
	{Edge::e2|Edge::e3|Edge::e6|Edge::e5|Edge::e9|Edge::e8, Edge::none, Edge::none, Edge::none}, // 184
	{Edge::e0|Edge::e2|Edge::e6|Edge::e5|Edge::e9, Edge::none, Edge::none, Edge::none}, // 185
	{Edge::e2|Edge::e6|Edge::e5|Edge::e1|Edge::e8|Edge::e0|Edge::e3, Edge::none, Edge::none, Edge::none}, // 186
	{Edge::e1|Edge::e2|Edge::e5|Edge::e6, Edge::none, Edge::none, Edge::none}, // 187
	{Edge::e6|Edge::e5|Edge::e10|Edge::e3|Edge::e1|Edge::e9|Edge::e8, Edge::none, Edge::none, Edge::none}, // 188
	{Edge::e6|Edge::e5|Edge::e9|Edge::e0|Edge::e1|Edge::e10, Edge::none, Edge::none, Edge::none}, // 189
	{Edge::e0|Edge::e3|Edge::e8, Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none}, // 190
	{Edge::e5|Edge::e6|Edge::e10, Edge::none, Edge::none, Edge::none}, // 191
	{Edge::e5|Edge::e7|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 192
	{Edge::e5|Edge::e7|Edge::e10|Edge::e11, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 193
	{Edge::e5|Edge::e7|Edge::e10|Edge::e11, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 194
	{Edge::e5|Edge::e7|Edge::e10|Edge::e11, Edge::e1|Edge::e3|Edge::e8|Edge::e9, Edge::none, Edge::none}, // 195
	{Edge::e11|Edge::e7|Edge::e5|Edge::e2|Edge::e1, Edge::none, Edge::none, Edge::none}, // 196
	{Edge::e11|Edge::e7|Edge::e5|Edge::e2|Edge::e1, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 197
	{Edge::e11|Edge::e7|Edge::e5|Edge::e2|Edge::e0|Edge::e9, Edge::none, Edge::none, Edge::none}, // 198
	{Edge::e3|Edge::e8|Edge::e11|Edge::e7|Edge::e5|Edge::e2|Edge::e9, Edge::none, Edge::none, Edge::none}, // 199
	{Edge::e7|Edge::e5|Edge::e10|Edge::e2|Edge::e3, Edge::none, Edge::none, Edge::none}, // 200
	{Edge::e7|Edge::e5|Edge::e10|Edge::e2|Edge::e0|Edge::e8, Edge::none, Edge::none, Edge::none}, // 201
	{Edge::e7|Edge::e5|Edge::e10|Edge::e2|Edge::e3, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 202
	{Edge::e7|Edge::e5|Edge::e10|Edge::e2|Edge::e8|Edge::e9|Edge::e1, Edge::none, Edge::none, Edge::none}, // 203
	{Edge::e1|Edge::e3|Edge::e5|Edge::e7, Edge::none, Edge::none, Edge::none}, // 204
	{Edge::e0|Edge::e1|Edge::e5|Edge::e7|Edge::e8, Edge::none, Edge::none, Edge::none}, // 205
	{Edge::e3|Edge::e7|Edge::e5|Edge::e0|Edge::e9, Edge::none, Edge::none, Edge::none}, // 206
	{Edge::e5|Edge::e7|Edge::e8|Edge::e9, Edge::none, Edge::none, Edge::none}, // 207
	{Edge::e4|Edge::e8|Edge::e5|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 208
	{Edge::e0|Edge::e3|Edge::e11|Edge::e4|Edge::e5|Edge::e10, Edge::none, Edge::none, Edge::none}, // 209
	{Edge::e4|Edge::e8|Edge::e11|Edge::e10|Edge::e5, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 210
	{Edge::e4|Edge::e5|Edge::e10|Edge::e1|Edge::e9|Edge::e11|Edge::e3, Edge::none, Edge::none, Edge::none}, // 211
	{Edge::e4|Edge::e8|Edge::e11|Edge::e5|Edge::e2|Edge::e1, Edge::none, Edge::none, Edge::none}, // 212
	{Edge::e11|Edge::e3|Edge::e2|Edge::e4|Edge::e5|Edge::e1|Edge::e0, Edge::none, Edge::none, Edge::none}, // 213
	{Edge::e11|Edge::e8|Edge::e2|Edge::e4|Edge::e0|Edge::e9|Edge::e5, Edge::none, Edge::none, Edge::none}, // 214
	{Edge::e4|Edge::e5|Edge::e9, Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none}, // 215
	{Edge::e4|Edge::e8|Edge::e3|Edge::e2|Edge::e10|Edge::e5, Edge::none, Edge::none, Edge::none}, // 216
	{Edge::e5|Edge::e10|Edge::e0|Edge::e2|Edge::e4, Edge::none, Edge::none, Edge::none}, // 217
	{Edge::e4|Edge::e8|Edge::e3|Edge::e2|Edge::e5|Edge::e10, Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none}, // 218
	{Edge::e4|Edge::e5|Edge::e10|Edge::e2|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 219
	{Edge::e4|Edge::e8|Edge::e5|Edge::e1|Edge::e3, Edge::none, Edge::none, Edge::none}, // 220
	{Edge::e0|Edge::e1|Edge::e4|Edge::e5, Edge::none, Edge::none, Edge::none}, // 221
	{Edge::e4|Edge::e8|Edge::e3|Edge::e5|Edge::e0|Edge::e9, Edge::none, Edge::none, Edge::none}, // 222
	{Edge::e4|Edge::e5|Edge::e9, Edge::none, Edge::none, Edge::none}, // 223
	{Edge::e4|Edge::e9|Edge::e10|Edge::e11|Edge::e7, Edge::none, Edge::none, Edge::none}, // 224
	{Edge::e4|Edge::e9|Edge::e10|Edge::e11|Edge::e7, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 225
	{Edge::e7|Edge::e11|Edge::e10|Edge::e4|Edge::e0|Edge::e1, Edge::none, Edge::none, Edge::none}, // 226
	{Edge::e7|Edge::e11|Edge::e10|Edge::e1|Edge::e4|Edge::e8|Edge::e3, Edge::none, Edge::none, Edge::none}, // 227
	{Edge::e7|Edge::e11|Edge::e4|Edge::e9|Edge::e2|Edge::e1, Edge::none, Edge::none, Edge::none}, // 228
	{Edge::e7|Edge::e11|Edge::e4|Edge::e9|Edge::e2|Edge::e1, Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none}, // 229
	{Edge::e7|Edge::e11|Edge::e2|Edge::e0|Edge::e4, Edge::none, Edge::none, Edge::none}, // 230
	{Edge::e7|Edge::e11|Edge::e3|Edge::e8|Edge::e4|Edge::e2, Edge::none, Edge::none, Edge::none}, // 231
	{Edge::e2|Edge::e3|Edge::e10|Edge::e9|Edge::e4|Edge::e7, Edge::none, Edge::none, Edge::none}, // 232
	{Edge::e7|Edge::e8|Edge::e4|Edge::e2|Edge::e0|Edge::e10|Edge::e9, Edge::none, Edge::none, Edge::none}, // 233
	{Edge::e7|Edge::e4|Edge::e3|Edge::e2|Edge::e0|Edge::e1|Edge::e10, Edge::none, Edge::none, Edge::none}, // 234
	{Edge::e4|Edge::e7|Edge::e8, Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none}, // 235
	{Edge::e7|Edge::e3|Edge::e1|Edge::e4|Edge::e9, Edge::none, Edge::none, Edge::none}, // 236
	{Edge::e7|Edge::e4|Edge::e8|Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 237
	{Edge::e0|Edge::e3|Edge::e4|Edge::e7, Edge::none, Edge::none, Edge::none}, // 238
	{Edge::e4|Edge::e7|Edge::e8, Edge::none, Edge::none, Edge::none}, // 239
	{Edge::e8|Edge::e9|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 240
	{Edge::e9|Edge::e10|Edge::e11|Edge::e0|Edge::e3, Edge::none, Edge::none, Edge::none}, // 241
	{Edge::e8|Edge::e10|Edge::e11|Edge::e0|Edge::e1, Edge::none, Edge::none, Edge::none}, // 242
	{Edge::e1|Edge::e3|Edge::e10|Edge::e11, Edge::none, Edge::none, Edge::none}, // 243
	{Edge::e8|Edge::e9|Edge::e11|Edge::e1|Edge::e2, Edge::none, Edge::none, Edge::none}, // 244
	{Edge::e11|Edge::e9|Edge::e3|Edge::e0|Edge::e1|Edge::e2, Edge::none, Edge::none, Edge::none}, // 245
	{Edge::e0|Edge::e2|Edge::e8|Edge::e11, Edge::none, Edge::none, Edge::none}, // 246
	{Edge::e2|Edge::e3|Edge::e11, Edge::none, Edge::none, Edge::none}, // 247
	{Edge::e3|Edge::e2|Edge::e8|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 248
	{Edge::e0|Edge::e2|Edge::e9|Edge::e10, Edge::none, Edge::none, Edge::none}, // 249
	{Edge::e8|Edge::e0|Edge::e3|Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none, Edge::none}, // 250
	{Edge::e1|Edge::e2|Edge::e10, Edge::none, Edge::none, Edge::none}, // 251
	{Edge::e1|Edge::e3|Edge::e8|Edge::e9, Edge::none, Edge::none, Edge::none}, // 252
	{Edge::e0|Edge::e1|Edge::e9, Edge::none, Edge::none, Edge::none}, // 253
	{Edge::e0|Edge::e3|Edge::e8, Edge::none, Edge::none, Edge::none}, // 254
	{Edge::none, Edge::none, Edge::none, Edge::none} // 255
};
