#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace dvb
{
	enum class CallbackMode { Serialized, Concurrent };

	// Shared by callbacks copied out of a registry/bus and their owner. Removing a
	// callback does not revoke copies already dispatched. Retire waits for admitted
	// calls and makes all later copies inert BEFORE the owner is destroyed.
	// The owner must not retire/destroy itself from inside an admitted callback.
	// Concurrent mode protects lifetime only: callers must synchronize owner data.
	// Serialized mode additionally orders callback bodies (the MCP default).
	template <class Owner, CallbackMode Mode = CallbackMode::Serialized>
	class CallbackLease
	{
	public:
		explicit CallbackLease(Owner& a_owner) : m_owner(&a_owner) {}
		CallbackLease(const CallbackLease&) = delete;
		CallbackLease& operator=(const CallbackLease&) = delete;

		template <class Fn>
		bool Invoke(Fn&& a_fn)
		{
			Owner* owner;
			{
				std::lock_guard lock(m_mutex);
				if (!m_owner)
					return false;
				owner = m_owner;
				++m_active;
			}
			// Drain even when the callback throws. No admission lock is held while
			// user code runs; closing admission cannot be starved by new calls.
			struct Admission
			{
				CallbackLease& lease;
				~Admission() { lease.Release(); }
			} admission{ *this };
			if constexpr (Mode == CallbackMode::Serialized) {
				std::lock_guard lock(m_executionMutex);
				std::forward<Fn>(a_fn)(*owner);
			} else {
				std::forward<Fn>(a_fn)(*owner);
			}
			return true;
		}

		void Retire()
		{
			std::unique_lock lock(m_mutex);
			m_owner = nullptr;
			m_drained.wait(lock, [this] { return m_active == 0; });
		}

	private:
		void Release()
		{
			std::lock_guard lock(m_mutex);
			if (--m_active == 0)
				m_drained.notify_all();
		}

		std::mutex m_mutex;
		std::mutex m_executionMutex;
		std::condition_variable m_drained;
		std::size_t m_active = 0;
		Owner* m_owner;
	};
}
