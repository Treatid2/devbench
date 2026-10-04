#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace dvb::CalendarControl
{
	inline constexpr std::int64_t kMaximumHoldMs = 300000;

	struct Source
	{
		std::string                       processSession;
		std::uint64_t                     generation = 0;
		std::uint32_t                     cell = 0;
		std::array<std::uint32_t, 6>       forms{};
		std::array<std::uintptr_t, 6>      storage{};
		std::uintptr_t                    calendar = 0;
		bool operator==(const Source&) const = default;

		bool SameStorage(const Source& a_other) const
		{
			return processSession == a_other.processSession && generation == a_other.generation &&
			       forms == a_other.forms && storage == a_other.storage && calendar == a_other.calendar;
		}
	};

	struct Snapshot
	{
		Source               source;
		bool                 available = false;
		bool                 worldLoaded = false;
		std::array<float, 5> date{};  // year, month, day, hour, days passed
		float                rate = 0;
		float                engineMultiplier = 0;
		int                  frame = -1;
	};

	/// WriteRate returns true only for a committed write; false must not mutate the backend.
	struct Backend
	{
		virtual ~Backend() = default;
		virtual Snapshot Read() = 0;
		virtual bool WriteRate(const Snapshot& a_expected, float a_rate) = 0;
	};

	struct Outcome
	{
		bool        ok = true;
		std::string code = "idle";
		bool        restored = false;
	};

	struct Lease
	{
		Snapshot     baseline;
		std::string  owner, connection, command, id;
		std::int64_t deadline = 0;
		bool         applied = false;
		bool         cleanupAttempted = false;
	};

	/// Main-thread policy; the transport wrapper serializes access and supplies monotonic time.
	class Controller
	{
	public:
		explicit Controller(Backend& a_backend) : _backend(a_backend) {}
		const std::optional<Lease>& Current() const { return _lease; }
		const std::optional<Lease>& Last() const { return _last; }
		const Outcome& Result() const { return _result; }
		bool NeedsPump() const { return _lease && !_lease->cleanupAttempted; }

		Outcome Hold(const Source& a_expected, const std::string& a_owner,
			const std::string& a_connection, const std::string& a_command,
			std::int64_t a_now, std::int64_t a_duration, std::int64_t a_applyDeadline)
		{
			Tick(a_now);
			if (_lease)
				return Refuse("lease_exists");
			if (a_owner.empty() || a_owner.size() > 128 || a_command.empty() || a_command.size() > 128 ||
			    a_duration < 1 || a_duration > kMaximumHoldMs || a_now >= a_applyDeadline ||
			    a_now > std::numeric_limits<std::int64_t>::max() - a_duration)
				return Refuse("invalid_or_expired_request");
			const auto s = _backend.Read();
			if (!s.available || !s.worldLoaded || s.frame < 0 || s.source != a_expected)
				return Refuse("source_unavailable_or_changed");
			if (!std::isfinite(s.rate) || s.rate <= 0 || !std::isfinite(s.engineMultiplier) || s.engineMultiplier <= 0)
				return Refuse("unsupported_clock_state");
			for (const auto value : s.date)
				if (!std::isfinite(value))
					return Refuse("invalid_calendar_values");
			if (s.date[0] < 0 || s.date[1] < 0 || s.date[1] >= 12 || s.date[2] < 1 || s.date[2] > 31 ||
			    s.date[3] < 0 || s.date[3] >= 24 || s.date[4] < 0)
				return Refuse("invalid_calendar_values");
			_lease = Lease{ s, a_owner, a_connection, a_command,
				std::to_string(++_sequence), a_now + a_duration };
			const bool wrote = _backend.WriteRate(s, 0);
			_lease->applied = wrote;
			const auto after = _backend.Read();
			if (!wrote)
				return Forget({ false, "apply_refused_without_write", false });
			if (after.available && after.source.SameStorage(s.source) && after.rate == 0) {
				if (after.date != s.date || after.engineMultiplier != s.engineMultiplier || !after.worldLoaded || after.source.cell != s.source.cell) {
					Restore("apply_state_changed");
					_result.ok = false;
					return _result;
				}
				_result = { true, "held", false };
				return _result;
			}
			if (after.available && after.source.SameStorage(s.source) && after.rate == s.rate)
				return Forget({ false, "apply_failed_without_change", false });
			// An uncertain write remains tracked until expiry or explicit reconciliation.
			_result = { false, "apply_unverified", false };
			return _result;
		}

		Outcome Release(const Source& a_expected, const std::string& a_owner,
			const std::string& a_connection, const std::string& a_id)
		{
			const auto& selected = _lease ? _lease : _last;
			if (!selected || selected->id != a_id || selected->owner != a_owner || selected->connection != a_connection)
				return Refuse("wrong_owner_or_lease");
			if (selected->baseline.source != a_expected)
				return Refuse("binding_mismatch");
			if (!_lease)
				return _result;
			return Restore("released");
		}

		Outcome Tick(std::int64_t a_now)
		{
			if (!_lease || _lease->cleanupAttempted)
				return _result;
			const auto s = _backend.Read();
			if (!s.available || !s.source.SameStorage(_lease->baseline.source))
				return Forget({ false, "source_invalidated_without_restore", false });
			if (s.rate != 0)
				return Forget({ false, "external_rate_write_without_restore", false });
			if (s.date != _lease->baseline.date)
				return Restore("external_calendar_change");
			if (s.engineMultiplier != _lease->baseline.engineMultiplier)
				return Restore("engine_multiplier_changed");
			if (!s.worldLoaded || s.source.cell != _lease->baseline.source.cell)
				return Restore("scene_lost");
			if (a_now >= _lease->deadline)
				return Restore("expired");
			return _result;
		}

		Outcome End(const std::string& a_reason, bool a_restoreAllowed)
		{
			if (!_lease)
				return _result;
			if (a_restoreAllowed)
				Restore(a_reason);
			if (_lease)
				return Forget({ false, a_reason + "_invalidated_without_restore", false });
			return _result;
		}

	private:
		static Outcome Refuse(const std::string& a_code) { return { false, a_code, false }; }
		Outcome Forget(Outcome a_result)
		{
			_last = std::move(_lease);
			_lease.reset();
			_result = std::move(a_result);
			return _result;
		}

		Outcome Restore(const std::string& a_reason)
		{
			const auto s = _backend.Read();
			if (!s.available || !s.source.SameStorage(_lease->baseline.source))
				return Forget({ false, "source_invalidated_without_restore", false });
			if (s.rate != 0)
				return Forget({ false, "external_rate_write_without_restore", false });
			_lease->cleanupAttempted = true;
			const bool wrote = _backend.WriteRate(s, _lease->baseline.rate);
			const auto after = _backend.Read();
			if (wrote && after.available && after.source.SameStorage(s.source) && after.rate == _lease->baseline.rate)
				return Forget({ true, a_reason, true });
			_result = { false, "restore_failed_requires_explicit_release", false };
			return _result;
		}

		Backend&             _backend;
		std::optional<Lease> _lease, _last;
		Outcome              _result;
		std::uint64_t        _sequence = 0;
	};
}
