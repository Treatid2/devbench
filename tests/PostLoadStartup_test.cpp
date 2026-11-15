#include "test_framework.h"
#include "PostLoadStartup.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using Startup = dvb::PostLoadStartup;
	using Phase = Startup::Phase;
	using Outcome = Startup::Outcome;

	// Inject only the production phase operations; exercise the very controller
	// main.cpp uses, not a replica of its sequencing/exception/readiness logic.
	struct Operations
	{
		Startup* startup = nullptr;
		std::vector<Phase> entered;
		bool enabled = true;
		bool listenOk = true;
		bool throwPhase = false;
		Phase failure = Phase::Configuration;
		bool throwReport = false;
		bool throwQuarantine = false;
		bool reenterListen = false;
		bool published = false;
		bool quarantined = false;
		bool ownerRetained = false;
		bool escapedOwner = false;
		bool reportReady = false;
		bool hadException = false;
		std::string reportedError;
		Phase reportedPhase = Phase::Configuration;
		Outcome reportedOutcome = Outcome::AlreadyAttempted;
		Outcome nestedOutcome = Outcome::Ready;

		void Enter(Phase phase)
		{
			CHECK(!startup->Ready());
			entered.push_back(phase);
			if (throwPhase && phase == failure)
				throw std::runtime_error("injected required phase");
		}
		bool Configure() { Enter(Phase::Configuration); return enabled; }
		void Construct() { Enter(Phase::Construction); ownerRetained = true; }
		void Prepare() { escapedOwner = true; Enter(Phase::Preparation); }
		bool Listen()
		{
			Enter(Phase::Listening);
			if (reenterListen)
				nestedOutcome = startup->Run(*this);
			return listenOk;
		}
		void Publish() { Enter(Phase::Publication); published = true; }
		void Quarantine()
		{
			CHECK(!startup->Ready());
			quarantined = true;
			published = false;
			// Mirrors process-lifetime retention: never delete the escaped owner.
			if (throwQuarantine)
				throw std::runtime_error("injected quarantine failure");
		}
		void Report(Outcome outcome, Phase phase, std::exception_ptr error)
		{
			reportedOutcome = outcome;
			reportedPhase = phase;
			hadException = static_cast<bool>(error);
			if (error) {
				try { std::rethrow_exception(error); }
				catch (const std::exception& e) { reportedError = e.what(); }
			}
			reportReady = startup->Ready();
			if (throwReport)
				throw std::runtime_error("injected optional discovery/report failure");
		}
	};
}

TEST_CASE("PostLoad commits readiness only after every required phase")
{
	Startup startup;
	Operations ops{ &startup };
	CHECK(startup.Run(ops) == Outcome::Ready);
	CHECK(ops.entered == std::vector<Phase>({ Phase::Configuration, Phase::Construction,
		Phase::Preparation, Phase::Listening, Phase::Publication }));
	CHECK(ops.published);
	CHECK(ops.reportReady);
	CHECK(startup.Ready());
	const auto phases = ops.entered.size();
	CHECK(startup.Run(ops) == Outcome::AlreadyAttempted);
	CHECK(ops.entered.size() == phases);
}

TEST_CASE("disabled PostLoad never constructs or publishes an owner")
{
	Startup startup;
	Operations ops{ &startup };
	ops.enabled = false;
	CHECK(startup.Run(ops) == Outcome::Disabled);
	CHECK(startup.Current() == Startup::State::Disabled);
	CHECK(ops.entered.size() == 1);
	CHECK(!ops.ownerRetained);
	CHECK(!ops.published);
	CHECK(!ops.quarantined);
	CHECK(!startup.Ready());
}

TEST_CASE("required phase throws cannot escape or run later PostLoad stages")
{
	const std::vector<Phase> phases{ Phase::Configuration, Phase::Construction,
		Phase::Preparation, Phase::Listening, Phase::Publication };
	for (std::size_t i = 0; i < phases.size(); ++i) {
		Startup startup;
		Operations ops{ &startup };
		ops.throwPhase = true;
		ops.failure = phases[i];
		CHECK(startup.Run(ops) == Outcome::Failed);
		CHECK(startup.Current() == Startup::State::Failed);
		CHECK(ops.entered.size() == i + 1);
		CHECK(ops.reportedPhase == phases[i]);
		CHECK(ops.hadException);
		CHECK(ops.quarantined);
		CHECK(!ops.published);
		CHECK(!startup.Ready());
		CHECK(startup.Run(ops) == Outcome::AlreadyAttempted);
		CHECK(ops.entered.size() == i + 1);
		if (i >= 2) {
			CHECK(ops.escapedOwner);
			CHECK(ops.ownerRetained);
		}
	}
}

TEST_CASE("false listener result denies publication without pretending it threw")
{
	Startup startup;
	Operations ops{ &startup };
	ops.listenOk = false;
	CHECK(startup.Run(ops) == Outcome::Failed);
	CHECK(ops.entered.size() == 4);
	CHECK(ops.reportedPhase == Phase::Listening);
	CHECK(!ops.hadException);
	CHECK(ops.ownerRetained);
	CHECK(ops.quarantined);
	CHECK(!ops.published);
	CHECK(!startup.Ready());
}

