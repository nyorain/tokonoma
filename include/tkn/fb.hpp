#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <ostream>
#include <numeric>

// WIP implementation of iq's floating bar concept

namespace tkn {

using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

// TODO: standard doesn't guarantee packing to 32 bits though
// could just use u32 and (mem)copy when needed
// TODO: to match int representation this has to depend on endianess i guess
// probably just do it manually, don't rely on bitfields... like in f16
struct fb32 {
public:
	unsigned sign : 1;
	unsigned bar : 5;
	unsigned nums : 26;

public:
	constexpr fb32() : sign(0), bar(0), nums(0) {} // zero
	explicit constexpr fb32(unsigned s, unsigned b, unsigned n) :
		sign(s), bar(b), nums(n) {}
	explicit constexpr inline fb32(int);

	// NOTE: should probably better be explicit, right?
	constexpr inline operator double() const;
};

static_assert(sizeof(fb32) == 4u, "Compiler doesn't implement packing");

// returns the number of bits needed to represent number (0 for 0)
// does not work for signed numbers (i.e. intergers!)
constexpr unsigned numbits(int) = delete;

template<typename T>
constexpr unsigned numbits(T u) {
	auto ret = 0u;
	while(u) {
		u >>= 1;
		++ret;
	}
	return ret;
}

// TODO: still off (by factor 2) sometimes, investigate!
inline constexpr fb32 fb32pack(unsigned sign, u64 nom, u64 denom) {
	if(denom == 0) { // dividing by zero, infinity
		return fb32{sign, 31, 0};
	}

	// TODO: really hurts performance here
	// implement alternative approaches like iq described, e.g. just
	// checking the first few primes for common divisors
	auto gcd = std::gcd(nom, denom);
	nom /= gcd;
	denom /= gcd;

	unsigned obn = numbits(nom);
	unsigned obd = numbits(denom);

	// unset first bit of denominator, it is stored implicitly
	--obd;

	auto bn = obn;
	auto bd = obd;
	if(bn + bd > 26) {
		// we might "waste" a bit here (the implicitly stored denom bit)
		auto diff = int(bn) - bd;
		bn = unsigned(26 + diff) >> 1u;
		bd = unsigned(26 - diff) >> 1u;
	}

	denom >>= (obd - bd);
	nom >>= (obn - bn);

	// we could run gcd here (again) but then have to adapt
	// the bit widths
	// TODO: performaaaaance...
	gcd = std::gcd(nom, denom);
	nom /= gcd;
	denom /= gcd;
	bd = numbits(denom) - 1;

	// unset first bit
	denom &= ~(u64(1) << bd);

	fb32 ret {};
	ret.sign = sign;
	ret.bar = bd;
	ret.nums = (nom << ret.bar) | denom;
	return ret;
}

inline constexpr fb32 fb32div(int nom, int denom) {
	auto sign = (nom < 0) ^ (denom < 0);
	nom = (nom < 0) ? -nom : nom;
	denom = (denom < 0) ? -denom : denom;
	return fb32pack(sign, nom, denom);
}

struct fb32components {
	unsigned sign;
	unsigned nom;
	unsigned denom;
};

inline constexpr fb32components components(fb32 bits) {
	fb32components ret {};
	ret.sign = bits.sign;
	ret.nom = (bits.nums >> bits.bar);
	ret.denom = u32(bits.nums) & ((1 << bits.bar) - 1);
	ret.denom |= (1u << bits.bar); // add implicit first bit
	return ret;
}

// inline constexpr fb32::operator float() const {
// 	auto c = components(*this);
// 	return (c.sign ? int(-c.nom) : int(c.nom)) / float(c.denom);
// }

inline constexpr fb32::operator double() const {
	auto c = components(*this);
	return (c.sign ? int(-c.nom) : int(c.nom)) / double(c.denom);
}

// inline constexpr fb32::operator int() const {
// 	auto c = components(*this);
// 	return (c.sign ? int(-c.nom) : int(c.nom)) / int(c.denom);
// }

// inline constexpr bool operator==(fb32 a, float b) {
// 	return float(a) == b;
// }
//
// inline constexpr bool operator==(fb32 a, double b) {
// 	return float(a) == b;
// }

inline constexpr bool operator==(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return ca.sign == cb.sign &&
		(u64(ca.nom) * cb.denom == u64(ca.denom) * cb.nom);
}

inline constexpr bool operator!=(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return ca.sign != cb.sign ||
		(u64(ca.nom) * cb.denom != u64(ca.denom) * cb.nom);
}

inline constexpr bool operator<=(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return (!ca.sign && cb.sign) ||
		(u64(ca.nom) * cb.denom <= u64(ca.denom) * cb.nom);
}

inline constexpr bool operator>=(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return (ca.sign && !cb.sign) ||
		(u64(ca.nom) * cb.denom >= u64(ca.denom) * cb.nom);
}

inline constexpr bool operator>(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return (ca.sign && !cb.sign) ||
		(u64(ca.nom) * cb.denom > u64(ca.denom) * cb.nom);
}

inline constexpr bool operator<(fb32 a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return (!ca.sign && cb.sign) ||
		(u64(ca.nom) * cb.denom < u64(ca.denom) * cb.nom);
}

inline constexpr fb32 operator-(fb32 v) {
	v.sign ^= 1;
	return v;
}

// TODO: performance can be improved for this operator
inline constexpr fb32& operator+=(fb32& a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	auto n1 = u64(ca.nom) * cb.denom;
	auto n2 = u64(cb.nom) * ca.denom;
	auto denom = u64(ca.denom) * cb.denom;
	// auto denom = std::lcm(u64(ca.denom), u64(cb.denom));
	// auto n1 = ca.nom * (denom /ca.denom);
	// auto n2 = cb.nom * (denom /cb.denom);
	if(ca.sign == cb.sign) {
		return a = fb32pack(ca.sign, n1 + n2, denom);
	}

	// auto sign = (ca.sign == (n1 > n2));
	auto i1 = i64(n1) * (ca.sign ? -1 : 1);
	auto i2 = i64(n2) * (cb.sign ? -1 : 1);
	auto sum = i1 + i2;
	return a = fb32pack(sum < 0, sum < 0 ? -sum : sum, denom);
}

inline constexpr fb32& operator-=(fb32& a, fb32 b) {
	return a += (-b);
}

inline constexpr fb32& operator*=(fb32& a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return a = fb32pack(ca.sign ^ cb.sign,
		u64(ca.nom) * cb.nom, u64(ca.denom) * cb.denom);
}

inline constexpr fb32& operator/=(fb32& a, fb32 b) {
	auto ca = components(a);
	auto cb = components(b);
	return a = fb32pack(ca.sign ^ cb.sign,
		u64(ca.nom) * cb.denom, u64(ca.denom) * cb.nom);
}

inline std::ostream& operator<<(std::ostream& os, fb32 v) {
	auto c = components(v);
	if(c.sign) {
		os << "-";
	}
	os << c.nom;
	os << "/";
	os << c.denom;
	return os;
}

inline constexpr fb32 operator+(fb32 a, fb32 b) { return a += b; }
inline constexpr fb32 operator-(fb32 a, fb32 b) { return a -= b; }
inline constexpr fb32 operator*(fb32 a, fb32 b) { return a *= b; }
inline constexpr fb32 operator/(fb32 a, fb32 b) { return a /= b; }

constexpr inline fb32::fb32(int i) : sign(i < 0), bar(0), nums(0) {
	unsigned u = (i < 0) ? -i : i;
	unsigned n = numbits(u);
	if(n > 26) {
		u >>= (n - 26);
	}
	nums = u;
}

} // namespace tkn
