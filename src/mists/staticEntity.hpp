#pragma once

#include "fwd.hpp"
#include "entity.hpp"
#include <nytl/tmpUtil.hpp>

/// Entity implementation deriving from 'Base' (usually directly Entity)
/// that has 'Parts' (a tuple) as statically stored parts.
/// 'Assoc' (all types must be PartAssoc types) can be used to associate
/// PartTypes with the stored parts (e.g. to make parts::Rigid receivable
/// via parts::Rigid PartType as well as the parts::Transform PartType).
/// By default (and if 'DefaultAssoc' is true), for every Part in 'Parts',
/// 'DefaultAssoc' (or just the part itself if there is no such type in Part)
/// will be automatically added as association.
/// Examples:
/// ```
/// using RigidEntity = BasicStaticEntity<std::tuple<parts::Rigid>>;
/// using CustomEntity = BasicStaticEntity<
/// 	std::tuple<parts::Rigid, parts::SomeTransformImpl>, // parts
///		SomeOtherEntityBase, // base
///		std::tuple< // custom associations
///			PartAssoc<parts::Rigid, parts::Rigid>,
///			PartAssoc<parts::SomeTransformImpl, parts::Transform>
///		>,
/// 	false // don't just default associations
///	>;
/// ```
template<typename Parts,  // the parts to add as tuple
	typename Base = Entity,  // the base entity class
	typename Assoc = std::tuple<>, // Additional PartAssocs
	bool DefaultAssoc = true // Whether to use Part::DefaultAssocs
> class BasicStaticEntity;

template<
	typename... Parts,
	typename Base,
	typename... Assoc,
	bool DefaultAssoc>
class BasicStaticEntity<
	std::tuple<Parts...>,
	Base,
	std::tuple<Assoc...>,
	DefaultAssoc> : public Base {
public:
	static_assert(std::is_base_of_v<Entity, Base>);
	using EntityBase = BasicStaticEntity;

public:
	using Base::Base;

	Part* part(PartType) override;
	const Part* part(PartType) const override;

public:
	// The parts can also directly be accessed.
	std::tuple<Parts...> parts;

protected:
	static const std::unordered_map<PartType, usize> offsets_;
};

/// Simple utility typedef.
/// Example:
/// ``` using RigidEntity = StaticEntity<parts::Rigid>; ```
template<typename... Parts> using StaticEntity =
	BasicStaticEntity<std::tuple<Parts...>>;



// -- implementation --
// Dear reader, you probably have no interest in wandering beyond this point.
namespace detail {

template<typename T> using HasDefaultAssoc = typename T::DefaultAssoc;
using OMap = std::unordered_map<PartType, usize>;

// EnterOffsets
template<typename Assoc>
struct EnterOffsets;

template<typename P, typename... Parts>
struct EnterOffsets<std::tuple<P, Parts...>> {
	static void call(OMap& map, usize offset) {
		EnterOffsets<std::tuple<Parts...>>::call(map, offset);
		map[{typeid(P)}] = offset;
	}
};

template<>
struct EnterOffsets<std::tuple<>> {
	static void call(OMap&, usize) {}
};

// EnterCustomOffsets
template<typename E, typename Assocs>
struct EnterCustomOffsets;

template<typename E, typename Part, typename... Assocs, typename... Tail>
struct EnterCustomOffsets<E,
		std::tuple<PartAssoc<Part, Assocs...>, Tail...>> {

	static void call(OMap& map) {
		const auto offset =
			reinterpret_cast<usize>(&(std::get<Part>(
				static_cast<E*>(nullptr)->parts)));
		EnterOffsets<std::tuple<Assocs...>>::call(map, offset);
		EnterCustomOffsets<E, std::tuple<Tail...>>::call(map);
	}
};

template<typename E>
struct EnterCustomOffsets<E, std::tuple<>> {
	static void call(OMap&) {}
};

// CreateOffsetMap
template<typename E, typename Parts>
struct EnterDefaultOffsets;

template<typename E, typename P, typename... Parts>
struct EnterDefaultOffsets<E, std::tuple<P, Parts...>> {
	static void call(OMap& map) {
		const auto offset =
			reinterpret_cast<usize>(&(std::get<P>(
				static_cast<E*>(nullptr)->parts)));

		if constexpr(nytl::validExpression<HasDefaultAssoc, P>) {
			EnterOffsets<typename P::DefaultAssoc>::call(map, offset);
		} else {
			EnterOffsets<std::tuple<P>>::call(map, offset);
		}

		EnterDefaultOffsets<E, std::tuple<Parts...>>::call(map);
	}
};

template<typename E>
struct EnterDefaultOffsets<E, std::tuple<>> {
	static void call(OMap&) {}
};

// create func
template<
	typename Parts,
	typename Base,
	typename Assocs,
	bool DefaultAssoc>
OMap createOffsetMap() {
	using Entity = BasicStaticEntity<Parts, Base, Assocs, DefaultAssoc>;

	OMap map;
	EnterCustomOffsets<Entity, Assocs>::call(map);
	if constexpr(DefaultAssoc) {
		EnterDefaultOffsets<Entity, Parts>::call(map);
	}

	return map;
}

} // namespace detail

template<
	typename... Parts,
	typename Base,
	typename... PartAssoc,
	bool DefaultAssoc>
const std::unordered_map<PartType, usize>
BasicStaticEntity<
	std::tuple<Parts...>,
	Base,
	std::tuple<PartAssoc...>,
	DefaultAssoc>::offsets_ = detail::createOffsetMap<
		std::tuple<Parts...>,
		Base,
		std::tuple<PartAssoc...>,
		DefaultAssoc>();

template<
	typename... Parts,
	typename Base,
	typename... Assoc,
	bool DefaultAssoc>
Part*
BasicStaticEntity<
	std::tuple<Parts...>,
	Base,
	std::tuple<Assoc...>,
	DefaultAssoc>
::part(PartType type) {
	// derived class can override base parts
	auto it = offsets_.find(type);
	if(it != offsets_.end()) {
		auto p = reinterpret_cast<std::byte*>(this) + it->second;
		return reinterpret_cast<Part*>(p);
	}

	// Entity has no parts by default
	if constexpr(!std::is_same_v<Base, Entity>) {
		auto bp = Base::part(type);
		if(bp) {
			return bp;
		}
	}

	return nullptr;
}

template<
	typename... Parts,
	typename Base,
	typename... Assoc,
	bool DefaultAssoc>
const Part*
BasicStaticEntity<
	std::tuple<Parts...>,
	Base,
	std::tuple<Assoc...>,
	DefaultAssoc>
::part(PartType type) const {
	// derived class can override base parts
	auto it = offsets_.find(type);
	if(it != offsets_.end()) {
		auto p = reinterpret_cast<const std::byte*>(this) + it->second;
		return reinterpret_cast<const Part*>(p);
	}

	// Entity has no parts by default
	if constexpr(!std::is_same_v<Base, Entity>) {
		auto bp = Base::part(type);
		if(bp) {
			return bp;
		}
	}

	return nullptr;
}
