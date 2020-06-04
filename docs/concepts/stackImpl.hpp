#pragma once

#include <new>
#include <type_traits>

// Allows to pass an interface implementation on the stack.
// Type-erasure with a fixed interface and no memory allocation (in
// turn, you have to know the maximum object size beforehand).
// I guess Interface = void should work, making this a no-interface
// type-erasure container with fixed maximum size.
template<typename Interface, std::size_t MaxImplSize>
class StackImpl {
public:
	// We could move impl_ and set a correct value
	// for ptr_. But: we cannot know whether the implementation object
	// is trivially movable, and we certainly can't call it's move
	// constructor/assignment operator here.
	// This could probably be fixed by using another virtual wrapper around
	// the implementation first that adds a "move" (or even "copy") function.
	// They just invoke the move/copy constructor/assignment operators
	// of the erased type.
	StackImpl(StackImpl&&) = delete;
	StackImpl& operator=(StackImpl&&) = delete;

	~StackImpl() { reset(); }

	template<typename Impl, typename... Args>
	Impl& emplace(Args&&... args) {
		static_assert(sizeof(Impl) <= MaxImplSize);
		ptr_ = new(&impl_) Impl(std::forward<Args>(args)...);
	}

	void reset() const {
		if(ptr_) {
			ptr_->~Interface();
			ptr_ = {};
		}
	}

	Interface* get() const { return ptr_; }
	Interface* operator->() const { return get(); }

protected:
	using Buffer = std::aligned_storage_t<MaxImplSize>;
	Buffer impl_;

	// We could retrieve this from impl_ using std::launder
	// But we need to track anyways whether or not impl_ holds
	// a value.
	Interface* ptr_ {};
};
