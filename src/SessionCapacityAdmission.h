#pragma once

#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace dvb
{
	// All admission and retirement of this map must use the same owner mutex.
	// Count and insertion are one transaction; constructing the candidate before
	// calling this function does not consume a slot. Zero preserves unlimited mode.
	// This is not idle expiry, caller custody, or session-worker lifetime ownership.
	template<class Map>
	bool AdmitSession(Map& sessions, std::mutex& mutex, std::size_t maximum,
		const typename Map::key_type& id, const typename Map::mapped_type& dispatcher)
	{
		std::lock_guard lock(mutex);
		if (maximum != 0 && sessions.size() >= maximum)
			return false;
		if (!sessions.emplace(id, dispatcher).second)
			throw std::runtime_error("MCP session ID collision; existing session retained");
		return true;
	}
}
