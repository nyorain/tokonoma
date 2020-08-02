#pragma once

#include <utility>
#include <functional>
#include <memory>
#include <nytl/functionTraits.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

template<typename Sig> class Callable;
template<typename Sig, typename F> class CallableImpl;

template<typename Ret, typename... Args>
class Callable<Ret(Args...)> {
public:
	using Destructor = void(*)(Callable&);
	using CallImpl = Ret(*)(Callable&, Args...);

	// Usually not created directly, use CallableImpl
	inline Callable(Destructor destructor, CallImpl callImpl) :
			destructor_(destructor), callImpl_(callImpl) {
		dlg_assert(callImpl_);
	}

	inline ~Callable() {
        // when destructor_ is unset, the derived class either needs no destructor
        // or was already called
        if(destructor_) {
		    (*destructor_)(*this);
        }
	}

	inline Ret operator()(Args... args) {
		return (*callImpl_)(*this, std::forward<Args>(args)...);
    }

protected:
	// in-object vtable
	Destructor destructor_;
	const CallImpl callImpl_;
};

template<typename Ret, typename... Args>
class Callable<Ret(Args...) const> {
public:
	using Destructor = void(*)(Callable&);
	using CallImpl = Ret(*)(const Callable&, Args...);

	// Usually not created directly, use CallableImpl
	inline Callable(Destructor destructor, CallImpl callImpl) :
			destructor_(destructor), callImpl_(callImpl) {
		dlg_assert(callImpl_);
	}

	inline ~Callable() {
        // when destructor_ is unset, the derived class either needs no destructor
        // or was already called
        if(destructor_) {
		    (*destructor_)(*this);
        }
	}

	inline Ret operator()(Args... args) const {
		return (*callImpl_)(*this, std::forward<Args>(args)...);
    }

protected:
	// in-object vtable
	Destructor destructor_;
	const CallImpl callImpl_;
};

template<typename F, typename Ret, typename... Args>
class CallableImpl<Ret(Args...), F> : public Callable<Ret(Args...)> {
public:
    using Base = Callable<Ret(Args...)>;
    static constexpr auto needsDestructor = !std::is_trivially_destructible_v<F>;
	F obj;

    inline CallableImpl(F xobj) :
        Base(needsDestructor ? &destructorImpl : nullptr, &callImpl),
        obj(std::move(xobj)) {
    }

    inline ~CallableImpl() {
        // NOTE: when needsDestructor is false, this isn't needed, as destructor_
        // will already be nullptr. But compilers can't seem to optimize for it,
        // so always setting this to null here makes sure they can properly optimize
        // out the if-branch in our base classes destructor
        this->destructor_ = nullptr;
    }

private:
	static Ret callImpl(Base& callable, Args... args) {
		// This will usually not generate any runtime code.
		auto& self = static_cast<CallableImpl&>(callable);
		// At least for lambdas (that are only used here), this is likely
		// to be inlined.
		return std::invoke(self.obj, std::forward<Args>(args)...);
	}

	static void destructorImpl(Base& callable) {
		auto& self = static_cast<CallableImpl&>(callable);
        self.~CallableImpl();
	}
};

template<typename F, typename Ret, typename... Args>
class CallableImpl<Ret(Args...) const, F> : public Callable<Ret(Args...) const> {
public:
    using Base = Callable<Ret(Args...) const>;
    static constexpr auto needsDestructor = !std::is_trivially_destructible_v<F>;
	F obj;

    inline CallableImpl(F xobj) :
        Base(needsDestructor ? &destructorImpl : nullptr, &callImpl),
        obj(std::move(xobj)) {
    }

    inline ~CallableImpl() {
        this->destructor_ = nullptr;
    }

private:
	static Ret callImpl(const Base& callable, Args... args) {
		// This will usually not generate any runtime code.
		auto& self = static_cast<const CallableImpl&>(callable);
		// At least for lambdas (that are only used here), this is likely
		// to be inlined.
		return std::invoke(self.obj, std::forward<Args>(args)...);
	}

	static void destructorImpl(Base& callable) {
		auto& self = static_cast<CallableImpl&>(callable);
        self.~CallableImpl();
	}
};

template<typename F>
CallableImpl(F) -> CallableImpl<typename nytl::FunctionTraits<F>::Signature, F>;

template<typename Sig> using UniqueCallable = std::unique_ptr<Callable<Sig>>;

template<typename Sig, typename F>
UniqueCallable<Sig> makeUniqueCallable(F func) {
    return std::make_unique<CallableImpl<Sig, F>>(std::move(func));
}

template<typename F>
auto makeUniqueCallable(F func) {
	using Sig = typename nytl::FunctionTraits<F>::Signature;
    return makeUniqueCallable<Sig, F>(std::move(func));
}

} // namespace tkn
