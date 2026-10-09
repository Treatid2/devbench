#pragma once

#include "PinnedMcpThreadPool.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

namespace dvb
{
	// The generated httplib::ThreadPool delegates to this actual production queue.
	// Reuse the owned pool's constructor rollback/join rather than maintaining a
	// second thread implementation. The member order keeps admission bookkeeping
	// alive while the last-declared pool drains and joins during destruction.
	class HttpTaskQueueOwnership
	{
	public:
		using thread_factory = mcp::thread_pool::thread_factory;

		explicit HttpTaskQueueOwnership(std::size_t workers, std::size_t maxQueued = 0,
			thread_factory factory = [](std::function<void()> fn) { return std::thread(std::move(fn)); }) :
			maxQueued_(maxQueued),
			pool_(workers, workers, 60000, std::move(factory))
		{}

		HttpTaskQueueOwnership(const HttpTaskQueueOwnership&) = delete;
		HttpTaskQueueOwnership& operator=(const HttpTaskQueueOwnership&) = delete;

		bool enqueue(std::function<void()> fn)
		{
			std::lock_guard admission(admissionMutex_);
			if (closed_ || !fn || (maxQueued_ != 0 && queued_ >= maxQueued_))
				return false;
			++queued_;
			try {
				(void)pool_.enqueue([this, fn = std::move(fn)] {
					{
						std::lock_guard admission(admissionMutex_);
						--queued_;  // maxQueued bounds waiting jobs, not active handlers
					}
					try {
						fn();
					} catch (...) {
						++failedRequests_;
						// The owned pool's packaged_task retains the exception, so it
						// cannot escape the worker entry and terminate the process.
						// This is not an HTTP response or socket-cleanup guarantee.
						throw;
					}
				});
			} catch (...) {
				--queued_;  // the core pool does not accept work on failed enqueue
				++refusedEnqueues_;
				return false;  // httplib owns closing the rejected accepted socket
			}
			return true;
		}

		// Owner-thread only. Do not hold admissionMutex_ while joining workers
		// that acquire it. A hung handler still prevents drain/destruction.
		void shutdown()
		{
			{
				std::lock_guard admission(admissionMutex_);
				closed_ = true;
			}
			pool_.shutdown();
		}

		std::uint64_t failed_requests() const { return failedRequests_.load(); }
		std::uint64_t refused_enqueues() const { return refusedEnqueues_.load(); }

	private:
		std::mutex admissionMutex_;
		const std::size_t maxQueued_;
		std::size_t queued_{ 0 };
		bool closed_{ false };
		std::atomic<std::uint64_t> failedRequests_{ 0 };
		std::atomic<std::uint64_t> refusedEnqueues_{ 0 };
		mcp::thread_pool pool_;  // MUST drain before the bookkeeping above is destroyed
	};
}
