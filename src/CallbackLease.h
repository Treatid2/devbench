#pragma once

#include <mutex>
#include <utility>

namespace dvb
{
	// Shared by callbacks copied out of a registry/bus and their owner. Removing a
	// callback does not revoke copies already dispatched. Retire waits for admitted
	// calls and makes all later copies inert BEFORE the owner is destroyed.
	// The owner must not retire/destroy itself from inside an admitted callback.
	template <class Owner>
	class CallbackLease
	{
	public:
		explicit CallbackLease(Owner& a_owner) : m_owner(&a_owner) {}
		CallbackLease(const CallbackLease&) = delete;
		CallbackLease& operator=(const CallbackLease&) = delete;

		template <class Fn>
		bool Invoke(Fn&& a_fn)
		{
			std::lock_guard lock(m_mutex);
			if (!m_owner)
				return false;
			std::forward<Fn>(a_fn)(*m_owner);
			return true;
		}

		void Retire()
		{
			std::lock_guard lock(m_mutex);
			m_owner = nullptr;
		}

	private:
		std::mutex m_mutex;
		Owner* m_owner;
	};
}
