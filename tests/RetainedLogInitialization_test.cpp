#include "test_framework.h"
#include "RetainedLogInitialization.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
	struct Fixture
	{
		using Stage = dvb::RetainedLogStage;
		std::vector<Stage> calls;
		Stage failure{ Stage::ready };
		bool missingPath{ false };
		bool systemFailure{ false };
		bool unknownFailure{ false };
		bool published{ false };
		std::weak_ptr<int> sinkLifetime;
		void Enter(Stage stage)
		{
			calls.push_back(stage);
			if (stage != failure) return;
			if (systemFailure) throw std::system_error(std::make_error_code(std::errc::permission_denied), "private path");
			if (unknownFailure) throw 1;
			throw std::runtime_error("private path or peer text must not enter the result");
		}
		dvb::RetainedLogResult Run()
		{
			return dvb::InitializeRetainedLog(
				[&]() -> std::optional<int> { Enter(Stage::path); if (missingPath) return {}; return 7; },
				[&](int path) { Enter(Stage::sink); CHECK(path == 7); auto sink = std::make_shared<int>(path); sinkLifetime = sink; return sink; },
				[&](auto sink) { Enter(Stage::logger); CHECK(!published); CHECK(*sink == 7); return sink; },
				[&](auto logger) { Enter(Stage::publish); CHECK(*logger == 7); published = true; });
		}
	};
}

TEST_CASE("retained logger resolves and configures once before publication")
{
	Fixture fixture;
	const auto result = fixture.Run();
	CHECK(result.Ready());
	CHECK(result.error == dvb::RetainedLogError::none);
	CHECK(result.code == 0);
	CHECK(fixture.published);
	CHECK((fixture.calls == std::vector{ Fixture::Stage::path, Fixture::Stage::sink, Fixture::Stage::logger, Fixture::Stage::publish }));
}

TEST_CASE("missing retained path never opens or publishes a sink")
{
	Fixture fixture;
	fixture.missingPath = true;
	const auto result = fixture.Run();
	CHECK(!result.Ready());
	CHECK(result.stage == Fixture::Stage::path);
	CHECK(result.error == dvb::RetainedLogError::pathUnavailable);
	CHECK(fixture.calls.size() == 1);
	CHECK(!fixture.published);
}

TEST_CASE("each retained logging exception stops subsequent stages and cleans local ownership")
{
	for (const auto stage : { Fixture::Stage::path, Fixture::Stage::sink, Fixture::Stage::logger, Fixture::Stage::publish }) {
		Fixture fixture;
		fixture.failure = stage;
		const auto result = fixture.Run();
		CHECK(!result.Ready());
		CHECK(result.stage == stage);
		CHECK(result.error == dvb::RetainedLogError::exception);
		CHECK(result.code == 0);
		CHECK(fixture.calls.back() == stage);
		CHECK(fixture.calls.size() == static_cast<unsigned>(stage) + 1);
		CHECK(!fixture.published);
		CHECK(fixture.sinkLifetime.expired());
	}
}

TEST_CASE("retained system errors preserve only the numeric code and exact stage")
{
	Fixture fixture;
	fixture.failure = Fixture::Stage::sink;
	fixture.systemFailure = true;
	const auto result = fixture.Run();
	CHECK(result.stage == Fixture::Stage::sink);
	CHECK(result.error == dvb::RetainedLogError::system);
	CHECK(result.code == std::make_error_code(std::errc::permission_denied).value());
	CHECK(!fixture.published);
}

TEST_CASE("nonstandard retained initialization exceptions are contained")
{
	Fixture fixture;
	fixture.failure = Fixture::Stage::logger;
	fixture.unknownFailure = true;
	const auto result = fixture.Run();
	CHECK(!result.Ready());
	CHECK(result.stage == Fixture::Stage::logger);
	CHECK(result.error == dvb::RetainedLogError::unknown);
	CHECK(fixture.sinkLifetime.expired());
}

TEST_CASE("retained initialization diagnostic names are fixed non-peer strings")
{
	CHECK(dvb::RetainedLogStageName(Fixture::Stage::path) == "path");
	CHECK(dvb::RetainedLogStageName(Fixture::Stage::sink) == "sink");
	CHECK(dvb::RetainedLogStageName(Fixture::Stage::logger) == "logger");
	CHECK(dvb::RetainedLogStageName(Fixture::Stage::publish) == "publish");
	CHECK(dvb::RetainedLogStageName(Fixture::Stage::ready) == "ready");
	CHECK(dvb::RetainedLogErrorName(dvb::RetainedLogError::pathUnavailable) == "path_unavailable");
	CHECK(dvb::RetainedLogErrorName(dvb::RetainedLogError::system) == "system_error");
	CHECK(dvb::RetainedLogErrorName(dvb::RetainedLogError::exception) == "exception");
	CHECK(dvb::RetainedLogErrorName(dvb::RetainedLogError::unknown) == "unknown_exception");
}
