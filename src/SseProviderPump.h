#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace dvb
{
	enum class SseWaitOutcome { Event, Timeout, Closed };

	// The HTTP response provider owns both endpoint publication and keepalives.
	// No separate worker can outlive a failed response or call back into its owner.
	class SseProviderPump
	{
	public:
		explicit SseProviderPump(std::string endpoint) : endpoint_(std::move(endpoint)) {}

		template<class Ready, class Write, class Wait>
		bool Pump(Ready&& ready, Write&& write, Wait&& wait) noexcept
		{
			try {
				if (!ready()) return false;
				if (!endpoint_sent_) {
					if (!write(endpoint_)) return false;
					endpoint_sent_ = true;
					return true;
				}
				const auto outcome = wait();
				if (!ready() || outcome == SseWaitOutcome::Closed) return false;
				if (outcome == SseWaitOutcome::Event) return true;
				const auto heartbeat = std::string("event: heartbeat\r\ndata: ") +
					std::to_string(heartbeat_count_) + "\r\n\r\n";
				if (!write(heartbeat)) return false;
				++heartbeat_count_;
				return true;
			} catch (...) {
				return false;
			}
		}
	private:
		std::string endpoint_;
		bool endpoint_sent_{ false };
		std::uint64_t heartbeat_count_{ 0 };
	};
}
