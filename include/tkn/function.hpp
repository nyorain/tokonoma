#pragma once

#include <utility>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <functional>
#include <memory>

#include <nytl/functionTraits.hpp>
#include <dlg/dlg.hpp>

// Good post on possible design choices:
// https://quuxplusone.github.io/blog/2019/03/27/design-space-for-std-function/

namespace tkn {

constexpr struct NoAllocTag {} noAlloc;

// Differences to std::function:
// - Offers more memory for the small-object optimization, trying to avoid
//   as many allocations as possible.
//   Does not allocate memory on the heap when constructed with `noAlloc`,
//   will result in a compile time error if the function object
//   is too large instead. This is useful to allow using Function even
//   in highly performance critical contexts where allocating would be
//   a problem.
// - Is not copyable, only movable. This allows move-only functors
//   to be stored and passed around in this.
//   NOTE: should the need ever arise for this to be copyable in some
//   context, we could instead offer copy constructors that simply
//   throw when the function object does not allow copying. Arguably worse though.
// - Offers const-correctness. If the Function has to be const-callable, just
//   add a const to the signature like `Ret(Args...) const` (which will
//   then e.g. not work with mutable lambdas or only non-const callables).
//   Otherwise, offers no const call operator.
template<typename Sig> class Function;

template<bool ConstSig, typename Ret, typename... Args>
class FunctionBase {
public:
	static constexpr auto maxSize = 4 * sizeof(std::uintptr_t);
	using Storage = std::aligned_storage_t<maxSize>;
	using CallStorage = std::conditional_t<ConstSig, const Storage, Storage>;

public:
	FunctionBase() = default;

	template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<Ret, F, Args...>>>
	FunctionBase(F obj) {
		impl_ = &impl<F>;
		if constexpr(sizeof(F) <= maxSize) {
			new(&storage_) F(std::move(obj));
		} else {
			new(&storage_) F*(new F(std::move(obj)));
		}
	}

	template<typename F, typename = std::enable_if_t<std::is_invocable_r_v<Ret, F, Args...>>>
	FunctionBase(NoAllocTag, F obj) {
		static_assert(sizeof(F) <= maxSize, "Callable object is too large for Function");
		impl_ = &impl<F>;
		new(&storage_) F(std::move(obj));
	}

	~FunctionBase() {
		if(impl_) {
			impl_->destroy(storage_);
		}
	}

	FunctionBase(FunctionBase&& rhs) {
		if(rhs.impl_) {
			impl_ = rhs.impl_;
			impl_->moveDestroy(rhs.storage_, storage_);
			rhs.impl_ = {};
		}
	}

	FunctionBase& operator=(FunctionBase&& rhs) {
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

	operator bool() const {
		return impl_ != nullptr;
	}

	friend void swap(FunctionBase& a, FunctionBase& b) {
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
		Ret (*call)(CallStorage&, Args... args);

		// moves from it's own implementation storage 'src' into
		// a new, object-free storage `dst` and destroys the
		// own object in src.
		void (*moveDestroy)(Storage& src, Storage& dst);
	};

	template<typename F>
	static void destroyImpl(Storage& storage) {
		if constexpr(sizeof(F) <= maxSize) {
			std::launder(reinterpret_cast<F*>(&storage))->~F();
		} else {
			delete *std::launder(reinterpret_cast<F**>(&storage));
		}
	}

	template<typename F>
	static Ret callImpl(CallStorage& storage, Args... args) {
		using CallF = std::conditional_t<ConstSig, const F, F>;
		CallF* func;
		if constexpr(sizeof(F) <= maxSize) {
			func = std::launder(reinterpret_cast<CallF*>(&storage));
		} else {
			func = *std::launder(reinterpret_cast<CallF**>(&storage));
		}

		return std::invoke(*func, std::forward<Args>(args)...);
	}

	template<typename F>
	static void moveDestroy(Storage& src, Storage& dst) {
		if constexpr(sizeof(F) <= maxSize) {
			auto& impl = *std::launder(reinterpret_cast<F*>(&src));
			new(&dst) F(std::move(impl));
			impl.~F();
		} else {
			auto* pimpl = *std::launder(reinterpret_cast<F**>(&src));
			new (&dst) F*(pimpl);
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
		&moveDestroy<F>
	};

protected:
	const FunctionImpl* impl_ {};
	Storage storage_;
};

template<typename Ret, typename... Args>
class Function<Ret(Args...)> : public FunctionBase<false, Ret, Args...> {
public:
	using FunctionBase<false, Ret, Args...>::FunctionBase;

	// Calling this on an empty function is undefined behavior, unlike
	// std::function, which throws std::bad_function_call.
	// Reasoning: if somebody wants to call a potentially empty function
	// object, they should just check it beforehand and not use the
	// exception mechanism. If otoh a contract for instance defines that
	// a function must not be empty and it is called without checking,
	// a thrown exception could not be handled by anyone anyways since
	// it's a logical programming error.
	// But omitting the potential throw here makes it more likely that
	// the function call is inlined (at least in release builds without
	// the assert), avoiding additional overhead.
	Ret operator()(Args... args) {
		dlg_assert(this->impl_);
		return this->impl_->call(this->storage_, std::forward<Args>(args)...);
	}
};

template<typename Ret, typename... Args>
class Function<Ret(Args...) const> : public FunctionBase<true, Ret, Args...> {
public:
	using FunctionBase<true, Ret, Args...>::FunctionBase;

	Ret operator()(Args... args) const {
		dlg_assert(this->impl_);
		return this->impl_->call(this->storage_, std::forward<Args>(args)...);
	}
};

template<bool ConstSig, typename Ret, typename... Args>
template<typename F> const typename
FunctionBase<ConstSig, Ret, Args...>::FunctionImpl
FunctionBase<ConstSig, Ret, Args...>::impl;

template<typename Ret, typename... Args>
Function(Ret (*)(Args...)) -> Function<Ret(Args...) const>;

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

	Ret operator()(Args... args) const {
		dlg_assert(impl_);
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
