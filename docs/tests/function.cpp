#include <tkn/function.hpp>
#include <tkn/threadPool.hpp>
#include <nytl/span.hpp>
#include <ostream>
#include <array>
#include <future>
#include "bugged.hpp"

void globalTest() {}

int callableDestructorCalled = 0;
struct Callable {
	~Callable() {
		++callableDestructorCalled;
	}

	int operator()() {
		return 72;
	}
};

TEST(function) {
	auto a = 0u;
	tkn::Function<int(int)> f = [&](int val){ a = val; return 1; };
	EXPECT(f(42), 1);
	EXPECT(f, true);
	EXPECT(a, 42u);

	EXPECT((tkn::Function<void()>{}), false);

	auto f2 = tkn::Function(globalTest);
	f2();

	{
		auto f3 = tkn::Function(Callable {});
		EXPECT(callableDestructorCalled, 1); // from temporary
		EXPECT(f3(), 72);
		EXPECT(f3, true);

		auto f4 = std::move(f3);
		EXPECT(f4(), 72);
		EXPECT(f3, false);
	}

	EXPECT(callableDestructorCalled, 3);

	// This should not compile since the function object is too large
	// auto largeArray = std::array<float, 32>{};
	// auto f4 = tkn::Function([largeArray]{});

	// test that it works with move-only objects
	std::promise<int> promise;
	auto future = promise.get_future();

	{
		auto func = tkn::Function([promise = std::move(promise)]() mutable {
			promise.set_value(5);
			return 1u;
		});

		EXPECT(future.valid(), true);
		EXPECT(future.wait_for(std::chrono::seconds(0)), std::future_status::timeout);

		EXPECT(func(), 1u);
		ERROR(func(), std::future_error);
		EXPECT(func, true);
	}

	EXPECT(future.valid(), true);
	EXPECT(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
	EXPECT(future.get(), 5);
	EXPECT(future.valid(), false);
}

TEST(functionView) {
	auto fv = tkn::FunctionView(globalTest);
	fv();

	callableDestructorCalled = 0u;
	Callable c {};
	EXPECT(callableDestructorCalled, 0);

	{
		auto fv = tkn::FunctionView(c);
		EXPECT(fv(), 72);
		EXPECT(fv, true);
	}

	EXPECT(callableDestructorCalled, 0);
	EXPECT((tkn::FunctionView<void()>{}), false);
}

TEST(functionLifetime) {
	callableDestructorCalled = 0u;
	auto ptr = std::make_unique<Callable>();
	EXPECT(callableDestructorCalled, 0);

	{
		auto f = tkn::Function([p = std::move(ptr)]{ return 1; });
		EXPECT(callableDestructorCalled, 0);
		EXPECT(f(), 1);
		EXPECT(callableDestructorCalled, 0);
	}

	EXPECT(callableDestructorCalled, 1);
}

TEST(threadPool) {
	tkn::ThreadPool tp(4u);
	auto promise = tp.addPromised([]{ return 4u; });
	EXPECT(promise.get(), 4u);

	{
		auto vec = std::vector<int>(64);
		auto func = [&](std::vector<int>& values) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			values[3] = 2;
			return values[7];
		};

		vec[7] = 42;
		auto promise2 = tp.addPromised(func, vec);

		auto vec2 = std::vector<int>(1024, -24);
		auto func2 = [&](nytl::Span<const int> values) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return values[1023];
		};

		auto promise3 = tp.addPromised(func2, nytl::Span<const int>(vec2));

		EXPECT(promise2.get(), 42);
		EXPECT(vec[3], 2);

		EXPECT(promise3.get(), -24);

	}
}

TEST(callable) {
	// TODO
}
