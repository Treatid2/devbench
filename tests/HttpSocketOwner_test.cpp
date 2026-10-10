#include "test_framework.h"
#include "HttpSocketOwner.h"

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
