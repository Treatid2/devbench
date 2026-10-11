#pragma once

#include <atomic>

namespace dvb
{
	// Allocation-free custody of the generated listener's shared socket slot.
	// Stop and exception guards claim that slot by exchange: only the winner
	// performs native shutdown/close. Close's return means claimed, not that
	// shutdown succeeded. Native callbacks return error codes and must not throw.
	// The server owner must forbid rebind until the previous listener has joined;
	// this slot guard does not implement that lifecycle or protect owner lifetime.
	template<class Socket>
	class HttpListenerSocketOwner
	{
	public:
		using cleanup = int (*)(Socket);
		HttpListenerSocketOwner(std::atomic<Socket>& socket, Socket invalid,
			cleanup shutdown, cleanup close) noexcept :
			socket_(socket), invalid_(invalid), shutdown_(shutdown), close_(close) {}
		~HttpListenerSocketOwner() noexcept { (void)Close(); }
		HttpListenerSocketOwner(const HttpListenerSocketOwner&) = delete;
		HttpListenerSocketOwner& operator=(const HttpListenerSocketOwner&) = delete;

		bool Close() noexcept
		{
			const Socket claimed = socket_.exchange(invalid_, std::memory_order_acq_rel);
			if (claimed == invalid_)
				return false;
			(void)shutdown_(claimed);
			(void)close_(claimed);
			return true;
		}

	private:
		std::atomic<Socket>& socket_;
		const Socket invalid_;
		cleanup shutdown_;
		cleanup close_;
	};

	// Avoid httplib's allocating std::function scope_exit for this flag. The flag
	// describes listener execution only; it is not bind/transport readiness.
	class HttpListenerRunningFlag
	{
	public:
		explicit HttpListenerRunningFlag(std::atomic<bool>& flag) noexcept : flag_(flag)
		{
			flag_.store(true, std::memory_order_release);
		}
		~HttpListenerRunningFlag() noexcept { flag_.store(false, std::memory_order_release); }
		HttpListenerRunningFlag(const HttpListenerRunningFlag&) = delete;
		HttpListenerRunningFlag& operator=(const HttpListenerRunningFlag&) = delete;
	private:
		std::atomic<bool>& flag_;
	};
}
