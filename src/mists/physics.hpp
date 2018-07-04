#pragma once

#include "fwd.hpp"
#include "part.hpp"
#include <nytl/nonCopyable.hpp>
#include <nytl/vec.hpp>

#include <Box2D/Dynamics/b2World.h>
#include <Box2D/Dynamics/b2Fixture.h>

class PhysicsSystem {
public:
	static constexpr auto velocityIterations = 6;
	static constexpr auto positionIterations = 4;
	b2World world {{0.f, 0.f}};

public:
	PhysicsSystem();
	~PhysicsSystem() = default;

	/// Updates all physics.
	/// Prefer to use a fixed delta for this every frame.
	void update(double delta);

protected:
	struct ContactListener : public b2ContactListener {
		void BeginContact(b2Contact* contact);
		void EndContact(b2Contact* contact);
	} contactListener;
};

namespace parts {

// interface part: returns the entities transform
struct Transform : public Part {
	virtual b2Transform transform(const Entity&) const = 0;
};

// data part: physical b2 body and fixture
class Rigid : public Transform, public nytl::NonCopyable {
public:
	using DefaultAssoc = std::tuple<Rigid, Transform>;

public:
	Rigid() = default;

	// TODO: remove in favor of init methods?
	Rigid(b2World&, const b2BodyDef&, const b2FixtureDef&);
	Rigid(b2World&, nytl::Vec2f, const b2Shape&, bool dynamic = true);
	~Rigid();

	// TODO: not sure if good idea.
	// if entity with rigid part is moved, the set user data
	// might get invalid...
	Rigid(Rigid&& rhs) noexcept;
	Rigid& operator=(Rigid rhs) noexcept;

	b2Transform transform(const Entity&) const override;
	friend void swap(Rigid& a, Rigid& b);

public:
	b2Body* body {};
	b2Fixture* fixture {};
};

/// Interface part for contact points (collision).
struct Contact : public Part {
	using DefaultAssoc = std::tuple<Contact>;

	struct Data {
		Entity& self; // the entity belonging to this listener
		Entity* other; // the other entity or nullptr if there is none
		b2Contact& contact; // information about the contact
		bool a; // whether this entity/listener is fixture a
	};

	/// Called when contact ends/begin.
	/// own entity, other entity (or null if no entity), contact
	/// The last bool signals whether this entity/listener is fixture a.
	virtual void begin(const Data&) {}
	virtual void end(const Data&) {}
};

} // namespace parts

/// Utility function that initializes the rigid part of an entity.
/// Will expect the given entity to have a rigid part.
/// Will also set the rigid's body user data to the entity, automatically
/// enabling the entities Contact implementation part (if there is any).
/// Can also be called on an already initialized rigid entity/part.
void initRigid(Entity&, b2World&, const b2BodyDef&, const b2FixtureDef&);
void initRigid(Entity&, b2World&, const b2BodyDef&, const b2Shape&);
void initRigid(Entity&, b2World&, nytl::Vec2f pos, const b2Shape&,
	bool dynamic = true);
