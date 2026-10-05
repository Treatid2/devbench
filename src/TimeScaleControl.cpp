#include "TimeScaleControl.h"

#include "Capture.h"
#include "GameClock.h"
#include "Recording.h"
#include "ToolRegistry.h"

#include <RE/B/BSTimer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace dvb::TimeScaleControl
{
	namespace
	{
		struct Change
		{
			double gameMs;
			float  effective;
		};

		std::mutex          g_mutex;
		Reconciler          g_reconciler;
		bool                g_leaseEngaged = false;
		bool                g_runActive = false;
		std::string         g_runOwner;
		std::vector<Change> g_changes;
		thread_local bool g_inWrite = false;

		// Written by the main-thread reconciler, read by any thread.
		std::atomic<float> g_liveMultiplier{ static_cast<float>(kNormalScale) };

		std::int64_t NowWallMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
			    .count();
		}

		// Called with g_mutex held. Apply the returned transition under that same lock,
		// so competing reservations cannot reorder pump engagement effects.
		std::optional<bool> LatchEngagement(bool a_leased)
		{
			if (g_leaseEngaged == a_leased)
				return std::nullopt;
			g_leaseEngaged = a_leased;
			return a_leased;
		}

		void ApplyEngagement(const std::optional<bool>& a_engagement)
		{
			if (!a_engagement)
				return;
			if (*a_engagement)
				GameClock::Engage();
			else
				GameClock::Disengage();
		}

		// True until the ENGINE itself (not our own bookkeeping) reports the requested value —
		// Reconciler::Pending() clears too early, mid-ramp, and would stop the pump prematurely.
		bool NeedsPump(std::int64_t a_now)
		{
			return g_reconciler.Leased(a_now) || g_reconciler.Pending() ||
			       std::fabs(g_liveMultiplier.load(std::memory_order_acquire) - g_reconciler.Requested()) >
			           kEffectiveTolerance;
		}
	}

	std::mutex& AdmissionMutex() { return g_mutex; }

	SetResult Set(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale)
	{
		// An engine hook must not re-enter reservation while the synchronous setter
		// holds the publication mutex. The public ABI reports refusal, never self-waits.
		if (g_inWrite) return { false, "reentrant time-scale request during native setter", {} };
		const std::int64_t holdMs = a_holdMs > 0 ? std::min<std::int64_t>(a_holdMs, kMaximumLeaseMs) : kDefaultLeaseMs;

		Request request;
		{
			// The admission check and the reservation it gates must be atomic with respect to
			// Recording::start/Capture::Handle's own check-and-commit (same mutex) — otherwise a
			// recording/capture could start in the gap between this check and g_reconciler.Request.
			std::lock_guard lock(g_mutex);
			if (a_scale != static_cast<float>(kNormalScale) && !a_allowTimeScale) {
				if (Recording::IsActive())
					return { false, "a recording is in progress — stop it first, or pass allowTimeScale:true to change the game's speed anyway" };
				if (Capture::InFlight())
					return { false, "a capture is in flight — retry once it finishes, or pass allowTimeScale:true to change the game's speed anyway" };
			}
			const std::int64_t now = NowWallMs();
			// Same direct read used by the existing any-thread Effective() API, not a
			// stale cache or queued main-thread round trip (including main-thread ABI callers).
			// This is an observation, not engine-wide locking against other mods.
			const float liveNow = Effective();
			if (a_scale == static_cast<float>(kNormalScale))
				// Request kNormalScale directly rather than Release()'s lease-restore baseline:
				// an explicit "set scale to 1" means exactly that, not "whatever it was before
				// devbench's current hold started".
				request = g_reconciler.Request(static_cast<float>(kNormalScale), {}, 0, liveNow, now);
			else
				request = g_reconciler.Request(a_scale, a_owner, now + holdMs, liveNow, now);
			// Request consumes that same sample atomically: idle/expired baseline capture,
			// current bookkeeping and target publication cannot be split or reordered here.
			ApplyEngagement(LatchEngagement(NeedsPump(now)));
		}
		return { true, {}, std::move(request) };
	}

	json SetAndWait(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale, int a_waitMs)
	{
		const auto started = NowWallMs();
		const auto set = Set(a_scale, a_holdMs, a_owner, a_allowTimeScale);
		if (!set.ok) throw ToolError(409, std::format("game setTimeScale: {}", set.error));
		const auto admitted = NowWallMs();
		return AwaitReceipt(started, admitted, a_waitMs, [&] {
			std::lock_guard lock(g_mutex);
			const float live = Effective();  // exactly one live sample for this immutable receipt
			return g_reconciler.Receipt(set.request, NowWallMs(), live);
		}, &NowWallMs, [](std::int64_t a_ms) { std::this_thread::sleep_for(std::chrono::milliseconds(a_ms)); });
	}

	void Reconcile()
	{
		const std::int64_t   now = NowWallMs();
		std::optional<float> value;
		{
			std::lock_guard lock(g_mutex);
			const float     previousLive = g_liveMultiplier.load(std::memory_order_relaxed);
			const float     live = RE::BSTimer::QGlobalTimeMultiplier();
			g_liveMultiplier.store(live, std::memory_order_release);
			// A run in flight keeps its own lease alive, so a long one never expires mid-run.
			if (g_runActive)
				g_reconciler.RenewLease(g_runOwner, now + kDefaultLeaseMs);
			// Publication, native invocation and acknowledgement are one controller
			// critical section: a later request cannot be acknowledged for an earlier write.
			// The void native setter only proves issuance, not convergence or exclusivity.
			value = g_reconciler.Reconcile(now, [](float a_value) {
				auto* timer = RE::BSTimer::GetSingleton();
				if (!timer) return false;
				struct Writing {
					Writing() { g_inWrite = true; }
					~Writing() { g_inWrite = false; }
				} writing;
				timer->SetGlobalTimeMultiplier(a_value, false);
				return true;
			});
			if (g_runActive) {
				if (value)
					g_changes.push_back({ GameClock::Now(), *value });
				else if (std::fabs(live - previousLive) > kEffectiveTolerance)
					// Nothing we issued moved the engine — an external sgtm/console change did, so
					// the run must still be flagged ineligible for a golden comparison.
					g_changes.push_back({ GameClock::Now(), live });
			}
			ApplyEngagement(LatchEngagement(NeedsPump(now)));
		}
	}

	float Effective()
	{
		// Sampled directly rather than only trusting the Reconcile-refreshed cache: Reconcile only
		// runs while the pump is engaged, so an external change (console sgtm, another mod) is
		// otherwise invisible to every caller here — Capture::Handle, Recording::Handle, Status,
		// Set — until some devbench-side lease happens to engage the clock. Also refreshes the
		// cache so NeedsPump/Reconcile's own diffing stays in sync with what callers just saw.
		const float live = RE::BSTimer::QGlobalTimeMultiplier();
		g_liveMultiplier.store(live, std::memory_order_release);
		return live;
	}

	json Status()
	{
		if (g_inWrite) throw ToolError(409, "time-scale status unavailable during reentrant native setter");
		std::lock_guard    lock(g_mutex);
		const std::int64_t now = NowWallMs();
		return json{
			{ "requested", g_reconciler.Requested() },
			{ "effective", Effective() },
			{ "owner", g_reconciler.Owner() },
			{ "leased", g_reconciler.Leased(now) },
			{ "leaseRemainingMs", g_reconciler.LeaseRemainingMs(now) },
		};
	}

	RunHold::RunHold(float a_scale, std::int64_t a_holdMs, const std::string& a_owner,
		bool a_allowTimeScale) :
		m_owner(a_owner)
	{
		// A throw here must happen before anything below is touched: a constructor that throws
		// never runs its own destructor, so a refusal after Engage()/g_runActive would leak them.
		if (a_scale != static_cast<float>(kNormalScale)) {
			const SetResult set = Set(a_scale, a_holdMs, a_owner, a_allowTimeScale);
			if (!set.ok)
				throw ToolError(409, std::format("time scale refused: {}", set.error));
			m_restore = true;
		}
		GameClock::Engage();
		const float startEffective = Effective();
		{
			std::lock_guard lock(g_mutex);
			g_runActive = true;
			g_runOwner = a_owner;
			g_changes.clear();
			// A run that starts non-normal is already incomparable to a golden, so record why.
			if (std::fabs(startEffective - static_cast<float>(kNormalScale)) > kEffectiveTolerance)
				g_changes.push_back({ GameClock::Now(), startEffective });
		}
		m_open = true;  // every path from here out is covered by the destructor
	}

	RunHold::~RunHold()
	{
		if (!m_open)
			return;
		{
			std::lock_guard lock(g_mutex);
			g_runActive = false;
			g_runOwner.clear();
			if (m_restore)
				g_reconciler.Release(m_owner);
			// NeedsPump, not just Leased: keeps pumping until the engine actually converges.
			const std::int64_t now = NowWallMs();
			ApplyEngagement(LatchEngagement(NeedsPump(now)));
		}
		GameClock::Disengage();
	}

	void RunHold::Escalate(float a_scale, std::int64_t a_holdMs, bool a_allowTimeScale)
	{
		if (a_scale == static_cast<float>(kNormalScale))
			return;
		const SetResult set = Set(a_scale, a_holdMs, m_owner, a_allowTimeScale);
		if (!set.ok)
			throw ToolError(409, std::format("time scale refused: {}", set.error));
		m_restore = true;
	}

	bool RunHold::Eligible() const
	{
		std::lock_guard lock(g_mutex);
		return g_changes.empty();
	}

	json RunHold::Changes() const
	{
		std::lock_guard lock(g_mutex);
		json            out = json::array();
		for (const auto& change : g_changes)
			out.push_back(json{ { "gameMs", change.gameMs }, { "effective", change.effective } });
		return out;
	}
}
