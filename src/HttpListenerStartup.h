#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace dvb
{
	// Queue/accept-loop preparation, not a request-level health probe. The owner
	// must serialize bind/start/stop and join the previous listener before Begin.
	// Epoch checks additionally prevent a stale exit from changing a newer result.
	class HttpListenerStartup
	{
	public:
		using Token = std::uint64_t;
		using Deadline = std::chrono::steady_clock::time_point;
		Token Begin()
		{
			std::lock_guard lock(mutex_);
			if (state_ == State::Pending || state_ == State::Ready)
				throw std::logic_error("Listener startup already active");
			if (token_ == std::numeric_limits<Token>::max())
				throw std::overflow_error("Listener startup epoch exhausted");
			++token_;
			state_ = State::Pending;
			return token_;
		}
		Token CurrentToken() const
		{
			std::lock_guard lock(mutex_);
			return token_;
		}
		bool PublishReady(Token token)
		{
			std::lock_guard lock(mutex_);
			if (token != token_ || state_ != State::Pending) return false;
			state_ = State::Ready;
			changed_.notify_all();
			return true;
		}
		void Finish(Token token)
		{
			std::lock_guard lock(mutex_);
			if (token != token_) return;
			state_ = State::Stopped;
			changed_.notify_all();
		}
		void Cancel()
		{
			std::lock_guard lock(mutex_);
			state_ = State::Stopped;
			changed_.notify_all();
		}
		bool WaitUntil(Token token, Deadline deadline)
		{
			std::unique_lock lock(mutex_);
			changed_.wait_until(lock, deadline, [&] {
				return token != token_ || state_ != State::Pending;
			});
			return token == token_ && state_ == State::Ready;
		}
		bool Ready() const
		{
			std::lock_guard lock(mutex_);
			return state_ == State::Ready;
		}
	private:
		enum class State { Idle, Pending, Ready, Stopped };
		mutable std::mutex mutex_;
		std::condition_variable changed_;
		Token token_{ 0 };
		State state_{ State::Idle };
	};

	// Install before queue construction. A second guard inside the queue's scope
	// publishes failure/exit before potentially blocking queue destruction/drain.
	class HttpListenerStartupExit
	{
	public:
		HttpListenerStartupExit(HttpListenerStartup& startup, HttpListenerStartup::Token token) :
			startup_(startup), token_(token) {}
		~HttpListenerStartupExit() { startup_.Finish(token_); }
		HttpListenerStartupExit(const HttpListenerStartupExit&) = delete;
		HttpListenerStartupExit& operator=(const HttpListenerStartupExit&) = delete;
	private:
		HttpListenerStartup& startup_;
		HttpListenerStartup::Token token_;
	};
}
