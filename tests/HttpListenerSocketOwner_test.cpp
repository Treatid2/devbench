#include "test_framework.h"
#include "HttpListenerSocketOwner.h"

#include <stdexcept>
#include <type_traits>

namespace
{
	thread_local int shutdownCount;
	thread_local int closeCount;
	thread_local int observedSocket;
	int Shutdown(int socket) { ++shutdownCount; observedSocket = socket; return -1; }
	int Close(int socket) { ++closeCount; observedSocket = socket; return -1; }
}

TEST_CASE("listener socket and running flag unwind failed preparation without allocation")
{
	shutdownCount = closeCount = 0;
	std::atomic<int> socket{ 42 };
	std::atomic<bool> running{ false };
	const auto prepare = [&] {
		dvb::HttpListenerSocketOwner<int> bound(socket, -1, Shutdown, Close);
		dvb::HttpListenerRunningFlag flag(running);
		CHECK(running.load());
		throw std::runtime_error("queue construction failed");
	};
	CHECK_THROWS(prepare());
	CHECK(!running.load());
	CHECK(socket.load() == -1);
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
	CHECK(observedSocket == 42);
	static_assert(!std::is_copy_constructible_v<dvb::HttpListenerSocketOwner<int>>);
	static_assert(!std::is_move_constructible_v<dvb::HttpListenerSocketOwner<int>>);
}

TEST_CASE("external listener stop and exception guards share one socket claim")
{
	shutdownCount = closeCount = 0;
	std::atomic<int> socket{ 99 };
	{
		dvb::HttpListenerSocketOwner<int> listener(socket, -1, Shutdown, Close);
		{
			dvb::HttpListenerSocketOwner<int> stop(socket, -1, Shutdown, Close);
			CHECK(stop.Close());  // claim, not successful OS shutdown
			CHECK(!stop.Close());
		}
		CHECK(!listener.Close());
	}
	CHECK(socket.load() == -1);
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
	CHECK(observedSocket == 99);
	{
		dvb::HttpListenerSocketOwner<int> unbound(socket, -1, Shutdown, Close);
		CHECK(!unbound.Close());
	}
	CHECK(closeCount == 1);
}

TEST_CASE("exceptional listener exit closes its socket before queue destruction")
{
	shutdownCount = closeCount = 0;
	std::atomic<int> socket{ 7 };
	bool queueSawClosedSocket = false;
	struct QueueDestruction
	{
		std::atomic<int>& socket;
		bool& sawClosed;
		~QueueDestruction() { sawClosed = socket.load() == -1; }
	};
	const auto listen = [&] {
		dvb::HttpListenerSocketOwner<int> constructorGuard(socket, -1, Shutdown, Close);
		QueueDestruction queue{ socket, queueSawClosedSocket };
		dvb::HttpListenerSocketOwner<int> closeBeforeDrain(socket, -1, Shutdown, Close);
		throw std::runtime_error("listener iteration failed");
	};
	CHECK_THROWS(listen());
	CHECK(queueSawClosedSocket);
	CHECK(shutdownCount == 1);
	CHECK(closeCount == 1);
}

TEST_CASE("listener running flag clears on normal scope exit")
{
	std::atomic<bool> running{ false };
	{
		dvb::HttpListenerRunningFlag flag(running);
		CHECK(running.load());
	}
	CHECK(!running.load());
	static_assert(!std::is_move_constructible_v<dvb::HttpListenerRunningFlag>);
}
