#pragma once
#ifndef DEVBENCH_PINNED_MCP_THREAD_POOL_H
#define DEVBENCH_PINNED_MCP_THREAD_POOL_H

// Local startup/lifetime overlay for cpp-mcp f1117d5. The build mirrors this as
// mcp_thread_pool.h only after an exact pinned-header comparison. Keep the public
// constructor/enqueue/get_active_threads surface; workers are joinable and owned.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp
{
	class thread_pool
	{
	public:
		// Failure seam: a factory must return the joinable thread that runs its
		// supplied function exactly once, asynchronously; no inline/reentrant calls.
		using thread_factory = std::function<std::thread(std::function<void()>)>;

		explicit thread_pool(
			size_t min_threads = (std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2),
			size_t max_threads = (std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() * 4 : 16),
			size_t idle_timeout_ms = 60000,
			thread_factory factory = [](std::function<void()> fn) { return std::thread(std::move(fn)); }) :
			min_threads_(min_threads ? min_threads : 1),
			max_threads_(max_threads < min_threads_ ? min_threads_ : max_threads),
			idle_timeout_(idle_timeout_ms),
			factory_(std::move(factory))
		{
			restart();
		}

		~thread_pool() { shutdown(); }
		thread_pool(const thread_pool&) = delete;
		thread_pool& operator=(const thread_pool&) = delete;

		// Owner-thread first phase: refuse new work without waiting for application
		// tasks. The server can then close sessions/listener paths needed by those
		// tasks before shutdown joins them. Mutex acquisition can still wait; this
		// is not a bounded-time or fully quiescent transport claim. Admitted work
		// continues draining, and restart always joins it before reopening.
		void close_admission()
		{
			{
				std::lock_guard queue(queue_mutex_);
				stop_ = true;
			}
			condition_.notify_all();
		}

		// Owner-thread operation; never call shutdown/restart from a pool task.
		// Wait for every admitted task and owned worker before releasing owner state.
		// A non-returning task still blocks shutdown; detaching is not a safe fallback.
		void shutdown()
		{
			std::lock_guard lifecycle(lifecycle_mutex_);
			shutdown_locked();
		}

		// Explicit owner retry after a fully drained shutdown. Constructor failure
		// uses the same cleanup path, including a failure after earlier worker starts.
		void restart()
		{
			std::lock_guard lifecycle(lifecycle_mutex_);
			{
				std::lock_guard queue(queue_mutex_);
				if (!stop_)
					return;
			}
			shutdown_locked();  // also joins a previously failed worker before reusing slots
			try {
				std::lock_guard queue(queue_mutex_);
				stop_ = false;
				for (size_t i = 0; i < min_threads_; ++i)
					spawn_locked();
			} catch (...) {
				// The queue lock has unwound before signalling/joining workers.
				shutdown_locked();
				throw;
			}
		}

		template<class F, class... Args>
		auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>
		{
			using return_type = typename std::invoke_result<F, Args...>::type;
			auto task = std::make_shared<std::packaged_task<return_type()>>(
				std::bind(std::forward<F>(f), std::forward<Args>(args)...));
			auto result = task->get_future();
			{
				std::lock_guard queue(queue_mutex_);
				if (stop_)
					throw std::runtime_error("Thread pool stopped, cannot add task");
				// Allocate a necessary worker BEFORE accepting the task. A factory
				// failure must not enqueue work whose caller received no future.
				if (idle_threads_ == 0 && active_threads_.load() < max_threads_)
					spawn_locked();
				tasks_.emplace([task] { (*task)(); });
			}
			condition_.notify_one();
			return result;
		}

		size_t get_active_threads() const { return active_threads_.load(); }
		uint64_t worker_failures() const { return worker_failures_.load(); }

	private:
		struct worker_slot
		{
			std::thread thread;
			bool retired{ false };  // protected by queue_mutex_; no worker touches it after retirement
		};

		// Requires queue_mutex_. Every worker waits on that mutex before it can
		// observe bookkeeping. Only successfully returned joinable threads count.
		void spawn_locked()
		{
			worker_slot* slot = nullptr;
			for (auto& candidate : workers_) {
				if (!candidate->thread.joinable() || candidate->retired) {
					if (candidate->thread.joinable())
						candidate->thread.join();  // finished worker no longer accesses queue_mutex_
					slot = candidate.get();
					break;
				}
			}
			if (!slot) {
				if (workers_.size() >= max_threads_)
					throw std::logic_error("Thread pool slot invariant violated");
				auto fresh = std::make_unique<worker_slot>();
				slot = fresh.get();
				workers_.push_back(std::move(fresh));
			}
			slot->retired = false;
			auto worker = factory_([this, slot] {
				worker_loop(*slot);
			});
			if (!worker.joinable())
				throw std::runtime_error("Thread factory must return the owned joinable worker");
			slot->thread = std::move(worker);
			++active_threads_;
		}

		void shutdown_locked()
		{
			close_admission();
			// Enqueue refuses all further admission; the slot list is now stable.
			for (auto& slot : workers_)
				if (slot->thread.joinable())
					slot->thread.join();
		}

		void worker_loop(worker_slot& slot) noexcept
		{
			try {
				for (;;) {
					std::function<void()> task;
					{
						std::unique_lock queue(queue_mutex_);
						++idle_threads_;
						bool timed_out = false;
						try {
							timed_out = !condition_.wait_for(queue, idle_timeout_, [this] { return stop_ || !tasks_.empty(); });
						} catch (...) {
							--idle_threads_;
							throw;
						}
						--idle_threads_;
						if ((stop_ && tasks_.empty()) ||
							(timed_out && tasks_.empty() && active_threads_.load() > min_threads_)) {
							// Retire under the decision's lock, so simultaneous idle exits
							// cannot all observe the same count and shrink below the core.
							// No pool or slot state is touched after lock release.
							// The current task is empty on both of these exit paths.
							--active_threads_;
							slot.retired = true;
							return;
						}
						if (tasks_.empty())
							continue;
						task = std::move(tasks_.front());
						tasks_.pop();
					}
					// packaged_task transports application exceptions through its future.
					task();
				}
			} catch (...) {
				++worker_failures_;
				std::queue<std::function<void()>> abandoned;
				{
					std::lock_guard queue(queue_mutex_);
					stop_ = true;
					abandoned.swap(tasks_);  // pending futures become broken promises, not false successes
				}
				condition_.notify_all();
			}
			// The abandoned queue (including application capture destructors) has
			// gone out of scope before marking this slot safe to join under the lock.
			std::lock_guard queue(queue_mutex_);
			--active_threads_;
			slot.retired = true;
		}

		const size_t min_threads_;
		const size_t max_threads_;
		const std::chrono::milliseconds idle_timeout_;
		thread_factory factory_;
		std::mutex lifecycle_mutex_;
		std::mutex queue_mutex_;
		std::condition_variable condition_;
		std::queue<std::function<void()>> tasks_;
		std::vector<std::unique_ptr<worker_slot>> workers_;
		std::atomic<size_t> active_threads_{ 0 };
		std::atomic<uint64_t> worker_failures_{ 0 };
		size_t idle_threads_{ 0 };  // protected by queue_mutex_
		bool stop_{ true };       // protected by queue_mutex_
	};
}
#endif  // DEVBENCH_PINNED_MCP_THREAD_POOL_H
