#pragma once

#include <cstddef>
#include <functional>
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
		const typename Map::key_type& id, const typename Map::mapped_type& dispatcher,
		const bool* admissionOpen = nullptr, bool* ownerClosed = nullptr,
		const std::function<bool()>* requestAdmission = nullptr,
		std::size_t* countBeforeAdmission = nullptr)
	{
		std::lock_guard lock(mutex);
		// Diagnostic snapshot at the admission boundary, not a later unlocked
		// map read. The caller may log it after this owner lock is released.
		if (countBeforeAdmission) *countBeforeAdmission = sessions.size();
		if (ownerClosed) *ownerClosed = false;
		// If supplied, this flag is read/written only under this same owner mutex.
		// The immutable embedding predicate must only read its retained readiness
		// token; no reentry or locks/callback work under the session owner mutex.
		// Exceptions fail closed before insertion and consume no session slot.
		bool committed = true;
		try { if (requestAdmission) committed = (*requestAdmission)(); }
		catch (...) { committed = false; }
		if ((admissionOpen && !*admissionOpen) || !committed) {
			if (ownerClosed) *ownerClosed = true;
			return false;
		}
		if (maximum != 0 && sessions.size() >= maximum)
			return false;
		if (!sessions.emplace(id, dispatcher).second)
			throw std::runtime_error("MCP session ID collision; existing session retained");
		return true;
	}
}
