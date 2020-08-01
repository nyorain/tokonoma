#pragma once

#include <utility>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <functional>
#include <memory>
#include <nytl/functionTraits.hpp>

namespace tkn {

// Function
template<typename Sig> class Function;
constexpr struct NoAllocTag {} noAlloc;

// Differences to std::function:
// - Offers more memory for the small-object optimization, trying to avoid
//   as many allocations as possible.
//   Does not allocate memory on the heap when constructed with `noAlloc`,
//   will result in a compile time error if the function object
//   is too large instead. This is useful to allow using Function even
//   in performance critical contexts.
// - Is not copyable, only movable. This allows move-only functors
//   to be stored and passed around in this.
//   NOTE: should the need ever arise for this to be copyable in some
//   context, we could instead offer copy constructors that simply
//   throw when the function object does not allow copying.
template<typename Ret, typename... Args>
class Function<Ret(Args...)> {
public:
	static constexpr auto maxSize = 8 * sizeof(std::uintptr_t);
	using Storage = std::aligned_storage_t<maxSize>;

public:
	Function() = default;

	template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<Ret, F, Args...>>>
	Function(F obj) {
		impl_ = &impl<F>;
		if constexpr(sizeof(F) <= maxSize) {
			new(&storage_) F(std::move(obj));
		} else {
			new(&storage_) F*(new F(std::move(obj)));
		}
	}

	template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<Ret, F, Args...>>>
	Function(NoAllocTag, F obj) {
		static_assert(sizeof(F) <= maxSize, "Callable object is too large for Function");
		impl_ = &impl<F>;
		new(&storage_) F(std::move(obj));
	}

	~Function() {
		if(impl_) {
			impl_->destroy(storage_);
		}
	}

	Function(Function&& rhs) {
		if(rhs.impl_) {
			impl_ = rhs.impl_;
			impl_->moveDestroy(rhs.storage_, storage_);
			rhs.impl_ = {};
		}
	}

	Function& operator=(Function&& rhs) {
		if(impl_) {
			impl_->destroy(storage_);
		}

		impl_ = rhs.impl_;
		if(rhs.impl_) {
			rhs.impl_->moveDestroy(rhs.storage_, storage_);
			rhs.impl_ = {};
		}

		return *this;
	}

	Ret operator()(Args... args) {
		if(!impl_) {
			throw std::bad_function_call();
		}

		return impl_->call(storage_, std::forward<Args>(args)...);
	}

	operator bool() const {
		return impl_ != nullptr;
	}

	friend void swap(Function& a, Function& b) {
		Storage tmp;
		if(a.impl_) {
			a.impl_->moveDestroy(a.storage_, tmp);
		}

		if(b.impl_) {
			b.impl_->moveDestroy(b.storage_, a.storage_);
		}

		if(a.impl_) {
			a.impl_->moveDestroy(tmp, b.storage_);
		}
		std::swap(a.impl_, b.impl_);
	}

protected:
	struct FunctionImpl {
		void (*destroy)(Storage&);
		Ret (*call)(Storage&, Args... args);

		// moves from it's own implementation storage 'src' into
		// a new, object-free storage `dst` and destroys the
		// own object in src.
		void (*moveDestroy)(Storage& src, Storage& dst);
	};

	template<typename F>
	static F& getImpl(Storage& storage) {
		if constexpr(sizeof(F) <= maxSize) {
			return *std::launder(reinterpret_cast<F*>(&storage));
		} else {
			return **std::launder(reinterpret_cast<F**>(&storage));
		}
	}

	template<typename F>
	static void destroyImpl(Storage& storage) {
		if constexpr(sizeof(F) <= maxSize) {
			std::launder(reinterpret_cast<F*>(&storage))->~F();
		} else {
			delete *std::launder(reinterpret_cast<F**>(&storage));
		}
	}

	template<typename F>
	static Ret callImpl(Storage& storage, Args... args) {
		return std::invoke(getImpl<F>(storage), std::forward<Args>(args)...);
	}

	template<typename F>
	static void moveDestroy(Storage& src, Storage& dst) {
		if constexpr(sizeof(F) <= maxSize) {
			new(&dst) F(std::move(getImpl(src)));
			destroyImpl<F>(src);
		} else {
			new (&dst) F*(&getImpl(src));
			// this is not strictly needed.
			// We just store a pointer in storage, don't have to call its
			// destructor
			// std::memset(&src, 0x0, sizeof(src));
		}
	}

	template<typename F>
	static const FunctionImpl impl = {
		&destroyImpl<F>,
		&callImpl<F>,
	};

protected:
	const FunctionImpl* impl_ {};
	Storage storage_;
};

template<typename Ret, typename... Args>
template<typename F>
const typename Function<Ret(Args...)>::FunctionImpl Function<Ret(Args...)>::impl;

template<typename Ret, typename... Args>
Function(Ret (*)(Args...)) -> Function<Ret(Args...)>;

template<typename F>
Function(F obj) -> Function<typename nytl::FunctionTraits<F>::Signature>;

// FunctionView
// Like Function (or std::function) but does not own a functor, only
// references it. Should be preferred as function arguments that
// are never stored. Calling also has less overhead compared to
// tkn::Function.
// This does not allow member function pointers (to be called in an
// std::invoke -like fashion), unlike std::function or tkn::Function.
template<typename Sig> class FunctionView;

template<typename Ret, typename... Args>
class FunctionView<Ret(Args...)> {
public:
	FunctionView() = default;

	template<typename F>
	FunctionView(F& obj) {
		impl_ = &callImplObj<F>;
		new(&storage_) (F*)(&obj);
	}

	template<typename FR, typename... FArgs>
	FunctionView(std::enable_if_t<std::is_invocable_r_v<
			Ret, FR (*)(FArgs...), Args...>, FR (*)(FArgs...)> obj) {
		using Ptr = FR (*)(FArgs...);
		impl_ = &callImplFuncPtr<FR, FArgs...>;
		new(&storage_) (Ptr)(std::move(obj));
	}

	Ret operator()(Args... args) {
		if(!impl_) {
			throw std::bad_function_call();
		}

		return std::invoke(*impl_, storage_, std::forward<Args>(args)...);
	}

	operator bool() const {
		return impl_ != nullptr;
	}

protected:
	static constexpr auto size = std::max(sizeof(std::uintptr_t), sizeof(void(*)()));
	using Storage = std::aligned_storage_t<size>;

	template<typename FR, typename... FArgs>
	static Ret callImplFuncPtr(const Storage& storage, Args... args) {
		auto ptr = std::launder(reinterpret_cast<FR(**)(FArgs...)>(&storage));
		return std::invoke(**ptr, std::forward<Args>(args)...);
	}

	template<typename F>
	static Ret callImplObj(const Storage& storage, Args... args) {
		auto ptr = std::launder(reinterpret_cast<F*const*>(&storage));
		return std::invoke(**ptr, std::forward<Args>(args)...);
	}

	using Impl = Ret (*)(const Storage& storage, Args... args);

	Impl impl_ {};
	Storage storage_;

};

template<typename Ret, typename... Args>
FunctionView(Ret (*)(Args...)) -> FunctionView<Ret(Args...)>;

template<typename F>
FunctionView(F obj) -> FunctionView<typename nytl::FunctionTraits<F>::Signature>;

} // namespace tkn
