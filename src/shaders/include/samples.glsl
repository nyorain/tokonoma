// NOTE: theoretically we can reduce it to half the size even,
// the other points are just axis mirrored.

// first 2 samples of 1+4 linear sample mask.
// the other 2 are just mirrored
const vec2 samplesLinear4[] = {
	{-1.5f, +0.5f},
	{-0.5f, -1.5f},
};

// first 4 samples of 1+8 linear sample mask.
// the other 4 are just mirrored
const vec2 samplesLinear8[] = {
	{-1.5f, +0.5f},
	{-1.5f, +2.5f},
	{+0.5f, +1.5f},
	{+2.5f, +1.5f},
};

/*
const ivec2 samplesLinear8Floor[] = {
	{-1, +1},
	{-1, +3},
	{+1, +2},
	{+3, +2},

	{+2, -0},
	{+2, -2},
	{-0, -1},
	{-2, -1},
};

// first 6 samples of 1+12 linear sample mask
// other 6 are mirrored
const vec2 samplesLinear12[] = {
	{-3.5f, +0.5f},
	{-1.5f, +0.5f},
	{-1.5f, +2.5f},
	{+0.5f, +3.5f},
	{+0.5f, +1.5f},
	{+2.5f, +1.5f},
};
*/
