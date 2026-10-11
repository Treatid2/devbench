#include "test_framework.h"
#include "HttpSocketOwner.h"

#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace
{
	thread_local int shutdownCount;
	thread_local int closeCount;
	thread_local int observedSocket;
	int Shutdown(int socket) { ++shutdownCount; observedSocket = socket; return -1; }
	int Close(int socket) { ++closeCount; observedSocket = socket; return 0; }
}

TEST_CASE("HTTP socket owner closes once even when shutdown reports failure")
{
	shutdownCount = closeCount = 0;
	{
		dvb::HttpSocketOwner<int> owner(42, Shutdown, Close);
		CHECK(shutdownCount == 0);
		CHECK(closeCount == 0);
	}
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
	CHECK(observedSocket == 42);
	static_assert(!std::is_copy_constructible_v<dvb::HttpSocketOwner<int>>);
	static_assert(!std::is_move_constructible_v<dvb::HttpSocketOwner<int>>);
}

TEST_CASE("HTTP socket ownership survives exceptional request unwinding")
{
	shutdownCount = closeCount = 0;
	const auto failedRequest = [] {
		dvb::HttpSocketOwner<int> owner(99, Shutdown, Close);
		throw std::runtime_error("request preparation/handler failed");
	};
	CHECK_THROWS(failedRequest());
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
	CHECK(observedSocket == 99);
}

TEST_CASE("accepted HTTP socket transfers cleanup only after successful admission")
{
	shutdownCount = closeCount = 0;
	{
		dvb::HttpSocketOwner<int> accepted(7, Shutdown, Close);
		accepted.Release();  // actual generated queue returns true before this call
	}
	CHECK(shutdownCount == 0);
	CHECK(closeCount == 0);
	{
		dvb::HttpSocketOwner<int> admittedHandler(7, Shutdown, Close);
	}
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
	CHECK(observedSocket == 7);
}

TEST_CASE("accepted HTTP socket keeps cleanup on refusal or preparation exception")
{
	for (bool preparationThrows : { false, true }) {
		shutdownCount = closeCount = 0;
		const auto admit = [&] {
			dvb::HttpSocketOwner<int> accepted(8, Shutdown, Close);
			if (preparationThrows)
				throw std::runtime_error("queue argument allocation failed");
			// A false admission must not call Release.
			return false;
		};
		if (preparationThrows) {
			CHECK_THROWS(admit());
		} else {
			CHECK(!admit());
		}
		CHECK(shutdownCount == 1);
		CHECK(closeCount == 1);
		CHECK(observedSocket == 8);
	}
}
