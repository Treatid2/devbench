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
		enum class State { Idle, Starting, Disabled, Ready, Failed };
		enum class Phase { Configuration, Construction, Preparation, Listening, Publication };
		enum class Outcome { Disabled, Ready, Failed, AlreadyAttempted };

		bool Ready() const noexcept { return m_state.load() == State::Ready; }
		State Current() const noexcept { return m_state.load(); }

		template <class Operations>
		Outcome Run(Operations& a_ops) noexcept
		{
			State expected = State::Idle;
			if (!m_state.compare_exchange_strong(expected, State::Starting))
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
