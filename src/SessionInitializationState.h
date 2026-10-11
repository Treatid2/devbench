#pragma once

#include <mutex>

namespace dvb
{
	// The dispatcher map and initialization flags share one owner mutex. A late
	// notification cannot recreate a flag after retirement removed its dispatcher.
	// This does not establish client custody or create/reopen a session.
	template<class DispatcherMap, class InitializedMap>
	bool SetRegisteredSessionInitialized(DispatcherMap& dispatchers,
		InitializedMap& initialized, std::mutex& mutex,
		const typename DispatcherMap::key_type& id, bool value)
	{
		std::lock_guard lock(mutex);
		const auto session = dispatchers.find(id);
		if (session == dispatchers.end() || !session->second || session->second->is_closed())
			return false;
		initialized.insert_or_assign(id, value);
		return true;
	}
}
