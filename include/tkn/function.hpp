#pragma once

#include <utility>
#include <cstdlib>
#include <cstdint>
#include <type_traits>
#include <functional>
#include <nytl/functionTraits.hpp>

namespace tkn {

template<typename Sig> class Function;

// Differences to std::function:
// - Does never allocate memory on the heap. Generates compile-time
//   error if constructed from object that doesn't fit in stack storage.
// - Is not copyable, only movable. This allows move-only functors
//   to be stored and passed around in this.
template<typename Ret, typename... Args>
class Function<Ret(Args...)> {
public:
	static constexpr auto maxSize = 8 * sizeof(std::uintptr_t);
	using Storage = std::aligned_storage_t<maxSize>;
	using Impl = Ret(*)(Storage&, Args...);

public:
	Function() = default;

	template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<Ret, F, Args...>>>
	Function(F obj) {
		static_assert(sizeof(F) <= maxSize, "Callable object is too large for Function");
		impl_ = &impl<F>;
		new(&storage_) F(std::move(obj));
	}

	~Function() {
		if(impl_) {
			impl_->destroy(storage_);
		}
	}

	Function(Function&& rhs) { swap(*this, rhs); }
	Function& operator=(Function rhs) {
		swap(*this, rhs);
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
		std::swap(a.impl_, b.impl_);
		std::swap(a.storage_, b.storage_);
	}

protected:
	struct FunctionImpl {
		void (*destroy)(Storage&);
		Ret (*call)(Storage&, Args... args);
	};

	template<typename F>
	static void destroyImpl(Storage& storage) {
		std::launder(reinterpret_cast<F*>(&storage))->~F();
	}

	template<typename F>
	static Ret callImpl(Storage& storage, Args... args) {
		return std::invoke(*std::launder(reinterpret_cast<F*>(&storage)),
			std::forward<Args>(args)...);
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
