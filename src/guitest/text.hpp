#pragma once

#include "font.hpp"
#include "draw.hpp"
#include <nytl/rect.hpp>

namespace rvg2 {

namespace detail {
	void outputException(const std::exception& err);
} // namespace detail

/// Small RAII wrapper around changing a DeviceObjects contents.
/// As long as the object is alive, the state can be changed. When
/// it gets destructed, the update function of the original object will
/// be called, i.e. the changed state will be applied.
///
/// For example, using a RectShape:
/// ```cpp
/// RectShape rect = {...};
/// auto rc = rect.change();
/// rc->position += Vec {10.f, 10.f};
/// rc->size.y = 0.f;
/// rc->rounding[0] = 2.f;
/// ```
/// When 'rc' goes out of scope in the above example, the rect shape
/// is updated (i.e. the fill/stroke/aa points baked).
/// Doing so only once for all those changes instead of again
/// after every single change can increase performance and avoids
/// redundant work.
/// If you only want to change one parameter, you can still
/// use it inline like this: ```rect.change()->position.x = 0.f```.
template<typename T, typename S>
class StateChange {
public:
	T& object;
	S& state;

public:
	~StateChange() {
		// We don't throw here since throwing from destructor
		// is bad idea.
		try {
			object.update();
		} catch(const std::exception& err) {
			detail::outputException(err);
		}
	}

	S* operator->() { return &state; }
	S& operator*() { return state; }
};

template<typename T, typename S>
StateChange(T&, S&) -> StateChange<T, S>;


class Text {
public:
	Text() = default;
	Text(IndexPool&, VertexPool&, Vec2f pos,
		std::string text, const FontRef&, float height);

	Text(Text&& rhs) noexcept;
	Text& operator=(Text&& rhs) noexcept;
	~Text();

	DrawInstance draw(DrawRecorder& rec);
	auto change() { return StateChange {*this, state_}; }

	/// Computes which char index lies at the given relative x.
	/// Returns the index of the char at the given x, or the index of
	/// the next one if there isn't any. Returns text.length() if x is
	/// after all chars.
	/// Must not be called during a state change.
	unsigned charAt(float x) const;

	/// Returns the (local) bounds of the full text
	Rect2f bounds() const;

	/// Returns the bounds of the ith char in local coordinates.
	Rect2f ithBounds(unsigned n) const;

	const auto& font() const { return state_.font; }
	const auto& text() const { return state_.text; }
	const auto& position() const { return state_.position; }
	float height() const { return state_.height; }
	float width() const;

	void update();
	bool updateDevice();

protected:
	struct State {
		std::string text {};
		FontRef font {}; // must not be set to invalid font
		Vec2f position {}; // baseline position of first character
		float height {}; // height to use
		// pre-transform
		nytl::Mat3f transform = nytl::identity<3, float>();
	} state_;

	IndexPool* indexPool_ {};
	IndexPool* vertexPool_ {};
	unsigned vertexStart_ {invalid};
	unsigned vertexCount_ {};
	unsigned indexStart_ {invalid};
	unsigned indexCount_ {};
};

} // namespace rvg2

