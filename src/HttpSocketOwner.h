#pragma once

namespace dvb
{
	// Accepted socket lifetime for the generated non-SSL httplib handler.
	// No std::function/allocation before ownership: even a later string allocation
	// or request exception must shut down and close the admitted socket exactly once.
	// Callbacks are the pinned native socket primitives, which return error codes
	// rather than throwing. This owner does not attest successful network shutdown.
	template<class Socket>
	class HttpSocketOwner
	{
	public:
		using cleanup = int (*)(Socket);
		HttpSocketOwner(Socket socket, cleanup shutdown, cleanup close) noexcept :
			socket_(socket), shutdown_(shutdown), close_(close) {}
		~HttpSocketOwner() noexcept
		{
			if (owned_) {
				(void)shutdown_(socket_);
				(void)close_(socket_);
			}
		}
		// Transfer only after the actual HTTP queue reports successful admission.
		// Refusal/pre-admission throws leave this scope responsible for closing.
		void Release() noexcept { owned_ = false; }
		HttpSocketOwner(const HttpSocketOwner&) = delete;
		HttpSocketOwner& operator=(const HttpSocketOwner&) = delete;
	private:
		Socket socket_;
		cleanup shutdown_;
		cleanup close_;
		bool owned_{ true };
	};
}
