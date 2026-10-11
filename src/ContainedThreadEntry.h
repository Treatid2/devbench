#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace dvb
{
	// Actual worker-entry boundary: never allocate/log/call another fallible
	// callback while containing a failure. Owner still must join the worker and
	// retire dependent state; exception containment alone is not safe teardown.
	class ContainedThreadEntry
	{
	public:
		template<class Body>
		bool Run(Body&& body) noexcept
		{
			try {
				std::forward<Body>(body)();
				return true;
			} catch (...) {
				failures_.fetch_add(1, std::memory_order_release);
				return false;
			}
		}
		std::uint64_t Failures() const noexcept { return failures_.load(std::memory_order_acquire); }
		bool Failed() const noexcept { return Failures() != 0; }
	private:
		std::atomic<std::uint64_t> failures_{ 0 };
	};
}
