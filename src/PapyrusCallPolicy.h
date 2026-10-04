#pragma once

#include "Json.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

namespace dvb::PapyrusCallPolicy
{
	inline constexpr int kMaximumTimeoutMs = 60000;
	inline bool ExactArgumentCount(std::size_t supplied, std::size_t declared) { return supplied == declared; }

	// Production and tests use this same admission/receipt policy. The owner must
	// hold its CallState mutex across every access. No engine object lives here.
	class Lifecycle
	{
	public:
		using Clock = std::chrono::steady_clock;
		Lifecycle(json request, Clock::time_point deadline) :
			request_(std::move(request)), deadline_(deadline) {}

		bool BeginPreparation(Clock::time_point now)
		{
			if (phase_ != "queued")
				return false;
			if (now >= deadline_) {
				phase_ = "abandoned_before_dispatch";
				return false;
			}
			phase_ = "preparing";
			return true;
		}

		bool BeginDispatch(Clock::time_point now)
		{
			if (phase_ != "preparing")
				return false;
			if (now >= deadline_) {
				phase_ = "abandoned_before_dispatch";
				return false;
			}
			phase_ = "dispatching";
			enteredDispatch_ = true;
			return true;
		}

		void Accepted()
		{
			// A synchronous callback can already have completed the request.
			if (phase_ == "dispatching")
				phase_ = "dispatched";
		}
		void Completed() { phase_ = "completed"; }
		void Failed()
		{
			if (phase_ != "abandoned_before_dispatch" && phase_ != "completed")
				phase_ = enteredDispatch_ ? "dispatch_uncertain" : "rejected_before_dispatch";
		}
		void TimedOut()
		{
			if (phase_ == "queued" || phase_ == "preparing")
				phase_ = "abandoned_before_dispatch";
		}
		void Resolved(json signature, json selfHandle)
		{
			resolved_ = json{ { "function", std::move(signature) }, { "selfHandle", std::move(selfHandle) } };
		}
		json Snapshot() const
		{
			return json{
				{ "request", request_ }, { "resolved", resolved_ }, { "phase", phase_ },
				{ "argumentPolicy", "explicit_exact_count" }, { "filledArgs", json::array() },
				{ "dispatchEntered", enteredDispatch_ },
				{ "executionMayHaveOccurred", enteredDispatch_ },
				{ "mayStillExecute", enteredDispatch_ && phase_ != "completed" },
				{ "automaticRetryAllowed", false },
				{ "scope", "response_time_snapshot_not_durable_status" },
			};
		}
		Clock::time_point Deadline() const { return deadline_; }

	private:
		const json request_;  // supplied JSON, not a claim about packed VM values
		const Clock::time_point deadline_;
		json resolved_ = nullptr;
		std::string phase_ = "queued";
		bool enteredDispatch_ = false;
	};
}
