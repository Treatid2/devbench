#pragma once

#include "Json.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

namespace dvb::NewGameControl
{
	// Game-thread-only receipt authority. Movie cleanup cannot resolve a mutation
	// whose GFx effects are unknown. Kept engine-free for focused regression tests.
	class RequestLedger
	{
	public:
		static constexpr std::size_t kMaximumRequests = 64;

		bool Contains(const std::string& a_id) const { return _records.contains(a_id); }
		bool IsRequested(const std::string& a_id) const
		{
			const auto found = _records.find(a_id);
			return found != _records.end() && found->second.phase == Phase::kRequested;
		}
		std::string UnresolvedID() const
		{
			for (const auto& [id, record] : _records)
				if (record.phase == Phase::kUncertain)
					return id;
			return {};
		}
		bool CanRequest() const { return _records.size() < kMaximumRequests && UnresolvedID().empty(); }

		void BeginRequest(const std::string& a_id, std::uint32_t a_row)
		{
			if (!CanRequest() || Contains(a_id))
				throw std::logic_error("New Game request blocked by retained history or unresolved dispatch");
			_records.emplace(a_id, Record{ Phase::kUncertain, false, {},
				json{ { "requestId", a_id }, { "newRow", a_row }, { "accepted", false }, { "completed", false } } });
		}
		void CompleteRequest(const std::string& a_id, const std::string& a_state)
		{
			auto& record = _records.at(a_id);
			if (record.phase != Phase::kUncertain || record.confirming)
				throw std::logic_error("New Game request completion without its started mutation");
			record.details["state"] = a_state;
			record.phase = Phase::kRequested;  // Only after all post-dispatch reads succeeded.
		}
		void BeginConfirmation(const std::string& a_id)
		{
			if (!IsRequested(a_id) || !UnresolvedID().empty())
				throw std::logic_error("New Game confirmation is not ready or has unresolved dispatch");
			auto& record = _records.at(a_id);
			record.confirming = true;
			record.phase = Phase::kUncertain;
		}
		void CompleteConfirmation(const std::string& a_id)
		{
			auto& record = _records.at(a_id);
			if (record.phase != Phase::kUncertain || !record.confirming)
				throw std::logic_error("New Game confirmation completion without its started mutation");
			record.details["accepted"] = true;
			record.phase = Phase::kDispatched;
		}
		void Invalidate(const std::string& a_id, const std::string& a_reason)
		{
			auto& record = _records.at(a_id);
			record.invalidationReason = a_reason;
			if (record.phase == Phase::kRequested)
				record.phase = Phase::kInvalidated;
			// Uncertain stays uncertain; already acknowledged dispatch stays dispatched.
		}
		json Snapshot(const std::string& a_id) const
		{
			const auto& record = _records.at(a_id);
			auto result = record.details;
			result["phase"] = record.phase == Phase::kUncertain ? "dispatchUncertain" :
			                  record.phase == Phase::kRequested ? "requested" :
			                  record.phase == Phase::kDispatched ? "dispatched" : record.invalidationReason;
			result["unresolvedDispatch"] = record.phase == Phase::kUncertain;
			if (!record.invalidationReason.empty())
				result["invalidationReason"] = record.invalidationReason;
			if (record.phase == Phase::kUncertain)
				result["recovery"] = "Await definitive completion of this same task; otherwise restart the process. A fresh ID or menu change cannot resolve uncertainty.";
			if (record.phase == Phase::kDispatched)
				result["note"] = "Normal confirmation accepted; fade/engine initialization is asynchronous. Verify RaceSex Menu and world state separately.";
			return result;
		}

	private:
		enum class Phase { kUncertain, kRequested, kDispatched, kInvalidated };
		struct Record
		{
			Phase phase;
			bool confirming;
			std::string invalidationReason;
			json details;
		};
		std::map<std::string, Record> _records;
	};
}
