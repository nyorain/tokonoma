#include "physics.hpp"
#include "entity.hpp"
#include <dlg/dlg.hpp>
#include <Box2D/Dynamics/Contacts/b2Contact.h>

PhysicsSystem::PhysicsSystem() {
	world.SetContactListener(&contactListener);
}

void PhysicsSystem::ContactListener::BeginContact(b2Contact* pcontact) {
	dlg_assert(pcontact);
	auto& contact = *pcontact;

	auto bodyA = contact.GetFixtureA()->GetBody();
	auto eA = static_cast<Entity*>(bodyA->GetUserData());

	auto bodyB = contact.GetFixtureB()->GetBody();
	auto eB = static_cast<Entity*>(bodyB->GetUserData());

	if(eA) {
		auto listener = eA->part<parts::Contact>();
		if(listener) {
			listener->begin({*eA, eB, contact, true});
		}
	}

	if(eB) {
		auto listener = eB->part<parts::Contact>();
		if(listener) {
			listener->begin({*eB, eA, contact, false});
		}
	}
}

void PhysicsSystem::ContactListener::EndContact(b2Contact* pcontact) {
	dlg_assert(pcontact);
	auto& contact = *pcontact;

	auto bodyA = contact.GetFixtureA()->GetBody();
	auto eA = static_cast<Entity*>(bodyA->GetUserData());

	auto bodyB = contact.GetFixtureB()->GetBody();
	auto eB = static_cast<Entity*>(bodyB->GetUserData());

	if(eA) {
		auto listener = eA->part<parts::Contact>();
		if(listener) {
			listener->end({*eA, eB, contact, true});
		}
	}

	if(eB) {
		auto listener = eB->part<parts::Contact>();
		if(listener) {
			listener->end({*eB, eA, contact, false});
		}
	}
}

void PhysicsSystem::update(double delta) {
	world.Step(delta, velocityIterations, positionIterations);
}

// Rigid part
namespace parts {

Rigid::Rigid(b2World& world, const b2BodyDef& bdf, const b2FixtureDef& fdf) {
	body = world.CreateBody(&bdf);
	fixture = body->CreateFixture(&fdf);
}

Rigid::Rigid(b2World& world, nytl::Vec2f pos, const b2Shape& shape, bool dyn) {
	b2BodyDef bdf;
	bdf.position = {pos[0], pos[1]};
	bdf.type = dyn ? b2_dynamicBody : b2_staticBody;
	bdf.angularDamping = 0.5f;
	bdf.linearDamping = 2.f;
	body = world.CreateBody(&bdf);

	b2FixtureDef fdf;
	fdf.shape = &shape;
	fdf.density = 1.f;
	fdf.friction = 0.2f;
	fdf.restitution = 0.0f;
	fixture = body->CreateFixture(&fdf);
}

Rigid::~Rigid() {
	if(fixture) {
		dlg_assert(body && fixture->GetBody() == body);
		body->DestroyFixture(fixture);
	}

	if(body) {
		auto w = body->GetWorld();
		dlg_assert(w);
		w->DestroyBody(body);
	}
}

Rigid::Rigid(Rigid&& rhs) noexcept {
	swap(*this, rhs);
}

Rigid& Rigid::operator=(Rigid rhs) noexcept {
	swap(*this, rhs);
	return *this;
}

b2Transform Rigid::transform(const Entity&) const {
	return body->GetTransform();
}

void swap(Rigid& a, Rigid& b) {
	using std::swap;
	swap(a.body, b.body);
	swap(a.fixture, b.fixture);
}

} // namespace parts

void initRigid(Entity& rigid, b2World& world, const b2BodyDef& body,
		const b2FixtureDef& fixture) {

	auto& r = rigid.expectPart<parts::Rigid>();
	if(r.body) {
		dlg_assert(r.body->GetWorld() == &world);
		r.body->GetWorld()->DestroyBody(r.body);
	}

	r.body = world.CreateBody(&body);
	r.fixture = r.body->CreateFixture(&fixture);
	r.body->SetUserData(&rigid);
}

void initRigid(Entity& rigid, b2World& world, const b2BodyDef& body,
		const b2Shape& shape) {

	b2FixtureDef fixture;
	fixture.shape = &shape;
	fixture.density = 1.f;
	fixture.friction = 0.2f;
	fixture.restitution = 0.0f;
	initRigid(rigid, world, body, fixture);
}

void initRigid(Entity& rigid, b2World& world, nytl::Vec2f pos,
		const b2Shape& shape, bool dynamic) {

	b2BodyDef bdf;
	bdf.position = {pos[0], pos[1]};
	bdf.type = dynamic ? b2_dynamicBody : b2_staticBody;
	bdf.angularDamping = 0.5f;
	bdf.linearDamping = 2.f;
	initRigid(rigid, world, bdf, shape);
}
