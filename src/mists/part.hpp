#pragma once

#include <stage/typesafe.hpp>
#include <typeindex>

struct PartTypeTag;
using PartType = doi::Typesafe<PartTypeTag, std::type_index>;

/// Component of an Entity.
class Part {
public:
	inline virtual ~Part() = 0;
	using Type = PartType;
};

inline Part::~Part() {}