TEST_CASE("optional discovery or reporting failure cannot undo committed readiness")
{
	Startup startup;
	Operations ops{ &startup };
	ops.throwReport = true;
	CHECK(startup.Run(ops) == Outcome::Ready);
	CHECK(startup.Ready());
	CHECK(ops.published);
	CHECK(!ops.quarantined);
	CHECK(ops.reportReady);
}

TEST_CASE("failure reporting and quarantine exceptions cannot escape PostLoad")
{
	Startup startup;
	Operations ops{ &startup };
	ops.failure = Phase::Publication;
	ops.throwPhase = true;
	ops.throwReport = true;
	ops.throwQuarantine = true;
	CHECK(startup.Run(ops) == Outcome::Failed);
	CHECK(ops.quarantined);
	CHECK(ops.ownerRetained);
	CHECK(ops.reportedOutcome == Outcome::Failed);
	CHECK(!startup.Ready());
}

TEST_CASE("reentrant PostLoad does not repeat construction or irreversible publication")
{
	Startup startup;
	Operations ops{ &startup };
	ops.reenterListen = true;
	CHECK(startup.Run(ops) == Outcome::Ready);
	CHECK(ops.nestedOutcome == Outcome::AlreadyAttempted);
	CHECK(ops.entered.size() == 5);
	CHECK(startup.Ready());
}

TEST_CASE("loader preparation returns without worker phases and completes once on safe phase")
{
	Startup startup;
	Operations ops{ &startup };
	CHECK(startup.Prepare(ops) == Outcome::Prepared);
	CHECK(startup.Current() == Startup::State::Prepared);
	CHECK(!startup.Ready());
	CHECK(ops.entered == std::vector<Phase>({Phase::Configuration, Phase::Construction, Phase::Preparation}));
	CHECK(!ops.published);
	CHECK(startup.Complete(ops) == Outcome::Ready);
	CHECK(startup.Complete(ops) == Outcome::AlreadyAttempted);
}

TEST_CASE("failed safe-phase scheduling quarantines prepared owner without worker startup")
{
	Startup startup;
	Operations ops{ &startup };
	CHECK(startup.Prepare(ops) == Outcome::Prepared);
	CHECK(startup.SchedulingFailed(ops, {}) == Outcome::Failed);
	CHECK(ops.entered.size() == 3);
	CHECK(ops.quarantined);
	CHECK(startup.Complete(ops) == Outcome::AlreadyAttempted);
	CHECK(!startup.Ready());
}

TEST_CASE("deferred PostLoad accepts a cold start after ten simulated minutes only once")
{
	using namespace std::chrono_literals;
	Startup startup;
	Operations ops{ &startup };
	const auto queued = Startup::Clock::time_point{};
	const auto deadline = queued + Startup::kDeferredStartAllowance;
	CHECK(deadline == queued + 20min);
	CHECK(startup.Prepare(ops) == Outcome::Prepared);
	CHECK(startup.CompleteDeferred(ops, false, queued + 10min, deadline) == Outcome::Ready);
	CHECK(ops.entered == std::vector<Phase>({ Phase::Configuration, Phase::Construction,
		Phase::Preparation, Phase::Listening, Phase::Publication }));
	CHECK(startup.CompleteDeferred(ops, false, queued + 11min, deadline) == Outcome::AlreadyAttempted);
	CHECK(ops.entered.size() == 5);
}

TEST_CASE("deferred PostLoad quarantines at and beyond twenty simulated minutes")
{
	using namespace std::chrono_literals;
	const auto queued = Startup::Clock::time_point{};
	const auto deadline = queued + Startup::kDeferredStartAllowance;
	for (const auto now : { deadline, deadline + 1s }) {
		Startup startup;
		Operations ops{ &startup };
		CHECK(startup.Prepare(ops) == Outcome::Prepared);
		CHECK(startup.CompleteDeferred(ops, false, now, deadline) == Outcome::Failed);
		CHECK(ops.reportedPhase == Phase::Scheduling);
		CHECK(ops.reportedError.find("missed its scheduling deadline") != std::string::npos);
		CHECK(ops.quarantined && ops.ownerRetained && !ops.published);
		CHECK(ops.entered.size() == 3);
		CHECK(startup.CompleteDeferred(ops, false, now, deadline) == Outcome::AlreadyAttempted);
	}
}

TEST_CASE("deferred PostLoad reports inline re-entry separately and never starts workers")
{
	Startup startup;
	Operations ops{ &startup };
	const auto queued = Startup::Clock::time_point{};
	CHECK(startup.Prepare(ops) == Outcome::Prepared);
	CHECK(startup.CompleteDeferred(ops, true, queued, queued + Startup::kDeferredStartAllowance) == Outcome::Failed);
	CHECK(ops.reportedPhase == Phase::Scheduling);
	CHECK(ops.reportedError.find("ran inline during PostLoad") != std::string::npos);
	CHECK(ops.quarantined && ops.ownerRetained && !ops.published);
	CHECK(ops.entered.size() == 3);
}
