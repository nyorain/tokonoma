// WIP (barely started) implementation of iq's floating bar concept

#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>

namespace doi {

using u32 = std::uint32_t;

struct fb32 {
	unsigned sign : 1;
	unsigned bar : 5;
	unsigned nums : 26;
};

static_assert(sizeof(fb32) == 4u, "Compiler doesn't implement packing");

fb32 fb32div(unsigned sign, unsigned nom, unsigned denom) {
	// TODO: use integer log, stdlib function for that?
	unsigned obn = std::log2(nom);
	unsigned obd = std::log2(denom);
	if(obd == 0) { // dividing by zero, infinity
		return {sign, 31, 0};
	}

	auto bn = obn;
	auto bd = obd - 1u; // we know that obd > 0
	if(bn + bd > 26) {
		if(bn > bd) {
			bd = std::min(bd, 13u);
			bn = 26 - bd;
		} else {
			bn = std::min(bn, 13u);
			bd = 26 - bn;
		}
	}

	fb32 ret;
	ret.sign = sign;
	ret.bar = 26 - bd;
	// unset first bit of denominator (implicitly stored)
	// then shift to abandon bits we can't store
	ret.nums = (denom & ~(1 << obd)) >> ((obd - 1) - bd);
	// first shift away the bits we don't need, then
	// shift it to its position
	ret.nums |= ((nom >> (obn - bn)) << ret.bar);
	return ret;
}

struct fb32components {
	unsigned sign;
	unsigned nom;
	unsigned denom;
};

fb32components components(fb32 bits) {
	fb32components ret;
	ret.sign = bits.sign;
	ret.denom = (bits.nums >> bits.bar);
	ret.nom = (u32(bits.nums) << (32 - bits.bar)) >> (32 - bits.bar);
	return ret;
}

} // namespace doi
