#pragma once

#include <mutex>

namespace dvb
{
	// Only roll back the exact dispatcher allocated by this failed initialization.
	// Retirement and a replacement with the same key must not transfer ownership.
	// This helper is for unpublished Streamable HTTP allocations, not SSE worker
	// teardown, foreign client cleanup, or a reset of the session capacity policy.
	template<class DispatcherMap, class InitializedMap>
	bool RollbackSessionRegistration(DispatcherMap& dispatchers,
		InitializedMap& initialized, std::mutex& mutex,
		const typename DispatcherMap::key_type& id,
		const typename DispatcherMap::mapped_type& expected)
	{
		std::lock_guard lock(mutex);
		const auto session = dispatchers.find(id);
		if (session == dispatchers.end() || !expected || session->second != expected)
			return false;
		initialized.erase(id);
		dispatchers.erase(session);
		return true;
	}
}
