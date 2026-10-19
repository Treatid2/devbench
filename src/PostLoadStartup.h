#pragma once

#include <atomic>
#include <exception>

namespace dvb
{
	// The actual PostLoad operation sequence, shared with failure-source tests.
	// Once attempted, it never repeats publication/hook installs in this process.
	// Failure does NOT certify transport rollback: owners remain quarantined until
	// the transport's cleanup contract is established separately.
	class PostLoadStartup
	{
	public:
		enum class State { Idle, Preparing, Prepared, Starting, Disabled, Ready, Failed };
		enum class Phase { Configuration, Construction, Preparation, Scheduling, Listening, Publication };
		enum class Outcome { Prepared, Disabled, Ready, Failed, AlreadyAttempted };

		bool Ready() const noexcept { return m_state.load() == State::Ready; }
		State Current() const noexcept { return m_state.load(); }

		template <class Operations>
		Outcome Prepare(Operations& a_ops) noexcept
		{
			State expected = State::Idle;
			if (!m_state.compare_exchange_strong(expected, State::Preparing))
				return Outcome::AlreadyAttempted;

			Phase phase = Phase::Configuration;
			try {
				if (!a_ops.Configure()) {
					m_state.store(State::Disabled);
					Report(a_ops, Outcome::Disabled, phase, {});
					return Outcome::Disabled;
				}
				phase = Phase::Construction;
				a_ops.Construct();
				phase = Phase::Preparation;
				a_ops.Prepare();
				m_state.store(State::Prepared);
				return Outcome::Prepared;
			} catch (...) {
				return Fail(a_ops, phase, std::current_exception());
			}
		}

		// Must run on the application's qualified safe main-thread task phase,
		// never inline in a loader callback. All worker construction/waits/rollback
		// stay in this half; the retained Operations owner outlives the queued task.
		template <class Operations>
		Outcome Complete(Operations& a_ops) noexcept
		{
			State expected = State::Prepared;
			if (!m_state.compare_exchange_strong(expected, State::Starting))
				return Outcome::AlreadyAttempted;
			Phase phase = Phase::Listening;
			try {
				phase = Phase::Listening;
				if (!a_ops.Listen())
					return Fail(a_ops, phase, {});
				phase = Phase::Publication;
				a_ops.Publish();
				m_state.store(State::Ready);
			} catch (...) {
				return Fail(a_ops, phase, std::current_exception());
			}
			// Reporting/discovery is optional: diagnostics must neither escape to
			// SKSE nor turn a committed service into a false failed-start claim.
			Report(a_ops, Outcome::Ready, phase, {});
			return Outcome::Ready;
		}

		template <class Operations>
		Outcome SchedulingFailed(Operations& a_ops, std::exception_ptr error) noexcept
		{
			State expected = State::Prepared;
			if (!m_state.compare_exchange_strong(expected, State::Starting))
				return Outcome::AlreadyAttempted;
			return Fail(a_ops, Phase::Scheduling, error);
		}

		template <class Operations>
		Outcome PublicationFailed(Operations& a_ops, std::exception_ptr error) noexcept
		{
			State expected = State::Ready;
			if (!m_state.compare_exchange_strong(expected, State::Starting))
				return Outcome::AlreadyAttempted;
			return Fail(a_ops, Phase::Publication, error);
		}

		// Existing ordinary-host callers may run both halves in a safe context.
		template <class Operations>
		Outcome Run(Operations& a_ops) noexcept
		{
			const auto prepared = Prepare(a_ops);
			return prepared == Outcome::Prepared ? Complete(a_ops) : prepared;
		}

	private:
		template <class Operations>
		Outcome Fail(Operations& a_ops, Phase a_phase, std::exception_ptr a_error) noexcept
		{
			m_state.store(State::Failed);
			try { a_ops.Quarantine(); } catch (...) {}
			Report(a_ops, Outcome::Failed, a_phase, a_error);
			return Outcome::Failed;
		}

		template <class Operations>
		static void Report(Operations& a_ops, Outcome a_outcome, Phase a_phase, std::exception_ptr a_error) noexcept
		{
			try { a_ops.Report(a_outcome, a_phase, a_error); } catch (...) {}
		}

		std::atomic<State> m_state{ State::Idle };
	};
}
