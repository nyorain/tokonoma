#pragma once

#include <tkn/typesafe.hpp>
#include <typeindex>

struct PartTypeTag;
using PartType = tkn::Typesafe<PartTypeTag, std::type_index>;

/// Component of an Entity.
class Part {
public:
	inline virtual ~Part() = 0;
	using Type = PartType;
};

inline Part::~Part() {}
