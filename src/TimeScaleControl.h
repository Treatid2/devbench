#pragma once

#include "Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dvb::TimeScaleControl
{
	// The game's normal speed, and the value a lease restores to when nothing else was set.
	inline constexpr double kNormalScale = 1.0;
	// Slowest supported scale (further down is a freeze, which needs its own flag).
	inline constexpr double kMinScale = 0.1;
	// Normal ceiling. kHighMaxScale is the hard ceiling, reachable only with an explicit allowHigh.
	inline constexpr double kMaxScale = 3.0;
	inline constexpr double kHighMaxScale = 10.0;
	inline constexpr double kFreezeScale = 0.0;
	// How close the engine's live multiplier must get to an acknowledged request to be reached
	// (the engine ramps toward a new multiplier when bChangeTimeMultSlowly is on).
	inline constexpr float        kEffectiveTolerance = 0.01F;
	inline constexpr std::int64_t kDefaultLeaseMs = 60000;
	inline constexpr std::int64_t kMaximumLeaseMs = 3600000;
	inline constexpr int kMaximumWaitMs = 30000;
	inline constexpr int kReceiptPollMs = 5;

	inline std::optional<int> ParseWaitMs(const json& a_value)
	{
		if (!a_value.is_number_integer()) return std::nullopt;
		if (a_value.is_number_unsigned()) {
			const auto value = a_value.get<std::uint64_t>();
			return value <= kMaximumWaitMs ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
		}
		const auto value = a_value.get<std::int64_t>();
		return value >= 0 && value <= kMaximumWaitMs ? std::optional<int>(static_cast<int>(value)) : std::nullopt;
	}

	enum class RequestEnd { active, expired, displaced, released };
	struct RequestRecord
	{
		std::uint64_t id;
		float requested;
		std::string owner;
		std::int64_t expiresAtWallMs;
		bool written = false;
		RequestEnd end = RequestEnd::active;
	};
	// Retained only by the current controller and callers holding this exact request.
	// No process-long history/map. All mutable record access requires the controller mutex.
	using Request = std::shared_ptr<RequestRecord>;

	struct Validation
	{
		bool        accepted = false;
		float       value = static_cast<float>(kNormalScale);
		std::string error;
	};

	// Pure: validate/clamp a requested scale. Rejects non-finite values, a freeze without its
	// flag, anything under kMinScale, and anything above kMaxScale/kHighMaxScale.
	inline Validation Validate(double a_scale, bool a_freeze, bool a_allowHigh)
	{
		const auto reject = [](std::string a_error) { return Validation{ false, static_cast<float>(kNormalScale), std::move(a_error) }; };
		if (!std::isfinite(a_scale))
			return reject("scale must be a finite number");
		if (a_freeze) {
			if (a_scale != kFreezeScale)
				return reject("freeze requires scale=0");
			return { true, static_cast<float>(kFreezeScale), {} };
		}
		if (a_scale == kFreezeScale)
			return reject("scale=0 freezes the game — pass freeze:true if that is intended");
		if (a_scale < kMinScale)
			return reject(std::format("scale must be at least {}", kMinScale));
		if (a_scale <= kMaxScale)
			return { true, static_cast<float>(a_scale), {} };
		if (a_scale > kHighMaxScale)
			return reject(std::format("scale must be at most {}", kHighMaxScale));
		if (!a_allowHigh)
			return reject(std::format("scale above {} needs allowHigh:true (hard maximum {})", kMaxScale, kHighMaxScale));
		return { true, static_cast<float>(a_scale), {} };
	}

	// Pure lease + reconcile state. Any thread may request; the main thread reconciles once per
	// engine frame. Never re-issues a value the engine already has, so a ramp set in motion by an
	// earlier write is not restarted.
	class Reconciler
	{
	public:
		// Sets a_value for a_owner until a_expiresAtWallMs (0 = no expiry). Every acquisition
		// synchronizes bookkeeping with its direct live sample BEFORE publishing the target.
		// Only a genuinely live previous lease retains its original restore baseline. Idle or
		// at/after-deadline replacement captures a_liveValue as a fresh baseline, even if the
		// old lease's frame-driven restoration has not run. The new command supersedes that
		// pending restoration; this does not claim that the old request was restored.
		TimeScaleControl::Request Request(float a_value, std::string a_owner, std::int64_t a_expiresAtWallMs,
			float a_liveValue = static_cast<float>(kNormalScale), std::int64_t a_nowWallMs = 0)
		{
			if (m_generation == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("time-scale request generation exhausted");
			auto request = std::make_shared<RequestRecord>(RequestRecord{ m_generation + 1, a_value, a_owner, a_expiresAtWallMs });
			const bool continuingLiveLease = Leased(a_nowWallMs);
			m_applied = a_liveValue;
			if (!continuingLiveLease) m_restoreValue = a_liveValue;
			EndRequest(m_request && m_request->expiresAtWallMs != 0 && a_nowWallMs >= m_request->expiresAtWallMs ?
				RequestEnd::expired : RequestEnd::displaced);
			++m_generation;
			m_request = request;
			m_requested = a_value;
			m_owner = std::move(a_owner);
			m_expiresAtWallMs = a_expiresAtWallMs;
			return request;
		}

		// Extends a lease only while a_owner still holds it, so an ad-hoc override that displaced a
		// run cannot be extended by the run it displaced.
		void RenewLease(std::string_view a_owner, std::int64_t a_expiresAtWallMs)
		{
			if (m_expiresAtWallMs != 0 && m_owner == a_owner) {
				m_expiresAtWallMs = a_expiresAtWallMs;
				if (m_request) m_request->expiresAtWallMs = a_expiresAtWallMs;
			}
		}

		// Requests the baseline value and drops the lease: an empty a_owner drops whoever holds it
		// (an explicit return to normal speed), a named one only releases that holder's own lease.
		void Release(std::string_view a_owner = {})
		{
			if (!a_owner.empty() && m_owner != a_owner)
				return;
			EndRequest(RequestEnd::released);
			m_expiresAtWallMs = 0;
			m_owner.clear();
			m_requested = m_restoreValue;
		}

		// Caller serializes request publication AND this synchronous writer. Only a true
		// return after issuing the native setter acknowledges a write; absent timers retry
		// on a later frame without publishing fictional applied bookkeeping. Not convergence.
		template <class Writer>
		std::optional<float> Reconcile(std::int64_t a_nowWallMs, Writer&& a_write)
		{
			if (m_expiresAtWallMs != 0 && a_nowWallMs >= m_expiresAtWallMs) {
				EndRequest(RequestEnd::expired);
				m_expiresAtWallMs = 0;
				m_owner.clear();
				m_requested = m_restoreValue;
			}
			if (m_requested == m_applied)
				return std::nullopt;
			const float value = m_requested;
			const auto request = m_request;
			if (!a_write(value)) return std::nullopt;
			m_applied = value;
			if (request && request->end == RequestEnd::active && request->requested == value)
				request->written = true;
			return value;
		}

		json Receipt(const TimeScaleControl::Request& a_request, std::int64_t a_now, float a_live) const
		{
			const bool current = m_request == a_request;
			auto end = a_request->end;
			if (end == RequestEnd::active && a_request->expiresAtWallMs != 0 && a_now >= a_request->expiresAtWallMs)
				end = RequestEnd::expired;
			const bool owns = current && end == RequestEnd::active;
			const bool atTarget = std::isfinite(a_live) && std::fabs(a_live - a_request->requested) <= kEffectiveTolerance;
			const bool reached = owns && a_request->written && atTarget;
			const bool unchanged = owns && !a_request->written && !Pending() && atTarget;
			const char* state = end == RequestEnd::expired ? "expired" : end == RequestEnd::displaced ? "displaced" :
				end == RequestEnd::released ? "released" : reached ? "reached" : unchanged ? "unchanged" :
				a_request->written ? "written" : "accepted";
			return json{
				{ "requestId", a_request->id }, { "requested", a_request->requested }, { "owner", a_request->owner },
				{ "accepted", true }, { "applied", a_request->written }, { "reached", reached }, { "atTarget", atTarget },
				{ "state", state }, { "ownsController", owns }, { "effective", a_live }, { "sampledWallMs", a_now },
				{ "leased", owns && a_request->expiresAtWallMs != 0 },
				{ "leaseRemainingMs", owns && a_request->expiresAtWallMs != 0 ? a_request->expiresAtWallMs - a_now : 0 },
				{ "currentRequested", Requested() }, { "currentOwner", Owner() },
				{ "currentRequestId", m_request ? m_request->id : 0 },
			};
		}

		float       Effective() const { return m_applied; }
		float       Requested() const { return m_requested; }
		std::string Owner() const { return m_owner; }
		bool        Leased(std::int64_t a_nowWallMs) const
		{
			return m_expiresAtWallMs != 0 && a_nowWallMs < m_expiresAtWallMs;
		}
		// True until the requested setter has been issued (not engine convergence).
		bool         Pending() const { return m_requested != m_applied; }
		std::int64_t LeaseRemainingMs(std::int64_t a_nowWallMs) const
		{
			return m_expiresAtWallMs == 0 || a_nowWallMs >= m_expiresAtWallMs ? 0 : m_expiresAtWallMs - a_nowWallMs;
		}

	private:
		void EndRequest(RequestEnd a_end)
		{
			if (m_request && m_request->end == RequestEnd::active) m_request->end = a_end;
		}
		std::uint64_t m_generation = 0;
		TimeScaleControl::Request m_request;
		float        m_applied = static_cast<float>(kNormalScale);
		float        m_requested = static_cast<float>(kNormalScale);
		std::string  m_owner;
		float        m_restoreValue = static_cast<float>(kNormalScale);
		std::int64_t m_expiresAtWallMs = 0;
	};

	// --- engine-facing (src/TimeScaleControl.cpp) ---

	struct SetResult
	{
		bool        ok = false;
		std::string error;
		Request request;
	};

	// Pure production-used wait seam. Admission and convergence timings are distinct;
	// waitMs bounds requested convergence sleeps, not mutex/native/scheduler/transport latency.
	// The final returned snapshot alone supplies effective/reached/owner/state and note.
	template <class Observe, class Clock, class Sleep>
	json AwaitReceipt(std::int64_t a_started, std::int64_t a_admitted, int a_waitMs,
		Observe&& a_observe, Clock&& a_clock, Sleep&& a_sleep)
	{
		const auto deadline = a_admitted + a_waitMs;
		json snapshot;
		std::int64_t now;
		for (;;) {
			snapshot = a_observe();
			now = a_clock();
			const auto state = snapshot.at("state").template get<std::string>();
			if (snapshot.at("reached").template get<bool>() || state == "expired" || state == "displaced" ||
				state == "released" || state == "unchanged" || now >= deadline) break;
			a_sleep(std::min<std::int64_t>(kReceiptPollMs, deadline - now));
		}
		snapshot["admissionMs"] = a_admitted - a_started;
		snapshot["convergenceWaitedMs"] = now - a_admitted;
		snapshot["waitedMs"] = now - a_admitted;  // legacy alias, NOT total elapsed
		snapshot["totalElapsedMs"] = now - a_started;
		snapshot["convergenceBudgetMs"] = a_waitMs;
		snapshot["convergenceDeadlineExceeded"] = now > deadline;
		snapshot["note"] = std::format("request {} is {}; sampled effective={}, requested={}. applied acknowledges only this request's issued setter; equality alone is atTarget, not ownership proof.",
			snapshot.at("requestId").template get<std::uint64_t>(), snapshot.at("state").template get<std::string>(),
			snapshot.at("effective").template get<double>(), snapshot.at("requested").template get<double>());
		return snapshot;
	}

	// Shared serialization point for the moment recording start, capture start, and a
	// non-normal time-scale request each decide whether to proceed against the others:
	// whichever locks it first excludes the rest until it commits its own state (or aborts).
	// Set() locks it internally; Recording/Capture hold it across their own admission check
	// + state transition so the two can never interleave.
	std::mutex& AdmissionMutex();

	// Any thread, no queued engine wait: reserves a request under the admission mutex;
	// the main-thread reconciler issues it on a later frame. Mutex contention is possible.
	// Refuses a non-normal scale while a recording or a capture is in flight unless
	// a_allowTimeScale. a_holdMs <= 0 uses kDefaultLeaseMs.
	SetResult Set(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale);
	json SetAndWait(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale, int a_waitMs);

	// Main thread, once per engine frame (driven by GameClock::Tick).
	void Reconcile();

	// Direct observation of the engine's live global time multiplier — what the game is actually
	// running at, which lags a request while the engine ramps toward it.
	float Effective();

	// { requested, effective, owner, leased, leaseRemainingMs }.
	json Status();

	// A run's hold on the scale: set for the run's duration and restored when the scope ends, on
	// any exit path. Logs every change the reconciler issues while it is open, so a run can report
	// whether its captures were taken at a comparable speed. Throws ToolError(409) if the request
	// is refused (a recording or capture in flight without a_allowTimeScale).
	class RunHold
	{
	public:
		RunHold(float a_scale, std::int64_t a_holdMs, const std::string& a_owner, bool a_allowTimeScale);
		~RunHold();
		RunHold(const RunHold&) = delete;
		RunHold& operator=(const RunHold&) = delete;
		RunHold(RunHold&&) = delete;
		RunHold& operator=(RunHold&&) = delete;

		// True when the run's whole window ran at the normal scale.
		bool Eligible() const;
		// [{ gameMs, effective }] — one entry per change issued while the run was open.
		json Changes() const;

		// Raises the hold to a_scale partway through an already-open run (e.g. once its setup/
		// settle phase ends and the recorded trajectory begins), instead of for the whole run.
		// Same refusal/restore semantics as the constructor: throws ToolError(409) if refused, and
		// whatever scale is current when this RunHold is destroyed is what gets released.
		void Escalate(float a_scale, std::int64_t a_holdMs, bool a_allowTimeScale);

	private:
		std::string m_owner;
		bool        m_open = false;
		bool        m_restore = false;
	};
}
