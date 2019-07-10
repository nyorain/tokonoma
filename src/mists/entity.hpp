#pragma once

#include "part.hpp"
#include <tkn/deriveCast.hpp>
#include <dlg/dlg.hpp>
#include <typeindex>
#include <cstddef>
#include <unordered_map>
#include <type_traits>

/// Holds an id and can have parts.
/// Deriving classes define in which way the parts are stored.
class Entity {
public:
	Entity() = default;
	virtual ~Entity() = default;

	// abstract
	/// Returns a part of the entity or null if it is not
	/// supported/known/existent. The part is expected to be
	/// valid for the rest of the frame.
	inline virtual Part* part(PartType) = 0;
	inline virtual const Part* part(PartType) const = 0;

	// utility
	inline Part& expectPart(PartType);
	inline const Part& expectPart(PartType) const;

	template<typename T> T* part();
	template<typename T> const T* part() const;

	template<typename T> T& expectPart();
	template<typename T> const T& expectPart() const;
};

/// Entity that has no parts by default.
class EmptyEntity : public Entity {
public:
	using Entity::Entity;
	Part* part(PartType) override { return nullptr; }
	const Part* part(PartType) const override { return nullptr; }
};


// - implementation -
inline Part* Entity::part(PartType) { return nullptr; }
inline const Part* Entity::part(PartType) const { return nullptr; }

inline Part& Entity::expectPart(PartType type) {
	auto p = part(type);
	dlg_assertm(p, "Entity::expectPart: {}", type.value.name());
	return *p;
}

inline const Part& Entity::expectPart(PartType type) const {
	auto p = part(type);
	dlg_assertm(p, "Entity::expectPart: {}", type.value.name());
	return *p;
}

template<typename T> T* Entity::part() {
	return tkn::deriveCast<T*>(part({typeid(T)}), "Entity::part");
}

template<typename T> const T* Entity::part() const {
	return tkn::deriveCast<const T*>(part({typeid(T)}), "Entity::part");
}

template<typename T> T& Entity::expectPart() {
	return tkn::deriveCast<T&>(part({typeid(T)}), "Entity::expectPart");
}

template<typename T> const T& Entity::expectPart() const {
	return tkn::deriveCast<const T&>(expectPart({typeid(T)}),
			"Entity::expectPart");
}
