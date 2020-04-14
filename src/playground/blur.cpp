#include <cmath>
#include <cassert>
#include <vector>
#include <iostream>

float gauss(float x, float isigma2) {
	return std::exp(-x * x * isigma2);
}

std::vector<unsigned> pascal(unsigned line) {
	std::vector<unsigned> ret;
	unsigned last = 1;
	ret.push_back(last);

	for(auto i = 1u; i < line; ++i) {
		last = last * (line - i) / i;
		ret.push_back(last);
	}

	return ret;
}

// might only works for positive numbers
float roundOdd(float x) {
	return std::floor(x / 2) * 2 + 1;
}

int main() {
	unsigned hsize = 4;

	// for best quality (what we learned in cv): sigma = hsize / 3
	// for an even larger blur (at loss of quality): sigma = hsize
	// float sigma = hsize / 1.5f;
	// float isigma2 = 1.0 / (sigma * sigma);
//
	// std::vector<float> weights;
	// weights.reserve(1 + hsize);
	// for(auto i = 0u; i <= hsize; ++i) {
	// 	weights.push_back(gauss(i, isigma2));
	// }
//
	// auto sum = 0.f;
	// auto first = true;
	// for(auto w : weights) {
	// 	sum += w;
	// 	if(!first) sum += w;
	// 	first = false;
	// }
//
	// std::cout << "original weights: \n";
	// for(auto& w : weights) {
	// 	w /= sum;
	// 	std::cout << w << "\n";
	// }


	// NOTE: all of this could be calculcated more effeciently using
	// pascals triangle. But there i don't really control the sigma
	// i guess?

	std::vector<float> weights;
	{
		auto tc = 1 + 2 * hsize;
		auto nc = unsigned(roundOdd(1.5 * tc));
		std::cout << nc << "\n";
		auto p = pascal(nc);
		assert(p.size() == nc);

		for(auto w : p) {
			std::cout << w << "\n";
		}

		auto start = (nc - 1) / 2;
		float sum = p[start];
		for(auto i = start + 1; i < start + hsize + 1; ++i) {
			sum += 2 * p[i];
		}

		weights.reserve(1 + hsize);

		for(auto i = start; i < start + hsize + 1; ++i) {
			weights.push_back(p[i] / sum);
		}

		std::cout << "original weights: \n";
		for(auto w : weights) {
			std::cout << w << "\n";
		}
	}

	std::cout << "new weights: \n";
	for(auto i = 1u; i < 1 + hsize; i += 2) {
		std::cout << weights[i] + weights[i + 1] << "\n";
	}

	std::cout << "offsets: \n";
	for(auto i = 1u; i < 1 + hsize; i += 2) {
		float o = i * weights[i] + (i + 1) * weights[i + 1];
		o /= weights[i] + weights[i + 1];
		std::cout << o << "\n";
	}
}
